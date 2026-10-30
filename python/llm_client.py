"""OpenAI 互換 API へのストリーミング送信と SSE の解析、埋め込みの取得"""

from __future__ import annotations

import http.client
import json
import socket
import threading
from dataclasses import dataclass
from typing import Any, Callable
from urllib.parse import urlsplit

SERVER_URL = "http://127.0.0.1:8080/v1/chat/completions"

# RAG 用の埋め込みサーバー (llama-server --embeddings)
EMBED_URL = "http://127.0.0.1:8081/v1/embeddings"

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


def _post(
    url: str,
    request: dict[str, Any],
    headers: dict[str, str],
    cancel: CancelToken,
    connect_timeout: float | None,
) -> tuple[http.client.HTTPConnection, http.client.HTTPResponse]:
    parts = urlsplit(url)
    connection = http.client.HTTPConnection(
        parts.hostname, parts.port or 80, timeout=connect_timeout)

    try:
        connection.connect()

        # 接続した後は、回答を待つのでタイムアウトなし
        connection.sock.settimeout(None)
        cancel._attach(connection)

        body = json.dumps(request, ensure_ascii=False).encode("utf-8")
        connection.request(
            "POST", parts.path or "/", body=body,
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
        raise LLMError(json.dumps(data["error"], ensure_ascii=False))

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

    request: dict[str, Any] = {
        "model": "local",
        "messages": messages,
        "max_tokens": options.max_tokens,
        "stream": True,
    }

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

    try:
        connection, response = _post(
            SERVER_URL, request, {"Accept": "text/event-stream"},
            cancel, connect_timeout=None)

        if response.status == 200:
            for raw in response:
                line = raw.decode("utf-8").rstrip("\n").rstrip("\r")
                text = _parse_line(line, on_delta)

                if text is None:
                    done = True
                    break

                answer.append(text)
    except Exception as e:
        if cancel.is_set():
            raise Cancelled() from None
        if isinstance(e, LLMError):
            raise
        raise LLMError(str(e)) from e
    finally:
        if connection is not None:
            cancel._detach(connection)
            connection.close()

    if cancel.is_set():
        raise Cancelled()

    if response.status != 200:
        raise LLMError(f"HTTP {response.status}")

    if not done:
        raise LLMError("Stream ended before [DONE]")

    return "".join(answer)


def embed(inputs: list[str], cancel: CancelToken) -> list[list[float]]:
    """埋め込みサーバーで文章をベクトルに変換する (inputs と同じ順で返す)
    cancel されると Cancelled を投げる"""

    request = {"model": "embed", "input": inputs}
    connection = None

    try:
        # サーバーが起動していないときに待たされないように
        connection, response = _post(
            EMBED_URL, request, {}, cancel, connect_timeout=3)
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
