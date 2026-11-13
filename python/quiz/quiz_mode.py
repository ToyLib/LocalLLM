"""クイズ: 分野の読み込み、出題 (JSON)、採点、先読み、結果のメッセージ"""

from __future__ import annotations

import random
import threading
from dataclasses import dataclass, field
from typing import Callable

from common import config
from common import rag
from common.agent import Agent, create_answerer, run_agent, run_agent_json
from common.llm_client import CancelToken, Cancelled, LLMError, LLMOptions, load_cloud_settings

QUIZ_CONFIG = "quiz.json"

# 自作の分野 (ユーザーごとの設定フォルダー。同梱の quiz.json とは分けて、更新で消えないようにする)
CUSTOM_CONFIG = "quiz_custom.json"

# 自作の分野の名前・プロンプトの最大の長さ (文字)
MAX_NAME_LENGTH = 40
MAX_PROMPT_LENGTH = 2000

# 選択肢の数 (この版では 4 択だけ)
CHOICE_COUNT = 4

# 1 問を作るのを試す回数
MAX_ATTEMPTS = 3

# 既出の問題として次の出題に渡す件数
PREVIOUS_QUESTIONS = 10

# 振り返りでねぎらいに添える、間違えた問題の件数
COMMENT_MISSES = 3

# quiz.json に無いときの既定値
DEFAULT_QUESTION_COUNT = 10
DEFAULT_MAX_TOKENS = 1024
DEFAULT_COMMENT_PERSONA = "assistant"

DEFAULT_GENERATE_PROMPT = (
    "あなたはクイズの出題者です。指定された分野とトピック (または資料) から、4 択の問題を 1 問だけ作ってください。"
    "correct には正解の選択肢、wrong には誤りの選択肢を 3 つ、explanation には正解の理由を書いてください。"
    "explanation で選択肢の番号や記号 (1・A など) は使わず、内容を言葉で書いてください。")

DEFAULT_TOPICS_PROMPT = (
    "あなたはクイズの出題者です。指定されたお題について、4 択の問題の題材になる小さなトピックを、"
    "重ならないように指定された個数だけ挙げてください。1 つは短い名詞句にします。")

DEFAULT_RESULTS = (
    (100, "全問正解です。お見事でした！"),
    (80, "すばらしい成績です。よくがんばりました！"),
    (50, "いい調子です。間違えた問題を見直すと、さらに伸びます。"),
    (0, "おつかれさまでした。間違えたところが伸びしろです。もう一度挑戦してみましょう。"),
)

# 問題の JSON の形 (llama-server が、この形に制限して出力する)
# 正解の番号ではなく、正解の文と誤りの文を別々に出させる
# (小さなモデルは番号を 1 始まりで答えたり、解説の番号とずれたりするため。並べ替えはアプリがする)
QUESTION_SCHEMA = {
    "type": "object",
    "properties": {
        "question": {"type": "string"},
        "correct": {"type": "string"},
        "wrong": {
            "type": "array",
            "items": {"type": "string"},
            "minItems": CHOICE_COUNT - 1,
            "maxItems": CHOICE_COUNT - 1,
        },
        "explanation": {"type": "string"},
    },
    "required": ["question", "correct", "wrong", "explanation"],
}

# お題からトピックを作らせるときの JSON の形
TOPICS_SCHEMA = {
    "type": "object",
    "properties": {
        "topics": {"type": "array", "items": {"type": "string"}, "minItems": 1, "maxItems": 20},
    },
    "required": ["topics"],
}

PHASE_SELECT = "select"
PHASE_PLAYING = "playing"
PHASE_RESULT = "result"


@dataclass
class Category:
    """出題の分野 (config/quiz.json の categories の 1 件)"""

    key: str
    name: str

    # "llm" (モデルの知識から出題)、"rag" (資料から出題)、"custom" (自作。use_rag で決まる)
    source: str
    guide: str = ""
    topics: list[str] = field(default_factory=list)

    # 自作の分野で、資料 (rag/) から出題するか
    use_rag: bool = False

    # 自作の分野で、ローカルの LLM ではなくクラウド (ChatGPT など) で出題するか
    # 資料を使う分野でオンにすると、資料の内容の一部も外に送られる (画面で警告する)
    # ただし、設定の「資料 (rag/) の内容を送ってよい」がオフなら、資料を使う分野ではクラウドを使わない
    use_cloud: bool = False

    @property
    def custom(self) -> bool:
        return self.source == "custom"

    @property
    def cloud(self) -> bool:
        """出題をクラウドに頼むか"""

        if not (self.custom and self.use_cloud):
            return False

        return not self.from_rag or load_cloud_settings().allow_rag

    @property
    def from_rag(self) -> bool:
        """資料から出題するか (guide があれば、それに近い資料を選ぶ)"""

        return self.source == "rag" or (self.custom and self.use_rag)


