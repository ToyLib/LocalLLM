"""OpenAI 互換 API へのストリーミング送信と SSE の解析、埋め込みの取得"""

from __future__ import annotations

import http.client
import json
import socket
import threading
from dataclasses import dataclass
from typing import Any, Callable
from urllib.parse import urlsplit

import config

# サーバーの URL は config/server.json に書く
# chat_url: チャットのサーバー、embed_url: RAG 用の埋め込みサーバー (llama-server --embeddings)
SERVER_CONFIG = "server.json"

# 受信したテキスト片ごとに呼ばれる (ワーカースレッドから呼ばれる)
DeltaCallback = Callable[[str], None]


class LLMError(Exception):
    pass


class Cancelled(LLMError):
    def __init__(self) -> None:
        super().__init__("Cancelled")


class CancelToken:
    """中断ボタン用。cancel() で通信中のソケットも閉じて、受信待ちをすぐに抜ける"""

    def __init__(self) -> None:
        self._event = threading.Event()
        self._lock = threading.Lock()
        self._connections: set[http.client.HTTPConnection] = set()

    def cancel(self) -> None:
        self._event.set()

        with self._lock:
            connections = list(self._connections)

        for connection in connections:
            _shutdown(connection)

    def reset(self) -> None:
        self._event.clear()

    def is_set(self) -> bool:
        return self._event.is_set()

    def _attach(self, connection: http.client.HTTPConnection) -> None:
        with self._lock:
            self._connections.add(connection)

        # 接続中に中断されていたら、すぐに閉じる
        if self.is_set():
            _shutdown(connection)

    def _detach(self, connection: http.client.HTTPConnection) -> None:
        with self._lock:
            self._connections.discard(connection)


def _shutdown(connection: http.client.HTTPConnection) -> None:
    sock = connection.sock

    if sock is not None:
        try:
            sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass


@dataclass
class LLMOptions:
    """1回の問い合わせごとに変えられる設定"""

    max_tokens: int = 1024

    # None ならサーバーの既定値を使う
    temperature: float | None = None

    # JSON スキーマ (None 以外なら、回答をこの形の JSON に制限する)
    schema: dict[str, Any] | None = None

    # 考える過程を出すモデル (Qwen3 など) で、考えずに答えさせる
    disable_thinking: bool = False

    # True ならローカルの LLM ではなく、クラウド (ChatGPT など、server.json の cloud) に送る
    # クラウドには max_tokens / temperature / disable_thinking を送らない
    # (OpenAI の推論モデルは temperature を受け付けず、llama-server 専用の項目も通らないため)
    cloud: bool = False


@dataclass
class CloudSettings:
    """クラウドの LLM の設定 (config/server.json の cloud。書いていない項目は既定値)"""

    # 画面に出す名前
    name: str = "ChatGPT"

    chat_url: str = "https://api.openai.com/v1/chat/completions"
    model: str = "gpt-5-mini"

    # API キーを探す名前 (環境変数 → 秘密ファイル。値は設定ファイルに書かない)
    api_key_name: str = "OPENAI_API_KEY"

    # 認証ヘッダーの形式: "bearer" (Authorization: Bearer) か "api-key" (Azure OpenAI)
    auth: str = "bearer"

    # 0 なら送らない (サーバーの既定値)
    max_completion_tokens: int = 0

    # 推論の深さ (minimal / low / medium / high)。空なら送らない
    reasoning_effort: str = ""


def load_cloud_settings() -> CloudSettings:
    """server.json を読んで、クラウドの設定を返す (読めなければ既定値)"""

    settings = CloudSettings()
    cloud = config.load_or_empty(SERVER_CONFIG).get("cloud")

    if cloud is None:
        return settings

    if not isinstance(cloud, dict):
        print("Config cloud is not an object (using defaults)")
        return settings

    for key in ("name", "chat_url", "model", "api_key_name", "auth", "reasoning_effort"):
        value = cloud.get(key)
        if isinstance(value, str):
            setattr(settings, key, value)

    tokens = cloud.get("max_completion_tokens")
    if isinstance(tokens, (int, float)) and not isinstance(tokens, bool):
        settings.max_completion_tokens = int(tokens)

    return settings


