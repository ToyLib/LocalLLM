"""会話の状態と、ワーカースレッドでの送受信"""

from __future__ import annotations

import enum
import threading
from dataclasses import dataclass
from typing import Callable

import rag
from agent import create_assistant, create_samurai, route_question, run_agent
from llm_client import CancelToken


class RagMode(enum.Enum):
    """質問に rag/ の資料を添えるか"""

    AUTO = "auto"      # ルーターが質問ごとに判断する
    ALWAYS = "always"
    NEVER = "never"


@dataclass
class Message:
    role: str
    content: str = ""

    # 回答の下に表示する補足 (RAG で参照した資料など)
    note: str = ""

    # 回答したエージェントの名前 (assistant のみ)
    agent: str = ""


class ChatState:
    """UIスレッドとワーカースレッドで共有する会話状態"""

    def __init__(self, notify: Callable[[], None]) -> None:
        self.lock = threading.Lock()

        # system プロンプトを除いた会話 (user / assistant)
        self.messages: list[Message] = []

        self.busy = False
        self.error = ""

        # 失敗時に入力欄へ戻す質問
        self.restored_prompt = ""

        self.cancel = CancelToken()
        self.worker: threading.Thread | None = None

        # 質問に rag/ の資料を添えるか (UIスレッドだけが触る)
        self.rag_mode = RagMode.AUTO

        # UIスレッドに再描画を頼む (どのスレッドからでも呼べる)
        self.notify = notify


def _format_sources(hits: list[rag.RagHit]) -> str:
    """参照した資料の一覧 (回答の下に表示する)"""

    if not hits:
        return "参照: 関係のある資料は見つかりませんでした"

    note = "参照:"

    for i, hit in enumerate(hits):
        note += f"\n  [{i + 1}] {hit.source}"

        if hit.heading:
            note += f" ({hit.heading})"

        note += f" {hit.score:.2f}"

    return note


def _decide_rag(
    rag_state: rag.RagState,
    history: list[dict[str, str]],
    prompt: str,
    cancel: CancelToken,
) -> tuple[bool, str]:
    """ルーターに資料が必要かを判断させ、(判断, note に書く文) を返す
    判断できなかったときは資料を参照する (ルーター導入前と同じ動き)"""

    sources = rag.describe_sources(rag_state)

    if not sources:
        return False, "ルーター: 資料が無いので参照しません"

    try:
        # 送信する履歴から今回の質問を除いたもの
        decision = route_question(history[:-1], prompt, sources, cancel)
    except Exception as e:
        if cancel.is_set():
            raise
        return True, f"ルーター: 判断できなかったので資料を参照します: {e}"

    note = "ルーター: " + (
        "資料を参照します" if decision.use_rag else "資料は参照しません")

    if decision.reason:
        note += f" ({decision.reason})"

    return decision.use_rag, note


def join_worker(state: ChatState) -> None:
    if state.worker is not None:
        state.worker.join()
        state.worker = None


def send_prompt(state: ChatState, rag_state: rag.RagState, prompt: str) -> None:
    """質問を送信してワーカースレッドで回答を受信する"""

    join_worker(state)

    with state.lock:
        state.messages.append(Message("user", prompt))

        # 履歴全体を送信 (system プロンプトは回答役のエージェントが付ける)
        history = [{"role": message.role, "content": message.content}
                   for message in state.messages]

        # ストリーミングで埋めていくAIの回答
        state.messages.append(Message("assistant"))

        state.busy = True
        state.error = ""

    state.cancel.reset()
    rag_mode = state.rag_mode

    def run() -> None:
        try:
            note = ""
            use_rag = rag_mode == RagMode.ALWAYS

            if rag_mode == RagMode.AUTO:
                use_rag, note = _decide_rag(
                    rag_state, history, prompt, state.cancel)

            # 資料を使う質問はアシスタント、使わない質問は侍が答える
            answerer = create_assistant() if use_rag else create_samurai()

            with state.lock:
                state.messages[-1].agent = answerer.name
                state.messages[-1].note = note
            state.notify()

            if use_rag:
                # 資料は送信する質問にだけ添え、画面の履歴には残さない
                # (ルーターの判断の下に、参照した資料を続ける)
                if note:
                    note += "\n"

                try:
                    hits = rag.search(rag_state, prompt, state.cancel)

                    if hits:
                        history[-1]["content"] = rag.build_prompt(hits, prompt)

                    note += _format_sources(hits)
                except Exception as e:
                    if state.cancel.is_set():
                        raise

                    # 検索できなくても、資料なしで回答は続ける
                    note += f"資料を参照できませんでした: {e}"

                with state.lock:
                    state.messages[-1].note = note
                state.notify()

            def on_delta(text: str) -> None:
                with state.lock:
                    state.messages[-1].content += text
                state.notify()

            run_agent(answerer, history, on_delta, state.cancel)
        except Exception as e:
            with state.lock:
                cancelled = state.cancel.is_set()
                keep_partial = cancelled and state.messages[-1].content

                if not keep_partial:
                    # 失敗した質問は履歴から取り除き、入力欄に戻す
                    del state.messages[-2:]
                    state.restored_prompt = prompt

                if not cancelled:
                    state.error = str(e)

        with state.lock:
            state.busy = False
        state.notify()

    state.worker = threading.Thread(target=run, daemon=True)
    state.worker.start()