@dataclass
class Question:
    text: str
    choices: list[str]

    # 正解の選択肢の番号 (シャッフルしたあとの choices での位置)
    answer: int
    explanation: str

    # 出題のトピックと、資料の出典 (無ければ空)
    topic: str = ""
    origin: str = ""


@dataclass
class QuizSettings:
    question_count: int = DEFAULT_QUESTION_COUNT
    max_tokens: int = DEFAULT_MAX_TOKENS
    comment_persona: str = DEFAULT_COMMENT_PERSONA
    generate_prompt: str = DEFAULT_GENERATE_PROMPT

    # 自作の分野で、お題から出題のトピックを作らせるプロンプト
    topics_prompt: str = DEFAULT_TOPICS_PROMPT

    # (正解率の下限 %, メッセージ)。下限の大きい順
    results: tuple[tuple[int, str], ...] = DEFAULT_RESULTS


def _positive_int(entry: dict, key: str, default: int) -> int:
    value = entry.get(key, default)

    if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
        return default

    return value


def _read_results(entry: dict) -> tuple[tuple[int, str], ...]:
    results = entry.get("results")

    if not isinstance(results, list):
        return DEFAULT_RESULTS

    parsed = []

    for item in results:
        if (isinstance(item, dict) and isinstance(item.get("text"), str)
                and isinstance(item.get("min_percent"), (int, float))
                and not isinstance(item.get("min_percent"), bool)):
            parsed.append((item["min_percent"], item["text"]))

    if not parsed:
        return DEFAULT_RESULTS

    return tuple(sorted(parsed, key=lambda r: r[0], reverse=True))


def load_settings() -> QuizSettings:
    """config/quiz.json を読む (無い・正しくない項目は既定値)"""

    entry = config.load_or_empty(QUIZ_CONFIG)
    prompt = entry.get("generate_prompt")

    if isinstance(prompt, list) and prompt and all(isinstance(v, str) for v in prompt):
        prompt = "\n".join(prompt)

    if not isinstance(prompt, str) or not prompt:
        prompt = DEFAULT_GENERATE_PROMPT

    topics_prompt = entry.get("topics_prompt")

    if isinstance(topics_prompt, list) and topics_prompt and all(
            isinstance(v, str) for v in topics_prompt):
        topics_prompt = "\n".join(topics_prompt)

    if not isinstance(topics_prompt, str) or not topics_prompt:
        topics_prompt = DEFAULT_TOPICS_PROMPT

    persona = entry.get("comment_persona", DEFAULT_COMMENT_PERSONA)

    return QuizSettings(
        question_count=_positive_int(entry, "question_count", DEFAULT_QUESTION_COUNT),
        max_tokens=_positive_int(entry, "max_tokens", DEFAULT_MAX_TOKENS),
        comment_persona=persona if isinstance(persona, str) else DEFAULT_COMMENT_PERSONA,
        generate_prompt=prompt,
        topics_prompt=topics_prompt,
        results=_read_results(entry),
    )


def load_categories() -> list[Category]:
    """config/quiz.json の分野の一覧 (正しくない項目は読み飛ばす)"""

    entries = config.load_or_empty(QUIZ_CONFIG).get("categories")
    categories: list[Category] = []

    if not isinstance(entries, list):
        return categories

    for entry in entries:
        if (not isinstance(entry, dict)
                or not isinstance(entry.get("key"), str)
                or not isinstance(entry.get("name"), str)
                or entry.get("source") not in ("llm", "rag")):
            continue

        topics = entry.get("topics", [])
        guide = entry.get("guide", "")
        categories.append(Category(
            key=entry["key"],
            name=entry["name"],
            source=entry["source"],
            guide=guide if isinstance(guide, str) else "",
            topics=[t for t in topics if isinstance(t, str) and t]
            if isinstance(topics, list) else [],
        ))

    return categories


