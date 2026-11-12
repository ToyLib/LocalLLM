"""会話の状態と、ワーカースレッドでの送受信"""

from __future__ import annotations

import enum
import threading
import time
from dataclasses import dataclass, field
from typing import Callable

import rag
from agent import (
    compare_answers, create_answerer, create_assistant, create_cloud, create_general,
    rewrite_answer, route_question, run_agent)
from llm_client import (
    CancelToken, Citation, LLMError, has_cloud_api_key, load_cloud_settings,
    number_citations)


class RagMode(enum.Enum):
    """質問に rag/ の資料を添えるか"""

    AUTO = "auto"      # ルーターが質問ごとに判断する
    ALWAYS = "always"
    NEVER = "never"


# 回答を作った段階 (ローカル → ChatGPT → ChatGPT + Web 検索)
STAGE_LOCAL = "local"
STAGE_CLOUD = "cloud"
STAGE_WEB = "web"


@dataclass
class Message:
    role: str
    content: str = ""

    # 回答の下に表示する補足 (RAG で参照した資料など)
    note: str = ""

    # 回答したエージェントの名前 (assistant のみ)
    agent: str = ""

    # 回答役のキー (assistant / general。ChatGPT に聞き直したときの書き直しに使う)
    agent_key: str = ""

    # assistant: 資料 (rag/) の内容を使って答えたか (ChatGPT 宛ての履歴から除き、聞き直しも止める目印)
    used_rag: bool = False

    # assistant: ChatGPT に聞き直して置き換えられた回答 (LLM に送る履歴から除き、薄く表示する)
    superseded: bool = False

    # assistant: ChatGPT に聞き直した回答の、書き直す前の元の文章 (折りたたんで表示する)
    cloud_original: str = ""

    # assistant: 回答を作った段階
    stage: str = STAGE_LOCAL

    # assistant: Web 検索の出典 (本文の [1] などの番号の順)
    citations: list[Citation] = field(default_factory=list)

    # assistant: 聞き直す前の回答との違いのまとめ (まとめていなければ compare_verdict は空)
    compare_verdict: str = ""
    compare_points: list[str] = field(default_factory=list)

    # assistant: 回答の途中の状態 (「ChatGPT に問い合わせ中…」など。表示は format_status)
    status: str = ""
    status_since: float = 0.0

    # 今の段階で受け取った文字数と、目安の全体の文字数 (分からなければ 0)
    received_chars: int = 0
    total_chars: int = 0


# この秒数を過ぎたら、中断できることを添える
SLOW_SECONDS = 20


def _set_status(message: Message, status: str, total_chars: int = 0) -> None:
    """途中の状態を変える (呼ぶ側で state.lock を取っておく)"""

    message.status = status
    message.status_since = time.monotonic()
    message.received_chars = 0
    message.total_chars = total_chars


def format_status(message: Message) -> str:
    """回答の途中の状態に、受け取った文字数と経過時間を添えた文 (status が空なら空)"""

    if not message.status:
        return ""

    seconds = int(time.monotonic() - message.status_since)
    text = message.status

    if message.received_chars or message.total_chars:
        text += f" {message.received_chars:,} 文字"

        # 書き直すと長さが変わるので、元の長さは目安として添える
        if message.total_chars:
            text += f" (元の回答は {message.total_chars:,} 文字)"

    text += f" ({seconds} 秒)"

    # 本文がまだ無いときだけ (違いのまとめを作っている間は、止めても回答は残る)
    if seconds >= SLOW_SECONDS and not message.received_chars and not message.content:
        text += "\n時間がかかっています。中断ボタンで止めると、元の回答に戻ります。"

    return text


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

        # クラウド (ChatGPT) の API キーがあるか (UIスレッドだけが触る。起動時と設定の保存時に調べ直す)
        self.has_cloud_key = False
        self.cloud_name = "ChatGPT"

        # 聞き直した後に Web 検索でも聞き直せるか (server.json の cloud.web_search。UIスレッドだけが触る)
        self.web_search = False

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
    """質問を送信してワーカースレッドで回答を受信する
    流れ: 履歴に質問と空の回答を積む → (ワーカー) 資料が要るかをルーターで判断
          → 資料を検索して質問に添える → 回答をストリーミングで受信
    失敗したら質問を履歴から除いて入力欄に戻す"""

    join_worker(state)

    with state.lock:
        state.messages.append(Message("user", prompt))

        # 履歴全体を送信 (system プロンプトは回答役のエージェントが付ける)
        # ChatGPT に聞き直して置き換えた回答は除く
        history = [{"role": message.role, "content": message.content}
                   for message in state.messages if not message.superseded]

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

            # 資料を使う質問はアシスタント、使わない質問は汎用の回答役が答える
            answerer = create_assistant() if use_rag else create_general()

            with state.lock:
                state.messages[-1].agent = answerer.name
                state.messages[-1].agent_key = answerer.key
                state.messages[-1].note = note
            state.notify()

            if use_rag:
                # 資料は送信する質問にだけ添え、画面の履歴には残さない
                if note:
                    note += "\n"

                try:
                    hits = rag.search(rag_state, prompt, state.cancel)

                    if hits:
                        rag_prompt = rag.build_prompt(hits, prompt)
                        history[-1]["content"] = rag_prompt

                        # 資料を使ったやりとりを ChatGPT 宛ての履歴から除き、聞き直しを止めるための目印
                        with state.lock:
                            state.messages[-1].used_rag = True

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


