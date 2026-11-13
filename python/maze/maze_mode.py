"""迷路: 6×6 のマス目のロボットを、日本語の指示で動かして、障害物を避けてゴールまで運ぶ
LLM は指示を移動命令の JSON に訳すだけ (盤面もロボットの向きも見せない)。動かすのと判定はアプリ
C++ 版 (src/maze_mode.cpp) と同じ動き。説明は doc/maze.md を参照"""

from __future__ import annotations

import random
import threading
import time
from collections import deque
from dataclasses import dataclass, field
from typing import Any, Callable

from common.agent import Agent, run_agent_json
from common.llm_client import CancelToken, Cancelled, LLMOptions

SIZE = 6

# 1 つの命令で進める最大のマス数
MAX_STEPS = 5

# 障害物の数と、最短の道順で曲がる回数の下限 (簡単すぎる盤を避ける)
MIN_BLOCKS = 9
MAX_BLOCKS = 12
MIN_TURNS = 3

# 1 回の指示で受け付ける命令の数
MAX_MOVES = 10

TRANSLATOR_MAX_TOKENS = 512

# スタートは左下、ゴールは右上 (x, y)。y は下に向かって増える
START = (0, SIZE - 1)
GOAL = (SIZE - 1, 0)

# 画面から見た向き
UP, DOWN, LEFT, RIGHT = "up", "down", "left", "right"
DIRECTIONS = (UP, DOWN, LEFT, RIGHT)
DELTAS = {UP: (0, -1), DOWN: (0, 1), LEFT: (-1, 0), RIGHT: (1, 0)}

# ロボットから見た向き (LLM は言葉を置き換えるだけ。いまの向きからの計算はアプリが実行するときに行う)
# clockwise / counterclockwise という名前だと、Qwen3 4B が「時計回り」を取り違えやすかった
FORWARD, BACK, TURN_CW, TURN_CCW = "forward", "back", "turn_cw", "turn_ccw"
TURNS = (FORWARD, BACK, TURN_CW, TURN_CCW)

# 時計回りの順 (上 → 右 → 下 → 左)
CLOCKWISE = (UP, RIGHT, DOWN, LEFT)

# 向きの言葉は、断りが無ければ画面から見た向きにする (ロボットから見た右と取り違えないように)
TRANSLATOR_PROMPT = (
    "あなたは、ロボットへの日本語の指示を移動命令に変換する係です。"
    "ロボットはマス目の上を動きます。"
    "dir は画面から見た向きで、上は up、下は down、左は left、右は right です。"
    "向きを表す言葉 (右・上・北・3時の方向など) は、特に断りが無ければ、すべて画面から見た向きです。"
    "steps は進むマス数 (1〜5) です。数が書かれていなければ 1 にします。"
    "「ぶつかるまで」「行けるところまで」「行けるだけ」「突き当たりまで」「ずっと」のように、"
    "止まる場所まで進む指示なら until_blocked を true にします (steps は 1 にしておきます)。それ以外は false です。"
    "「しばらく」「ちょっと」のようなあいまいな言い方は、あなたの判断でマス数を決めます。"
    "ロボットから見た向きで言われたときは、dir に次の値を使います。向きの計算はしなくてよく、言葉をそのまま置き換えます。"
    "まっすぐ・前へ・そのまま進む → forward、振り返って・後ろへ・来た方へ戻る・向いている方の反対へ → back、"
    "時計回りに曲がって → turn_cw、反時計回りに曲がって → turn_ccw。"
    "turn_cw と turn_ccw は、曲がってから、そのまま steps マス (until_blocked なら進めるところまで) 進む 1 つの命令です。"
    "曲がった後の移動を forward に分けないでください。"
    "指示に書かれた順に、命令を並べます。"
    "移動と関係の無い指示なら、moves を空にします。"
    "指示に無い移動を足さないでください。"
)

# 向きの言葉を禁止するステージで足す説明 (小さいモデルでも言い換えが通じるように、よくある言い方の常識を添える)
HINT_PROMPT = (
    "時計の文字盤の向きは、12時が up、3時が right、6時が down、9時が left です。"
    "空・天井・頭の方は up、地面・床・足もとの方は down です。"
    "お箸を持つ手の方は right、お茶碗を持つ手の方は left です。"
)


@dataclass
class Move:
    """1 つの命令。dir は画面から見た向きか、ロボットから見た向き (TURNS)"""

    dir: str
    steps: int = 1

    # True なら steps によらず、進めるところまで進む (障害物や盤の端の手前で止まる。ぶつかった扱いにはしない)
    until_blocked: bool = False


@dataclass
class Stage:
    """ステージ (禁止ワードで、指示の言い方を縛る)"""

    title: str

    # 画面に出すルールの説明
    rule: str

    # 指示に入れてはいけない言葉 (英字は大文字・小文字を区別しない)
    banned: tuple[str, ...] = ()

    # 盤を「進めるところまで進む」だけで解けるものに限る (数字を使えないステージ用)
    slide_only: bool = False

    # LLM に、向きの言い換えの常識 (時計の文字盤・空は上など) を教える
    hints: bool = False