def has_cloud_api_key(settings: CloudSettings) -> bool:
    """クラウドの API キーが見つかるか (環境変数か秘密ファイル)"""

    return bool(config.find_api_key(settings.api_key_name)[0])


def _error_message(body: bytes) -> str:
    """エラーの本文 ({"error": {"message": ...}}) からメッセージを取り出す"""

    try:
        data = json.loads(body)
    except ValueError:
        return ""

    error = data.get("error") if isinstance(data, dict) else None

    if isinstance(error, dict) and isinstance(error.get("message"), str):
        return error["message"]

    return error if isinstance(error, str) else ""


def _http_error(status: int, body: bytes, server: str) -> str:
    """HTTP のエラーを、分かりやすいメッセージにする"""

    text = f"{server}: HTTP {status}"

    if status == 401:
        text += " (API キーが無効です)"
    elif status == 429:
        text += " (回数の制限か、残高不足です)"

    if message := _error_message(body):
        text += f": {message}"

    return text


def _post(
    url: str,
    request: dict[str, Any],
    headers: dict[str, str],
    cancel: CancelToken,
    connect_timeout: float | None,
) -> tuple[http.client.HTTPConnection, http.client.HTTPResponse]:
    parts = urlsplit(url)

    if parts.scheme == "https":
        connection: http.client.HTTPConnection = http.client.HTTPSConnection(
            parts.hostname, parts.port or 443, timeout=connect_timeout)
    else:
        connection = http.client.HTTPConnection(
            parts.hostname, parts.port or 80, timeout=connect_timeout)

    # クエリ (Azure OpenAI の ?api-version= など) も送る
    target = parts.path or "/"

    if parts.query:
        target += "?" + parts.query

    try:
        connection.connect()

        # 接続した後は、回答を待つのでタイムアウトなし
        connection.sock.settimeout(None)
        cancel._attach(connection)

        body = json.dumps(request, ensure_ascii=False).encode("utf-8")
        connection.request(
            "POST", target, body=body,
            headers={"Content-Type": "application/json", **headers})

        return connection, connection.getresponse()
    except BaseException:
        cancel._detach(connection)
        connection.close()
        raise


def _parse_line(line: str, on_delta: DeltaCallback | None) -> str | None:
    """SSE の1行を処理する。受信したテキスト片を返す ([DONE] なら None)"""

    if not line or line.startswith(":") or not line.startswith("data:"):
        return ""

    line = line[5:]

    if line.startswith(" "):
        line = line[1:]

    if line == "[DONE]":
        return None

    if not line:
        return ""

    try:
        data = json.loads(line)
    except json.JSONDecodeError:
        raise LLMError("Invalid SSE JSON") from None

    if not isinstance(data, dict):
        return ""

    if "error" in data:
        error = data["error"]
        if isinstance(error, dict) and isinstance(error.get("message"), str):
            raise LLMError(error["message"])
        raise LLMError(json.dumps(error, ensure_ascii=False))

    choices = data.get("choices")

    if not isinstance(choices, list) or not choices:
        return ""

    delta = choices[0].get("delta")

    if not isinstance(delta, dict):
        return ""

    text = delta.get("content")

    if not isinstance(text, str):
        return ""

    if on_delta:
        on_delta(text)

    return text


