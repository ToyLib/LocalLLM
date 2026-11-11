"""コーディング練習: コードの生成・解説と、サブプロセスでの実行"""

from __future__ import annotations

import os
import re
import signal
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from typing import Callable

import config
from agent import Agent, run_agent
from llm_client import CancelToken, Cancelled, LLMError, LLMOptions

CODE_CONFIG = "code.json"

# code.json に無いときの既定値
DEFAULT_TIMEOUT_SECONDS = 10
DEFAULT_MAX_OUTPUT_BYTES = 20000
DEFAULT_MAX_TOKENS = 2048
DEFAULT_EXPLAIN_MAX_TOKENS = 1536

DEFAULT_GENERATE_PROMPT = (
    "あなたはプログラミングを教えるアシスタントです。"
    "ユーザーの依頼に合う Python 3 のプログラムを、標準ライブラリだけで、短く読みやすく書いてください。"
    "回答は ```python で始まるコードブロック 1 つだけにしてください。")

DEFAULT_EXPLAIN_PROMPT = (
    "あなたはプログラミングを教えるアシスタントです。"
    "渡された Python のコードを、初心者に向けて日本語で解説してください。"
    "全体の流れを 2〜3 文で書いてから、大事な部分だけを「N 行目: ...」の形で書いてください。")

# 実行したコードに渡す環境変数 (API キーなどを見せないため、これだけにする)
_PASSED_ENVIRONMENT = (
    "PATH", "HOME", "USERPROFILE", "LANG", "LC_ALL", "TMPDIR", "TEMP", "TMP",
    "SYSTEMROOT", "COMSPEC", "PATHEXT")

_CODE_BLOCK = re.compile(r"```[ \t]*[A-Za-z0-9_+-]*[ \t]*\r?\n(.*?)(?:```|\Z)", re.S)


@dataclass
class CodeSettings:
    timeout_seconds: float = DEFAULT_TIMEOUT_SECONDS
    max_output_bytes: int = DEFAULT_MAX_OUTPUT_BYTES
    max_tokens: int = DEFAULT_MAX_TOKENS
    explain_max_tokens: int = DEFAULT_EXPLAIN_MAX_TOKENS
    explain_persona: str = ""
    generate_prompt: str = DEFAULT_GENERATE_PROMPT
    explain_prompt: str = DEFAULT_EXPLAIN_PROMPT


def _number(entry: dict, key: str, default: float) -> float:
    value = entry.get(key, default)

    if isinstance(value, bool) or not isinstance(value, (int, float)) or value <= 0:
        return default

    return value


def _prompt(entry: dict, key: str, default: str) -> str:
    """文字列か、文字列の配列 (つなげて 1 つにする)"""

    value = entry.get(key)

    if isinstance(value, list) and value and all(isinstance(v, str) for v in value):
        return "\n".join(value)

    if isinstance(value, str) and value:
        return value

    return default


def load_settings() -> CodeSettings:
    """config/code.json を読む (無い・正しくない項目は既定値)"""

    entry = config.load_or_empty(CODE_CONFIG)
    persona = entry.get("explain_persona", "")

    return CodeSettings(
        timeout_seconds=_number(entry, "timeout_seconds", DEFAULT_TIMEOUT_SECONDS),
        max_output_bytes=int(_number(entry, "max_output_bytes", DEFAULT_MAX_OUTPUT_BYTES)),
        max_tokens=int(_number(entry, "max_tokens", DEFAULT_MAX_TOKENS)),
        explain_max_tokens=int(_number(entry, "explain_max_tokens", DEFAULT_EXPLAIN_MAX_TOKENS)),
        explain_persona=persona if isinstance(persona, str) else "",
        generate_prompt=_prompt(entry, "generate_prompt", DEFAULT_GENERATE_PROMPT),
        explain_prompt=_prompt(entry, "explain_prompt", DEFAULT_EXPLAIN_PROMPT),
    )


def extract_code(text: str) -> str:
    """回答からコードブロックの中身を取り出す
    ブロックが閉じていなくても (受信の途中でも) 読めた所までを返す
    ブロックが無ければ、回答の全体をコードとみなす"""

    match = _CODE_BLOCK.search(text)

    if match:
        return match.group(1).rstrip("\n")

    # 受信し始めで、まだ改行まで届いていない開き ``` だけのとき
    if text.lstrip().startswith("```"):
        return ""

    return text.strip("\n")


def numbered(code: str) -> str:
    """解説を頼むときに渡す、行番号付きのコード"""

    return "\n".join(
        f"{number}: {line}" for number, line in enumerate(code.split("\n"), 1))