def load_custom_categories() -> list[Category]:
    """自作の分野の一覧 (quiz_custom.json。無ければ空)
    ファイルが壊れているときは ConfigError (編集画面で上書きして中身を消さないため)"""

    entries = config.load_for_edit(CUSTOM_CONFIG).get("categories", [])
    categories: list[Category] = []

    if not isinstance(entries, list):
        return categories

    for entry in entries:
        if (not isinstance(entry, dict) or not isinstance(entry.get("name"), str)
                or not entry["name"].strip()):
            continue

        prompt = entry.get("prompt", "")
        categories.append(Category(
            key=f"custom{len(categories)}",
            name=entry["name"].strip()[:MAX_NAME_LENGTH],
            source="custom",
            guide=prompt.strip()[:MAX_PROMPT_LENGTH] if isinstance(prompt, str) else "",
            use_rag=entry.get("use_rag") is True,
            use_cloud=entry.get("use_cloud") is True,
        ))

    return categories


def save_custom_categories(categories: list[Category]) -> None:
    """自作の分野を保存する (ConfigError)"""

    config.save(CUSTOM_CONFIG, {"categories": [
        {"name": c.name, "prompt": c.guide, "use_rag": c.use_rag, "use_cloud": c.use_cloud}
        for c in categories]})


def tier_message(settings: QuizSettings, score: int, total: int) -> str:
    """点数の帯ごとの固定のメッセージ"""

    percent = 100 * score / total if total else 0

    for minimum, text in settings.results:
        if percent >= minimum:
            return text

    return settings.results[-1][1]


def validate_question(raw: dict) -> Question:
    """LLM が返した JSON を検証して、選択肢を並べ替えた問題にする
    正しくなければ LLMError"""

    text = raw.get("question")
    correct = raw.get("correct")
    wrong = raw.get("wrong")
    explanation = raw.get("explanation")

    if not isinstance(text, str) or not text.strip():
        raise LLMError("問題文がありません")

    if (not isinstance(correct, str) or not correct.strip()
            or not isinstance(wrong, list) or len(wrong) != CHOICE_COUNT - 1
            or not all(isinstance(c, str) and c.strip() for c in wrong)):
        raise LLMError("選択肢が正しくありません")

    choices = [correct.strip(), *(c.strip() for c in wrong)]

    if len(set(choices)) != CHOICE_COUNT:
        raise LLMError("選択肢が重複しています")

    # 正解は先頭に置かれているので、並べ替えて位置を覚える
    order = list(range(CHOICE_COUNT))
    random.shuffle(order)

    return Question(
        text=text.strip(),
        choices=[choices[i] for i in order],
        answer=order.index(0),
        explanation=explanation.strip() if isinstance(explanation, str) else "",
    )


class QuizState:
    """クイズ画面の状態 (ワーカースレッドと UI スレッドで共有する)"""

    def __init__(self, notify: Callable[[], None]) -> None:
        self.lock = threading.Lock()
        self.notify = notify

        # 開始・やめるたびに増やす。古いワーカーが新しい回に書き込まないための目印
        self.session = 0
        self.cancel = CancelToken()
        self.worker: threading.Thread | None = None
        self.comment_worker: threading.Thread | None = None

        self.phase = PHASE_SELECT
        self.category: Category | None = None

        # 出題する数 (作れなかった問題の分だけ減る)
        self.total = 0

        self.questions: list[Question] = []

        # 答えた選択肢の番号 (questions の先頭から)
        self.answers: list[int] = []
        self.index = 0
        self.answered = False

        self.generating = False
        self.error = ""

        # 結果のメッセージ (最初は固定文、LLM のひと言ができたら置き換える)
        self.comment = ""
        self.comment_pending = False

    # 呼ぶ側で lock を取っておく
    def score(self) -> int:
        return sum(1 for q, a in zip(self.questions, self.answers) if q.answer == a)

    def misses(self) -> list[tuple[int, Question, int]]:
        """間違えた問題 (問題の番号, 問題, 選んだ選択肢)"""

        return [(i, q, a) for i, (q, a) in enumerate(zip(self.questions, self.answers))
                if q.answer != a]

    def is_last(self) -> bool:
        """いまの問題が最後か (作れなかった分を除いた数で)"""

        return self.index + 1 >= self.total


def _new_session(state: QuizState) -> tuple[int, CancelToken]:
    """進行中の回を止めて、新しい回の目印と中断用のトークンを返す (lock の中で呼ぶ)"""

    state.cancel.cancel()
    state.session += 1
    state.cancel = CancelToken()
    return state.session, state.cancel


def start(state: QuizState, category: Category, rag_state: rag.RagState) -> None:
    """分野を決めて出題を始める (問題は別スレッドで 1 問ずつ作る)"""

    settings = load_settings()

    with state.lock:
        session, cancel = _new_session(state)
        state.phase = PHASE_PLAYING
        state.category = category
        state.total = settings.question_count
        state.questions = []
        state.answers = []
        state.index = 0
        state.answered = False
        state.generating = True
        state.error = ""
        state.comment = ""
        state.comment_pending = False

    state.worker = threading.Thread(
        target=_generate_all,
        args=(state, session, cancel, category, rag_state, settings), daemon=True)
    state.worker.start()
    state.notify()