def ask_llm(
    messages: list[dict[str, str]],
    on_delta: DeltaCallback | None,
    cancel: CancelToken,
    options: LLMOptions | None = None,
) -> str:
    """LLM に会話履歴を送信し、ストリーミングで回答を受け取る
    cancel されると Cancelled を投げる"""

    options = options or LLMOptions()

    request: dict[str, Any] = {"messages": messages, "stream": True}
    headers = {"Accept": "text/event-stream"}

    if options.cloud:
        cloud = load_cloud_settings()
        url = cloud.chat_url
        server = cloud.name
        key, _ = config.find_api_key(cloud.api_key_name)

        if not key:
            raise LLMError(
                f"{cloud.name} の API キー ({cloud.api_key_name}) が設定されていません")

        # キーを平文の通信やローカルのサーバーに送らないように
        if urlsplit(url).scheme != "https":
            raise LLMError(f"API キーは https の URL にだけ送ります: {url}")

        if cloud.auth == "api-key":
            headers["api-key"] = key
        else:
            headers["Authorization"] = f"Bearer {key}"

        request["model"] = cloud.model

        if cloud.max_completion_tokens > 0:
            request["max_completion_tokens"] = cloud.max_completion_tokens

        if cloud.reasoning_effort:
            request["reasoning_effort"] = cloud.reasoning_effort

        if options.schema is not None:
            request["response_format"] = {
                "type": "json_schema",
                "json_schema": {
                    "name": "result", "strict": True, "schema": options.schema},
            }
    else:
        url = config.get_string(SERVER_CONFIG, "chat_url")
        server = "LLM server"

        # llama-server はモデル名を見ない
        request["model"] = "local"
        request["max_tokens"] = options.max_tokens

        if options.temperature is not None:
            request["temperature"] = options.temperature

        if options.schema is not None:
            # llama-server は文法で出力を縛るので、小さいモデルでも形が崩れない
            request["response_format"] = {
                "type": "json_schema",
                "json_schema": {"name": "result", "schema": options.schema},
            }

        if options.disable_thinking:
            request["chat_template_kwargs"] = {"enable_thinking": False}

    answer: list[str] = []
    done = False
    connection = None
    error_body = b""

    try:
        connection, response = _post(
            url, request, headers, cancel, connect_timeout=None)

        if response.status == 200:
            for raw in response:
                line = raw.decode("utf-8").rstrip("\n").rstrip("\r")
                text = _parse_line(line, on_delta)

                if text is None:
                    done = True
                    break

                answer.append(text)
        else:
            # エラーの本文 (SSE ではない JSON) を読んでおく
            error_body = response.read(64 * 1024)
    except Exception as e:
        if cancel.is_set():
            raise Cancelled() from None
        if isinstance(e, LLMError):
            raise
        raise LLMError(f"{server}: {e}") from e
    finally:
        if connection is not None:
            cancel._detach(connection)
            connection.close()

    if cancel.is_set():
        raise Cancelled()

    if response.status != 200:
        raise LLMError(_http_error(response.status, error_body, server))

    if not done:
        raise LLMError("Stream ended before [DONE]")

    return "".join(answer)


def embed(inputs: list[str], cancel: CancelToken) -> list[list[float]]:
    """埋め込みサーバーで文章をベクトルに変換する (inputs と同じ順で返す)
    cancel されると Cancelled を投げる"""

    url = config.get_string(SERVER_CONFIG, "embed_url")
    request = {"model": "embed", "input": inputs}
    connection = None

    try:
        # サーバーが起動していないときに待たされないように
        connection, response = _post(
            url, request, {}, cancel, connect_timeout=3)
        body = response.read()
    except Exception as e:
        if cancel.is_set():
            raise Cancelled() from None
        raise LLMError(f"Embedding server: {e}") from e
    finally:
        if connection is not None:
            cancel._detach(connection)
            connection.close()

    if cancel.is_set():
        raise Cancelled()

    if response.status != 200:
        raise LLMError(f"Embedding server: HTTP {response.status}")

    try:
        data = json.loads(body)["data"]
        if not isinstance(data, list) or len(data) != len(inputs):
            raise ValueError

        # index の順に並べ直す
        vectors: list[list[float] | None] = [None] * len(inputs)

        for item in data:
            index = item.get("index", 0)
            if not 0 <= index < len(vectors) or "embedding" not in item:
                raise ValueError
            vectors[index] = [float(v) for v in item["embedding"]]

        if any(vector is None for vector in vectors):
            raise ValueError
    except (ValueError, KeyError, TypeError, AttributeError):
        raise LLMError("Invalid embedding response") from None

    return vectors  # type: ignore[return-value]