@dataclass
class CodeState:
    """コーディング練習タブの状態 (ワーカースレッドと UI スレッドで共有する)"""

    notify: Callable[[], None]
    lock: threading.Lock = field(default_factory=threading.Lock)
    cancel: CancelToken = field(default_factory=CancelToken)

    # 最後に頼んだこと (修正の頼みのとき、いまのコードと一緒に送る)
    request: str = ""

    code: str = ""

    # コードを LLM の生成や「元に戻す」で書き換えるたびに増やす (画面のコード欄に反映する目印)
    code_version: int = 0
    original_code: str = ""
    explanation: str = ""
    output: str = ""

    # 作っている / 解説している / 実行している
    generating: bool = False
    explaining: bool = False
    running: bool = False

    # 表示するエラー (空なら無し) と、実行の結果の一言
    error: str = ""
    run_status: str = ""

    # code を explanation のために使った時点のコード (編集されたかの目印)
    explained_code: str = ""

    # 生成・解説のスレッドと、実行中のプロセス
    worker: threading.Thread | None = None
    process: subprocess.Popen | None = None

    @property
    def busy(self) -> bool:
        return self.generating or self.explaining


def _workspace() -> str:
    directory = config.user_data_directory() / "workspace"
    directory.mkdir(parents=True, exist_ok=True)
    return str(directory)


def _persona_prompt(key: str) -> str:
    """explain_persona (agents.json のキー) の口調の指示 (無ければ空)"""

    if not key:
        return ""

    entry = config.load_or_empty("agents.json").get(key)

    if not isinstance(entry, dict):
        return ""

    prompt = entry.get("system_prompt")

    if isinstance(prompt, list) and all(isinstance(v, str) for v in prompt):
        prompt = "".join(prompt)

    if not isinstance(prompt, str) or not prompt:
        return ""

    return ("\n\n解説の口調は、次の役の指示に従ってください。"
            "ただし、解説の形 (全体の流れとポイント) は変えないでください。\n" + prompt)


def start_generate(state: CodeState, prompt: str) -> None:
    """プロンプトからコードを作り、続けて解説を作る (別スレッド)"""

    with state.lock:
        if state.busy or state.running:
            return

        state.generating = True
        state.error = ""
        state.request = prompt
        current = state.code.strip()
        state.cancel.reset()

    state.worker = threading.Thread(
        target=_generate, args=(state, prompt, current), daemon=True)
    state.worker.start()
    state.notify()


def start_explain(state: CodeState) -> None:
    """いまのコード (編集後でも) の解説を作り直す (別スレッド)"""

    with state.lock:
        if state.busy or state.running or not state.code.strip():
            return

        state.explaining = True
        state.error = ""
        state.cancel.reset()

    state.worker = threading.Thread(target=_explain, args=(state,), daemon=True)
    state.worker.start()
    state.notify()


def _generate(state: CodeState, prompt: str, current: str) -> None:
    settings = load_settings()
    started = False

    try:
        # 直前のコードがあれば、それを直す頼みとして送る
        if current:
            content = f"いまのコード:\n```python\n{current}\n```\n\n依頼: {prompt}"
        else:
            content = prompt

        agent = Agent(
            name="コード生成", system_prompt=settings.generate_prompt,
            options=LLMOptions(max_tokens=settings.max_tokens, temperature=0.2,
                               disable_thinking=True))
        raw: list[str] = []

        def on_delta(delta: str) -> None:
            nonlocal started
            raw.append(delta)
            code = extract_code("".join(raw))

            with state.lock:
                if not started:
                    # 受信が始まったら、前のコードと解説と実行結果を消す
                    started = True
                    state.explanation = ""
                    state.output = ""
                    state.run_status = ""

                state.code = code
                state.code_version += 1

            state.notify()

        answer = run_agent(
            agent, [{"role": "user", "content": content}], on_delta, state.cancel)
        code = extract_code(answer)

        with state.lock:
            state.code = code
            state.code_version += 1
            state.original_code = code
            state.generating = False

        state.notify()

        if not code.strip():
            raise LLMError("コードが返ってきませんでした")

        _explain(state)
        return
    except Cancelled:
        error = ""
    except LLMError as e:
        error = str(e)
    except Exception as e:  # ワーカーが黙って終わらないように
        error = f"想定外のエラー: {e}"

    with state.lock:
        state.generating = False
        state.error = error

    state.notify()