def _expand_topics(
    settings: QuizSettings, category: Category, count: int, cancel: CancelToken,
) -> list[str]:
    """自作の分野のお題から、出題のトピックを作らせる (偏りと重複を防ぐ)
    作れなかったときは空のリスト (既出の問題を渡す方式だけで進める)"""

    agent = Agent(
        name="トピック", system_prompt=settings.topics_prompt,
        options=LLMOptions(
            max_tokens=settings.max_tokens, temperature=0.8,
            disable_thinking=True, schema=TOPICS_SCHEMA, cloud=category.cloud))

    try:
        raw = run_agent_json(
            agent, [{"role": "user", "content": f"お題: {category.guide}\n個数: {count}"}],
            cancel)
    except LLMError:
        return []

    topics = raw.get("topics")

    if not isinstance(topics, list):
        return []

    result: list[str] = []

    for topic in topics:
        if isinstance(topic, str) and topic.strip() and topic.strip() not in result:
            result.append(topic.strip())

    return result[:count]


def _plan(
    category: Category, rag_state: rag.RagState, count: int,
    settings: QuizSettings, cancel: CancelToken,
) -> list[tuple[str, rag.RagChunk | None]]:
    """出題ごとの (トピック, 資料のチャンク) の予定"""

    if category.from_rag:
        if category.guide:
            chunks = rag.top_chunks(rag_state, category.guide, count, cancel)
        else:
            chunks = rag.sample_chunks(rag_state, count)

        if not chunks:
            raise LLMError(
                "資料 (rag/) のインデックスがありません。資料を置いて、読み込みが終わってから試してください")

        return [("", chunk) for chunk in chunks]

    if category.custom:
        topics = _expand_topics(settings, category, count, cancel)
    else:
        topics = list(category.topics)
        random.shuffle(topics)

    if not topics:
        return [("", None)] * count

    # トピックが足りなければ循環させる
    return [(topics[i % len(topics)], None) for i in range(count)]


def _prompt(
    category: Category, topic: str, chunk: rag.RagChunk | None, previous: list[str],
) -> str:
    if chunk is not None:
        origin = chunk.source + (f" / {chunk.heading}" if chunk.heading else "")
        lines = [f"分野: {category.name}"]

        if category.custom and category.guide:
            lines.append(f"出題の方針: {category.guide}")

        lines += [f"資料 ({origin}):", chunk.text]
    else:
        lines = [f"分野: {category.name}"]

        if category.guide:
            lines.append(f"出題範囲: {category.guide}")

        if topic:
            lines.append(f"トピック: {topic}")

    if previous:
        lines.append("")
        lines.append("すでに出した問題 (これらと同じ内容は避けてください):")
        lines.extend(f"- {text}" for text in previous[-PREVIOUS_QUESTIONS:])

    return "\n".join(lines)


def _make_question(
    settings: QuizSettings, category: Category, topic: str,
    chunk: rag.RagChunk | None, previous: list[str], cancel: CancelToken,
) -> Question:
    agent = Agent(
        name="出題", system_prompt=settings.generate_prompt,
        options=LLMOptions(
            max_tokens=settings.max_tokens, temperature=0.8,
            disable_thinking=True, schema=QUESTION_SCHEMA, cloud=category.cloud))
    raw = run_agent_json(
        agent, [{"role": "user", "content": _prompt(category, topic, chunk, previous)}], cancel)
    question = validate_question(raw)
    question.topic = topic

    if chunk is not None:
        question.origin = chunk.source + (f" / {chunk.heading}" if chunk.heading else "")

    return question


def make_test_question(
    category: Category, rag_state: rag.RagState, cancel: CancelToken,
) -> Question:
    """分野の編集画面のテスト用に、1 問だけ作る (ワーカースレッドから呼ぶ)
    作れなければ LLMError、中断されたら Cancelled"""

    settings = load_settings()
    topic, chunk = _plan(category, rag_state, 1, settings, cancel)[0]
    error = LLMError("問題を作れませんでした")

    for _ in range(MAX_ATTEMPTS):
        try:
            return _make_question(settings, category, topic, chunk, [], cancel)
        except LLMError as e:
            error = e

    raise error