STAGES = (
    Stage("ステージ1", "自由に指示してよい"),
    Stage(
        "ステージ2",
        "数字禁止 (0〜9・０〜９・一〜十・ひとつ〜いつつ・one〜five)。「ぶつかるまで」などで伝えよう",
        tuple("0123456789") + tuple("０１２３４５６７８９") + tuple("一二三四五六七八九十")
        + ("ひとつ", "ふたつ", "みっつ", "よっつ", "いつつ", "one", "two", "three", "four", "five"),
        slide_only=True,
    ),
    Stage(
        "ステージ3",
        "向きの言葉禁止 (上下左右・東西南北・うえ・みぎ・ひだり・英語)。ロボットの向き (三角) や身近なものを使って伝えよう",
        tuple("上下左右東西南北")
        + ("うえ", "みぎ", "ひだり", "ウエ", "シタ", "ミギ", "ヒダリ",
           "up", "down", "left", "right", "north", "south", "east", "west"),
        hints=True,
    ),
)


def find_banned_word(stage: Stage, text: str) -> str:
    """指示に入っている禁止ワード (無ければ空)"""

    # 英字だけ小文字にそろえる (str.lower は全角英字も変えるが、一覧は半角だけなので同じ結果になる)
    lower = text.lower()

    for word in stage.banned:
        if word in lower:
            return word

    return ""


def stars(talks: int) -> int:
    """★の数 (1 回なら 3、2 回なら 2、それより多ければ 1)"""

    return 3 if talks <= 1 else 2 if talks == 2 else 1


def resolve_turn(facing: str, turn: str) -> str:
    """facing を向いたロボットにとっての turn の向き (画面から見た向き)"""

    index = CLOCKWISE.index(facing)

    if turn == BACK:
        return CLOCKWISE[(index + 2) % 4]
    if turn == TURN_CW:
        return CLOCKWISE[(index + 1) % 4]
    if turn == TURN_CCW:
        return CLOCKWISE[(index + 3) % 4]

    return facing


def describe_move(move: Move) -> str:
    """訳した命令 (UI に出す文字列)"""

    if move.until_blocked:
        return f'{{"dir": "{move.dir}", "until_blocked": true}}'

    return f'{{"dir": "{move.dir}", "steps": {move.steps}}}'


# --- 盤 ---

@dataclass
class Board:
    """盤面 (障害物の位置)"""

    blocked: set[tuple[int, int]] = field(default_factory=set)

    # 最短の道順で曲がる回数 (目安として画面に出す)
    min_turns: int = 0


def neighbor(cell: tuple[int, int], direction: str) -> tuple[int, int]:
    dx, dy = DELTAS[direction]
    return cell[0] + dx, cell[1] + dy


def inside(cell: tuple[int, int]) -> bool:
    return 0 <= cell[0] < SIZE and 0 <= cell[1] < SIZE


def is_open(board: Board, cell: tuple[int, int]) -> bool:
    return inside(cell) and cell not in board.blocked


def min_turns(board: Board) -> int:
    """ゴールまでに曲がる回数の最小 (通れなければ -1)
    (マス, 向き) を頂点にして、まっすぐ進むのは 0、曲がるのは 1 の 0-1 BFS
    スタートではどの向きを向いてもよい (最初の 1 歩は曲がりに数えない)"""

    cost: dict[tuple[tuple[int, int], str], int] = {}
    queue: deque[tuple[tuple[int, int], str]] = deque()

    for direction in DIRECTIONS:
        cost[(START, direction)] = 0
        queue.append((START, direction))

    while queue:
        cell, facing = queue.popleft()
        current = cost[(cell, facing)]

        for direction in DIRECTIONS:
            nxt = neighbor(cell, direction)

            if not is_open(board, nxt):
                continue

            weight = 0 if direction == facing else 1
            key = (nxt, direction)

            if current + weight < cost.get(key, 1 << 20):
                cost[key] = current + weight

                if weight == 0:
                    queue.appendleft(key)
                else:
                    queue.append(key)

    best = min((cost[(GOAL, d)] for d in DIRECTIONS if (GOAL, d) in cost), default=-1)
    return best


def slide_solvable(board: Board) -> bool:
    """「進めるところまで進む」だけでゴールに着けるか (途中でゴールを通るのも着いたことにする)"""

    visited = {START}
    queue = deque([START])

    while queue:
        cell = queue.popleft()

        for direction in DIRECTIONS:
            stop = cell
            nxt = neighbor(stop, direction)

            while is_open(board, nxt):
                stop = nxt

                if stop == GOAL:
                    return True

                nxt = neighbor(stop, direction)

            if stop not in visited:
                visited.add(stop)
                queue.append(stop)

    return False