def _explain(state: CodeState) -> None:
    settings = load_settings()

    with state.lock:
        code = state.code
        state.explaining = True
        state.explanation = ""

    state.notify()
    error = ""

    try:
        agent = Agent(
            name="解説",
            system_prompt=settings.explain_prompt + _persona_prompt(settings.explain_persona),
            options=LLMOptions(max_tokens=settings.explain_max_tokens, temperature=0.3,
                               disable_thinking=True))
        raw: list[str] = []

        def on_delta(delta: str) -> None:
            raw.append(delta)

            with state.lock:
                state.explanation = "".join(raw)

            state.notify()

        content = ("次のコードを解説してください。各行の頭の「番号:」は行番号で、"
                   "コードの一部ではありません。\n\n" + numbered(code))
        answer = run_agent(
            agent, [{"role": "user", "content": content}], on_delta, state.cancel)

        with state.lock:
            state.explanation = answer.strip()
            state.explained_code = code
    except Cancelled:
        pass
    except LLMError as e:
        error = str(e)
    except Exception as e:
        error = f"想定外のエラー: {e}"

    with state.lock:
        state.explaining = False
        state.error = error

    state.notify()


def cancel_generate(state: CodeState) -> None:
    state.cancel.cancel()


def revert(state: CodeState) -> None:
    """コードを生成直後の状態に戻す"""

    with state.lock:
        if state.busy or state.running:
            return

        state.code = state.original_code
        state.code_version += 1

    state.notify()


def clear(state: CodeState) -> None:
    """コード・解説・実行結果・エラーを消して、最初の状態に戻す (作っている間と実行中は何もしない)"""

    with state.lock:
        if state.busy or state.running:
            return

        state.request = ""
        state.code = ""
        state.original_code = ""
        state.explanation = ""
        state.explained_code = ""
        state.output = ""
        state.run_status = ""
        state.error = ""
        state.code_version += 1

    state.notify()


def start_run(state: CodeState, code: str) -> None:
    """コードを実行する (別スレッド)。画面のコード欄の内容をそのまま実行する"""

    with state.lock:
        if state.busy or state.running or not code.strip():
            return

        state.code = code
        state.running = True
        state.output = ""
        state.run_status = "実行中…"
        state.error = ""

    threading.Thread(target=_run, args=(state, code), daemon=True).start()
    state.notify()


def stop_run(state: CodeState) -> None:
    with state.lock:
        process = state.process

    if process is not None:
        _kill(process)


def _kill(process: subprocess.Popen) -> None:
    """プロセスと、そこから起動された子プロセスを止める"""

    try:
        if sys.platform == "win32":
            process.kill()
        else:
            os.killpg(process.pid, signal.SIGKILL)
    except (ProcessLookupError, PermissionError, OSError):
        pass


def _run(state: CodeState, code: str) -> None:
    settings = load_settings()
    timed_out = False
    truncated = False
    started = time.monotonic()

    try:
        directory = _workspace()
        path = os.path.join(directory, "main.py")

        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(code + "\n")

        env = {k: os.environ[k] for k in _PASSED_ENVIRONMENT if k in os.environ}
        env["PYTHONIOENCODING"] = "utf-8"
        env["PYTHONUNBUFFERED"] = "1"

        kwargs = ({"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP}
                  if sys.platform == "win32" else {"start_new_session": True})

        # -I: 環境変数の PYTHON* とユーザーの site-packages を読まない
        process = subprocess.Popen(
            [sys.executable, "-I", "main.py"], cwd=directory, env=env,
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, **kwargs)

        with state.lock:
            state.process = process

        def on_timeout() -> None:
            nonlocal timed_out
            timed_out = True
            _kill(process)

        timer = threading.Timer(settings.timeout_seconds, on_timeout)
        timer.start()
        received = 0
        chunks: list[str] = []
        assert process.stdout is not None

        try:
            while True:
                data = process.stdout.read1(4096)

                if not data:
                    break

                received += len(data)

                if received > settings.max_output_bytes:
                    truncated = True
                    data = data[:max(len(data) - (received - settings.max_output_bytes), 0)]
                    _kill(process)

                chunks.append(data.decode("utf-8", errors="replace"))

                with state.lock:
                    state.output = "".join(chunks)

                state.notify()

                if truncated:
                    break
        finally:
            timer.cancel()

        code_returned = process.wait()
        elapsed = time.monotonic() - started

        if timed_out:
            status = f"{settings.timeout_seconds:g} 秒たったので止めました"
        elif truncated:
            status = "出力が多すぎるので止めました"
        elif code_returned == 0:
            status = f"終了しました ({elapsed:.1f} 秒)"
        else:
            status = f"エラーで終了しました (終了コード {code_returned})"

        error = ""
    except OSError as e:
        status = ""
        error = f"実行できませんでした: {e}"

    with state.lock:
        state.process = None
        state.running = False
        state.run_status = status
        state.error = error

    state.notify()


def shutdown(state: CodeState) -> None:
    """アプリを閉じるとき: 通信と実行中のコードを止める"""

    state.cancel.cancel()
    stop_run(state)