def _generate_all(
    state: QuizState, session: int, cancel: CancelToken, category: Category,
    rag_state: rag.RagState, settings: QuizSettings,
) -> None:
    """問題を 1 問ずつ作り続ける (ユーザーが解いている間に先へ進む)"""

    error = ""

    try:
        plan = _plan(category, rag_state, settings.question_count, settings, cancel)

        with state.lock:
            if state.session != session:
                return

            # 資料が少ないときは、あるだけにする
            state.total = len(plan)

        previous: list[str] = []

        for topic, chunk in plan:
            question = None

            for _ in range(MAX_ATTEMPTS):
                try:
                    question = _make_question(
                        settings, category, topic, chunk, previous, cancel)
                    break
                except Cancelled:
                    return
                except LLMError as e:
                    error = str(e)

            with state.lock:
                if state.session != session:
                    return

                if question is None:
                    # 作れなかった問題は飛ばして、数を減らす
                    state.total -= 1
                else:
                    state.questions.append(question)
                    previous.append(question.text)

            state.notify()
    except Cancelled:
        return
    except LLMError as e:
        error = str(e)
    except Exception as e:  # ワーカーが黙って終わらないように
        error = f"想定外のエラー: {e}"

    with state.lock:
        if state.session != session:
            return

        state.generating = False

        if not state.questions:
            # 1 問も作れなかったら、分野の選択に戻す
            state.phase = PHASE_SELECT
            state.error = error or "問題を作れませんでした"

    state.notify()


def answer(state: QuizState, choice: int) -> None:
    """いまの問題に答える"""

    with state.lock:
        if (state.phase != PHASE_PLAYING or state.answered
                or state.index >= len(state.questions)
                or not 0 <= choice < CHOICE_COUNT):
            return

        state.answers.append(choice)
        state.answered = True

    state.notify()


def next_question(state: QuizState) -> None:
    """次の問題に進む。最後なら結果を出す (次の問題がまだ作れていなければ何もしない)"""

    settings = load_settings()

    with state.lock:
        if state.phase != PHASE_PLAYING or not state.answered:
            return

        if state.is_last():
            _finish(state, settings)
            return

        if state.index + 1 < len(state.questions):
            state.index += 1
            state.answered = False

    state.notify()


def _finish(state: QuizState, settings: QuizSettings) -> None:
    """結果にする (lock の中で呼ぶ)。固定のメッセージをすぐ出し、LLM のひと言を作る"""

    state.phase = PHASE_RESULT
    total = len(state.answers)
    score = state.score()
    state.comment = tier_message(settings, score, total)

    if not settings.comment_persona:
        return

    state.comment_pending = True
    session = state.session
    cancel = state.cancel
    category = state.category
    misses = [q.text for _, q, _ in state.misses()][:COMMENT_MISSES]

    state.comment_worker = threading.Thread(
        target=_make_comment,
        args=(state, session, cancel, settings, category, score, total, misses), daemon=True)
    state.comment_worker.start()
    state.notify()


def _make_comment(
    state: QuizState, session: int, cancel: CancelToken, settings: QuizSettings,
    category: Category | None, score: int, total: int, misses: list[str],
) -> None:
    """回答役の口調で、結果をねぎらうひと言を作って置き換える (失敗したら固定文のまま)"""

    text = ""

    try:
        try:
            persona = create_answerer(settings.comment_persona).system_prompt
        except Exception:
            persona = "あなたは親切なAIアシスタントです。日本語で答えてください。"

        agent = Agent(
            name="ねぎらい",
            system_prompt=persona + (
                "\n\nあなたはクイズの結果を伝える係です。次の結果を見て、受験者をねぎらうひと言を、"
                "日本語で 1〜2 文 (80 文字以内) で書いてください。"
                "点数を繰り返すだけにせず、前向きな言葉にしてください。"),
            options=LLMOptions(max_tokens=200, temperature=0.7, disable_thinking=True))
        lines = [f"分野: {category.name if category else ''}", f"点数: {score} / {total}"]

        if misses:
            lines.append("間違えた問題:")
            lines.extend(f"- {m}" for m in misses)

        text = run_agent(
            agent, [{"role": "user", "content": "\n".join(lines)}], None, cancel).strip()
    except Exception:
        text = ""

    with state.lock:
        if state.session == session:
            if text:
                state.comment = text

            state.comment_pending = False

    state.notify()


def quit_quiz(state: QuizState) -> None:
    """やめて、分野の選択に戻る (結果の画面からも使う)"""

    with state.lock:
        _new_session(state)
        state.phase = PHASE_SELECT
        state.generating = False
        state.comment_pending = False
        state.error = ""

    state.notify()


def shutdown(state: QuizState) -> None:
    """アプリを閉じるとき: 通信を止める"""

    state.cancel.cancel()