def generate_board(slide_only: bool = False) -> Board:
    """ランダムな盤を作る (スタートからゴールまで通れて、曲がる回数が MIN_TURNS 以上)
    slide_only なら、さらに「進めるところまで進む」だけでゴールできる盤に限る"""

    cells = [(x, y) for y in range(SIZE) for x in range(SIZE) if (x, y) not in (START, GOAL)]

    # 条件に合う盤が出なかったときは、曲がる回数が一番多かった盤 (障害物なしの盤より悪くはしない)
    best = Board()
    best.min_turns = min_turns(best)

    # 条件に合う盤が出るまで作り直す (6×6 なら、たいてい数回で出る)
    for _ in range(2000):
        count = random.randint(MIN_BLOCKS, MAX_BLOCKS)
        board = Board(set(random.sample(cells, count)))
        board.min_turns = min_turns(board)

        solvable = not slide_only or slide_solvable(board)

        if solvable and board.min_turns >= MIN_TURNS:
            return board

        if solvable and board.min_turns > best.min_turns:
            best = board

    return best


# --- 指示を命令に訳す ---

def _schema() -> dict[str, Any]:
    move = {
        "type": "object",
        "properties": {
            "dir": {"type": "string", "enum": list(DIRECTIONS + TURNS)},
            "steps": {"type": "integer", "minimum": 1, "maximum": MAX_STEPS},
            "until_blocked": {"type": "boolean"},
        },
        "required": ["dir", "steps", "until_blocked"],
    }

    return {
        "type": "object",
        "properties": {"moves": {"type": "array", "items": move, "maxItems": MAX_MOVES}},
        "required": ["moves"],
    }


def validate_moves(raw: dict[str, Any]) -> list[Move]:
    """LLM が返した JSON を命令の列にする。形が違えば ValueError"""

    moves = raw.get("moves")

    if not isinstance(moves, list):
        raise ValueError("命令 (moves) がありません")

    result: list[Move] = []

    for item in moves:
        if not isinstance(item, dict):
            continue

        direction = item.get("dir")

        if direction not in DIRECTIONS + TURNS:
            raise ValueError(f"知らない向きです: {direction}")

        steps = item.get("steps")

        if not isinstance(steps, int) or isinstance(steps, bool):
            raise ValueError("マス数 (steps) がありません")

        result.append(Move(direction, max(1, min(steps, MAX_STEPS)), item.get("until_blocked") is True))

        if len(result) >= MAX_MOVES:
            break

    return result


def translate(text: str, hints: bool, cancel: CancelToken) -> list[Move]:
    """指示を命令に訳す (ロボットの向きは LLM に見せない)"""

    agent = Agent(
        name="迷路",
        system_prompt=TRANSLATOR_PROMPT + (HINT_PROMPT if hints else ""),
        options=LLMOptions(
            max_tokens=TRANSLATOR_MAX_TOKENS, temperature=0.2,
            schema=_schema(), disable_thinking=True),
    )

    raw = run_agent_json(agent, [{"role": "user", "content": text}], cancel)
    return validate_moves(raw)


class MazeState:
    """ワーカーが書く結果 (lock で守る)。アニメーションの状態は画面側が持つ"""

    def __init__(self, on_change: Callable[[], None]) -> None:
        self.lock = threading.Lock()
        self._on_change = on_change

        # 指示を送るたび・リセットのたびに増やす。古いワーカーが書き込まないための目印
        self.session = 0
        self.cancel = CancelToken()

        self.busy = False
        self.plan: list[Move] | None = None
        self.error = ""
        self.elapsed = 0.0

    def start(self, text: str, hints: bool) -> None:
        """指示を別スレッドで LLM に訳させる (結果は plan か error に入る)"""

        self.stop()

        with self.lock:
            session = self.session
            cancel = self.cancel
            self.busy = True
            self.plan = None
            self.error = ""

        threading.Thread(
            target=self._run, args=(session, cancel, text, hints), daemon=True).start()

    def stop(self) -> None:
        """訳している途中なら中断する (結果は捨てる)"""

        with self.lock:
            self.session += 1
            self.cancel.cancel()
            self.cancel = CancelToken()
            self.busy = False
            self.plan = None
            self.error = ""

    def take(self) -> tuple[list[Move] | None, str, float]:
        """届いた結果を受け取る (命令の列、エラー、かかった秒数)。無ければ (None, "", 0)"""

        with self.lock:
            plan, error, elapsed = self.plan, self.error, self.elapsed
            self.plan = None
            self.error = ""

        return plan, error, elapsed

    def _run(self, session: int, cancel: CancelToken, text: str, hints: bool) -> None:
        started = time.monotonic()
        plan: list[Move] | None = None
        error = ""

        try:
            plan = translate(text, hints, cancel)
        except Cancelled:
            pass
        except Exception as e:
            error = str(e)

        with self.lock:
            if self.session != session:
                return

            self.busy = False
            self.elapsed = time.monotonic() - started
            self.plan = plan
            self.error = error

        self._on_change()