def _last_user_index(messages: list[Message]) -> int | None:
    """最新の質問の位置 (無ければ None)"""

    for i in range(len(messages) - 1, -1, -1):
        if messages[i].role == "user":
            return i

    return None


def _build_cloud_history(
        messages: list[Message], last_user: int) -> list[dict[str, str]]:
    """ChatGPT に送る履歴を作る (呼ぶ側で state.lock を取っておく)
    最新の質問より前のやりとりのうち、資料を使っていないものだけを送る"""

    history: list[dict[str, str]] = []

    for i in range(last_user):
        if messages[i].role != "user":
            continue

        # この質問への今の回答 (聞き直して置き換えたものは除く)
        answer = None
        j = i + 1

        while j < last_user and messages[j].role != "user":
            if not messages[j].superseded:
                answer = messages[j]
            j += 1

        if answer is None or answer.used_rag or not answer.content:
            continue

        history.append({"role": "user", "content": messages[i].content})
        history.append({"role": "assistant", "content": answer.content})

    return history


def _compare_with_previous(
        state: ChatState, question: str, local_answer: str, cloud_answer: str,
        web_search: bool) -> None:
    """聞き直す前の回答と、クラウドの回答 (書き直す前) の違いをまとめて、
    最新の回答に添える (ワーカースレッドから呼ぶ)
    失敗しても回答はそのまま残し、補足に理由を出す。中断したら何も添えない"""

    if not local_answer.strip():
        return

    with state.lock:
        _set_status(
            state.messages[-1],
            "Web 検索なしの回答と比べ中…" if web_search else "聞き直す前の回答と比べ中…")
    state.notify()

    def on_delta(text: str) -> None:
        with state.lock:
            state.messages[-1].received_chars += len(text)
        state.notify()

    try:
        comparison = compare_answers(
            question, local_answer, cloud_answer, on_delta, state.cancel, web_search)
    except Exception as e:
        with state.lock:
            answer = state.messages[-1]
            answer.status = ""

            if not state.cancel.is_set():
                answer.note += f"\n違いのまとめを作れませんでした: {e}"
        return

    with state.lock:
        answer = state.messages[-1]
        answer.status = ""
        answer.compare_verdict = comparison.verdict
        answer.compare_points = comparison.points


def refresh_cloud_status(state: ChatState) -> None:
    """クラウドの API キーの有無と名前を調べ直す (UIスレッドから呼ぶ)"""

    cloud = load_cloud_settings()
    state.has_cloud_key = has_cloud_api_key(cloud)
    state.cloud_name = cloud.name
    state.web_search = cloud.web_search


def can_ask_cloud(state: ChatState) -> bool:
    """最新の回答を ChatGPT に聞き直せるか (呼ぶ側で state.lock を取っておく)"""

    if state.busy or not state.messages:
        return False

    last = state.messages[-1]

    return (last.role == "assistant" and bool(last.agent_key) and
            bool(last.content) and _last_user_index(state.messages) is not None)


def next_stage(state: ChatState, message: Message) -> str:
    """この回答の次に聞き直せる段階 (STAGE_CLOUD / STAGE_WEB。無ければ空)
    資料を使った回答は、資料の内容が検索語として外に出ないように Web 検索しない
    (呼ぶ側で state.lock を取っておく)"""

    if message.stage == STAGE_WEB:
        return ""

    if message.stage == STAGE_CLOUD and state.web_search and not message.used_rag:
        return STAGE_WEB

    return STAGE_CLOUD


def ask_cloud(state: ChatState, web_search: bool = False) -> None:
    """最新の質問を ChatGPT に聞き直し、最新の回答の回答役の口調で書き直して表示する
    web_search なら、Web 検索用のモデルで聞き直し、検索なしの ChatGPT の回答と比べる
    資料を使った回答は聞き直せない (資料を外に送らないため)。資料を使ったやりとりは履歴からも除く
    流れ: 今の回答を「置き換え済み」にして新しい回答を積む → (ワーカー) クラウドに問い合わせ
          → 回答役の口調でローカルの LLM が書き直す (rewrite のとき) → ローカルの回答との違いをまとめる
    クラウドに聞けなかったら、聞き直す前の回答に戻す"""

    join_worker(state)

    with state.lock:
        if not can_ask_cloud(state):
            return

        previous = state.messages[-1]

        if previous.used_rag:
            return

        if next_stage(state, previous) != (STAGE_WEB if web_search else STAGE_CLOUD):
            return

        last_user = _last_user_index(state.messages)
        assert last_user is not None
        user = state.messages[last_user]

        history = _build_cloud_history(state.messages, last_user)
        question = user.content
        agent_key = previous.agent_key

        # 口調を書き直した回答は、書き直す前の文章と比べる
        local_answer = previous.cloud_original or previous.content

        history.append({"role": "user", "content": user.content})

        previous.superseded = True
        answer = Message(
            "assistant",
            agent=f"{previous.agent} ({state.cloud_name})",
            agent_key=agent_key,
            used_rag=previous.used_rag,
            stage=STAGE_WEB if web_search else STAGE_CLOUD,
            note=(f"{state.cloud_name} に Web で調べて聞き直しました" if web_search else
                  f"{state.cloud_name} に聞き直しました"),
        )
        _set_status(
            answer,
            f"{state.cloud_name} が Web で検索中…" if web_search
            else f"{state.cloud_name} に問い合わせ中…")
        state.messages.append(answer)

        state.busy = True
        state.error = ""

    state.cancel.reset()

    def run() -> None:
        try:
            persona = create_answerer(agent_key)
            cloud = create_cloud(web_search)
            citations: list[Citation] = []
            cloud.options.citations = citations
            source = f"{cloud.name} + Web 検索" if web_search else cloud.name

            with state.lock:
                state.messages[-1].agent = f"{persona.name} ({source})"
            state.notify()

            # 画面には流さず、受け取った文字数だけを進み具合として出す
            def on_cloud_delta(text: str) -> None:
                with state.lock:
                    answer = state.messages[-1]

                    if not answer.received_chars:
                        # 経過時間は問い合わせた時から数える
                        since = answer.status_since
                        _set_status(answer, f"{cloud.name} から受信中…")
                        answer.status_since = since

                    answer.received_chars += len(text)
                state.notify()

            cloud_answer = run_agent(cloud, history, on_cloud_delta, state.cancel)

            if not cloud_answer.strip():
                raise LLMError(f"{cloud.name} の回答が空でした")

            if web_search:
                # 出典は本文から分けて、書き直しや比較に URL を渡さない
                cloud_answer, numbered = number_citations(cloud_answer, citations)

                with state.lock:
                    state.messages[-1].citations = numbered

            if not persona.rewrite:
                with state.lock:
                    answer = state.messages[-1]
                    answer.content = cloud_answer
                    answer.status = ""
            else:
                with state.lock:
                    answer = state.messages[-1]
                    answer.agent = f"{persona.name} ({source} → ローカルで書き直し)"
                    answer.cloud_original = cloud_answer
                    _set_status(
                        answer, f"{persona.name}が言い直し中…", len(cloud_answer))
                state.notify()

                def on_delta(text: str) -> None:
                    with state.lock:
                        state.messages[-1].content += text
                        state.messages[-1].received_chars += len(text)
                    state.notify()

                failure = ""

                try:
                    rewrite_answer(
                        persona, question, cloud_answer, on_delta, state.cancel)
                except Exception as e:
                    failure = "中断しました" if state.cancel.is_set() else str(e)

                with state.lock:
                    answer = state.messages[-1]
                    answer.status = ""

                    # 書き直せなかったら、ChatGPT の回答をそのまま出す
                    # (中断しても、途中まで書き直したものがあればそれを残す)
                    if not answer.content.strip():
                        answer.content = cloud_answer
                        answer.agent = f"{persona.name} ({source})"
                        answer.cloud_original = ""
                        answer.note += (
                            f"\n書き直せなかったので、{cloud.name} の回答をそのまま表示しています" +
                            (f": {failure}" if failure else ""))
                    elif failure and not state.cancel.is_set():
                        answer.note += f"\n書き直しが途中で止まりました: {failure}"

            if not state.cancel.is_set():
                _compare_with_previous(
                    state, question, local_answer, cloud_answer, web_search)
        except Exception as e:
            # ChatGPT に聞けなかった: 聞き直す前の回答に戻す
            with state.lock:
                state.messages.pop()

                if state.messages:
                    state.messages[-1].superseded = False

                if not state.cancel.is_set():
                    state.error = str(e)

        with state.lock:
            state.busy = False
        state.notify()

    state.worker = threading.Thread(target=run, daemon=True)
    state.worker.start()
