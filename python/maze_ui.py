"""迷路タブの画面 (Tk): マス目とロボット、指示の入力欄、指示と命令の履歴
C++ 版 (src/maze_ui.cpp) と同じ動き。説明は doc/maze.md を参照"""

from __future__ import annotations

import math
import threading
import time
import tkinter as tk
import tkinter.font as tkfont
from collections import deque
from dataclasses import dataclass, field
from tkinter import ttk
from typing import Callable

import maze_mode
import server_manager
from maze_mode import GOAL, SIZE, START, Move

# ワーカースレッドからの更新を見に行く間隔 (ミリ秒)
POLL_INTERVAL_MS = 50

# アニメーションの 1 コマの間隔 (ミリ秒)
FRAME_MS = 16

# 1 マス進むのにかける秒数と、ぶつかったときに揺れる秒数
STEP_SECONDS = 0.22
BUMP_SECONDS = 0.4

# ステージごとの入力欄の例
HINTS = (
    "例: 右に3つ、上に2つ",
    "例: ぶつかるまで右、行けるところまで上",
    "例: まっすぐ進めるところまで、振り返ってふたつ",
)

TRIANGLE = {
    maze_mode.UP: (0, -1), maze_mode.DOWN: (0, 1),
    maze_mode.LEFT: (-1, 0), maze_mode.RIGHT: (1, 0),
}


@dataclass
class LogEntry:
    """指示の履歴の 1 件"""

    text: str

    # LLM が返した命令 (1 行に 1 命令。ロボットから見た向きは、直した向きを書き足す)
    commands: list[str] = field(default_factory=list)

    # 結果 (ゴール・ぶつかった・読み取れなかったなど)
    result: str = ""
    failed: bool = False

    # 訳すのにかかった秒数
    elapsed: float = 0.0


def _blend(color: str, background: str, alpha: float) -> str:
    """color を alpha の濃さで background に重ねた色 (#rrggbb)"""

    def rgb(value: str) -> tuple[int, int, int]:
        value = value.lstrip("#")
        return int(value[0:2], 16), int(value[2:4], 16), int(value[4:6], 16)

    try:
        fg, bg = rgb(color), rgb(background)
    except (ValueError, IndexError):
        return color

    mixed = (round(f * alpha + b * (1 - alpha)) for f, b in zip(fg, bg))
    return "#%02x%02x%02x" % tuple(mixed)


def _star_text(count: int) -> str:
    return "★" * count + "☆" * (3 - count)


class MazeTab(ttk.Frame):
    """日本語の指示でロボットを動かして、障害物を避けてゴールまで運ぶ画面"""

    def __init__(
        self,
        parent: tk.Misc,
        font: tkfont.Font,
        bold: tkfont.Font,
        colors: Callable[[], dict[str, str]],
    ) -> None:
        super().__init__(parent)

        self._font = font
        self._bold = bold
        self._colors = colors
        self._dirty = threading.Event()
        self.state = maze_mode.MazeState(self._dirty.set)

        # ステージ (0 から) と、ステージごとの一番良かった★の数 (0 は未クリア)
        self._stage = 0
        self._best = [0] * len(maze_mode.STAGES)

        self._board = maze_mode.generate_board()
        self._log: list[LogEntry] = []
        self._talks = 0
        self._cleared = False

        # ロボットのいるマス・向きと、通ったマス
        self._robot = START
        self._facing = maze_mode.RIGHT
        self._trail = [START]

        # 実行待ちの命令 (先頭の steps は、残りのマス数) と、実行中の命令の番号
        self._pending: deque[Move] = deque()
        self._command_index = 0
        self._executing = False
        self._moved_steps = 0

        # いま動いている 1 マスの進み具合 (0〜1) と、ぶつかったときの揺れ (残り秒)
        self._moving = False
        self._from = START
        self._progress = 0.0
        self._bump = 0.0
        self._last_frame = 0.0
        self._animating = False

        self._board_size = 300

        self._build()
        self._dirty.set()
        self.after(POLL_INTERVAL_MS, self._poll)

    # --- 画面の部品 ---

    def _build(self) -> None:
        # 入力欄は下から先に詰める (ウィンドウが狭いときに、盤と履歴が縮むように)
        bottom = ttk.Frame(self, padding=(8, 6))
        bottom.pack(side="bottom", fill="x")

        self.input = ttk.Entry(bottom, font=self._font)
        self.input.pack(side="left", fill="x", expand=True)
        self.input.bind("<Return>", lambda _e: self._send())

        ttk.Button(bottom, text="新しい迷路", command=lambda: self._reset(True)).pack(side="right")
        ttk.Button(bottom, text="やり直す", command=lambda: self._reset(False)).pack(side="right", padx=(6, 0))
        self.send_button = ttk.Button(bottom, text="動かす", command=self._send)
        self.send_button.pack(side="right", padx=(6, 0))

        self.hint_label = ttk.Label(self, padding=(8, 0))
        self.hint_label.pack(side="bottom", fill="x")

        top = ttk.Frame(self, padding=(8, 6, 8, 0))
        top.pack(side="top", fill="x")

        # ステージの選択 (どのステージからでも遊べる。クリアした★を並べる)
        self.stage_buttons: list[ttk.Button] = []

        for index in range(len(maze_mode.STAGES)):
            button = ttk.Button(top, command=lambda i=index: self._select_stage(i))
            button.pack(side="left", padx=(0, 6))
            self.stage_buttons.append(button)

        # ゴールしたら、次のステージへ進める
        self.next_button = ttk.Button(top, text="次のステージへ",
                                      command=lambda: self._select_stage(self._stage + 1))

        self.rule_label = ttk.Label(self, padding=(8, 4, 8, 0), font=self._font)
        self.rule_label.pack(side="top", fill="x")

        status = ttk.Frame(self, padding=(8, 2))
        status.pack(side="top", fill="x")
        self.talks_label = ttk.Label(status, font=self._font)
        self.talks_label.pack(side="left")
        self.turns_label = ttk.Label(status, font=self._font)
        self.turns_label.pack(side="left", padx=(16, 0))
        self.warning_label = ttk.Label(status, font=self._font)
        self.warning_label.pack(side="left", padx=(16, 0))

        middle = ttk.Frame(self, padding=(8, 4))
        middle.pack(side="top", fill="both", expand=True)
        middle.bind("<Configure>", self._on_resize)

        self.canvas = tk.Canvas(middle, highlightthickness=0, width=self._board_size, height=self._board_size)
        self.canvas.pack(side="left", anchor="n")

        self.log_text = tk.Text(middle, wrap="word", font=self._font, padx=8, pady=6,
                                state="disabled", relief="flat", highlightthickness=1)
        scroll = ttk.Scrollbar(middle, orient="vertical", command=self.log_text.yview)
        self.log_text.configure(yscrollcommand=scroll.set)
        scroll.pack(side="right", fill="y")
        self.log_text.pack(side="left", fill="both", expand=True, padx=(8, 0))

        self.log_text.tag_configure("user", font=self._bold)
        self.log_text.tag_configure("command", lmargin1=24, lmargin2=24)
        self.log_text.tag_configure("result", lmargin1=24, lmargin2=24)

        # 色は、会話画面が色を決めたときに apply_colors で付ける

    def apply_colors(self) -> None:
        """色を会話画面に合わせる"""

        colors = self._colors()
        self.canvas.configure(background=colors["background"])
        self.log_text.configure(background=colors["background"], foreground=colors["text"],
                                highlightbackground=colors["sub_text"])
        self.log_text.tag_configure("user", foreground=colors["user"])
        self.log_text.tag_configure("command", foreground=colors["sub_text"])
        self.log_text.tag_configure("sub", foreground=colors["sub_text"])
        self.log_text.tag_configure("ok", foreground=colors["ai"])
        self.log_text.tag_configure("failed", foreground=colors["error"])
        self.warning_label.configure(foreground=colors["error"])
        self.turns_label.configure(foreground=colors["sub_text"])
        self.hint_label.configure(foreground=colors["sub_text"])
        self._dirty.set()

    def _on_resize(self, event: tk.Event) -> None:
        """盤は正方形にして、高さと幅の半分くらいの小さい方に合わせる"""

        size = max(120, min(event.height - 8, int(event.width * 0.55)))

        if size != self._board_size:
            self._board_size = size
            self.canvas.configure(width=size, height=size)
            self._draw_board()

        self.rule_label.configure(wraplength=max(event.width - 16, 200))

    # --- 操作 ---

    def _select_stage(self, stage: int) -> None:
        """ステージを変えて、新しい盤で始める"""

        self._stage = max(0, min(stage, len(maze_mode.STAGES) - 1))
        self._reset(True)

    def _reset(self, new_board: bool) -> None:
        """位置・通った跡・履歴・回数を最初に戻す (訳している途中なら中断する)
        new_board なら新しい盤にする (False なら同じ盤でやり直す)"""

        self.state.stop()

        if new_board:
            self._board = maze_mode.generate_board(maze_mode.STAGES[self._stage].slide_only)

        self._log.clear()
        self._talks = 0
        self._cleared = False
        self._robot = START
        self._from = START
        self._facing = maze_mode.RIGHT
        self._trail = [START]
        self._pending.clear()
        self._executing = False
        self._moving = False
        self._progress = 0.0
        self._bump = 0.0
        self.warning_label.configure(text="")
        self.input.focus_set()
        self._dirty.set()

    def _send(self) -> None:
        text = self.input.get().strip()

        with self.state.lock:
            busy = self.state.busy

        if not text or busy or self._executing or self._cleared or not server_manager.is_chat_available():
            return

        # 禁止ワードが入っていたら送らない (回数にも数えない。入力はそのまま残す)
        stage = maze_mode.STAGES[self._stage]
        banned = maze_mode.find_banned_word(stage, text)

        if banned:
            self.warning_label.configure(text=f"「{banned}」は使えません")
            return

        self.warning_label.configure(text="")
        self._log.append(LogEntry(text))
        self.state.start(text, stage.hints)
        self.input.delete(0, "end")
        self._dirty.set()

    # --- 結果の受け取りとアニメーション ---

    def _take_result(self) -> None:
        """ワーカーの結果 (訳した命令・エラー) を受け取って、履歴に書き、実行を始める"""

        plan, error, elapsed = self.state.take()

        if (plan is None and not error) or not self._log:
            return

        entry = self._log[-1]
        entry.elapsed = elapsed

        if error:
            entry.result = "エラー: " + error
            entry.failed = True
            return

        assert plan is not None

        # 命令は 1 件ずつ実行する (ロボットから見た向きは、その命令を始めるときの向きで決める)
        for move in plan:
            entry.commands.append(maze_mode.describe_move(move))
            self._pending.append(Move(move.dir, move.steps, move.until_blocked))

        self._command_index = 0

        if not plan:
            entry.result = "動きの指示として読み取れませんでした (回数には数えません)"
            entry.failed = True
            return

        self._talks += 1
        self._executing = True
        self._moved_steps = 0
        self._start_animation()

    def _start_animation(self) -> None:
        if not self._animating:
            self._animating = True
            self._last_frame = time.monotonic()
            self.after(FRAME_MS, self._frame)

    def _frame(self) -> None:
        now = time.monotonic()
        dt = min(now - self._last_frame, 0.1)
        self._last_frame = now

        self._animate(dt)
        self._draw_board()

        if self._moving or self._bump > 0 or self._pending:
            self.after(FRAME_MS, self._frame)
        else:
            self._animating = False
            self._dirty.set()

    def _animate(self, dt: float) -> None:
        """1 マス分のアニメーションを進める (ふさがった先へ進む命令なら、そこで止める)"""

        self._bump = max(0.0, self._bump - dt)

        if self._moving:
            self._progress += dt / STEP_SECONDS

            if self._progress >= 1.0:
                self._moving = False
                self._progress = 0.0
                self._trail.append(self._robot)
                self._moved_steps += 1

                # ゴールに着いたら、残りの命令は捨てる
                if self._robot == GOAL:
                    self._pending.clear()
                    self._executing = False
                    self._cleared = True
                    self._best[self._stage] = max(self._best[self._stage], maze_mode.stars(self._talks))

                    if self._log:
                        self._log[-1].result = f"ゴール！ ({self._talks} 回目の指示で到着)"

                    self._dirty.set()

        # 進めるところまで進む命令は、先がふさがったら次の命令へ (同じコマで続けて調べる)
        while not self._moving and self._pending:
            move = self._pending[0]

            # ロボットから見た向きを、いまの向きで画面から見た向きに直す (履歴にも書き足す)
            if move.dir in maze_mode.TURNS:
                move.dir = maze_mode.resolve_turn(self._facing, move.dir)

                if self._log and self._command_index < len(self._log[-1].commands):
                    self._log[-1].commands[self._command_index] += f"  → {move.dir}"
                    self._dirty.set()

            self._facing = move.dir
            nxt = maze_mode.neighbor(self._robot, move.dir)

            if maze_mode.is_open(self._board, nxt):
                self._from = self._robot
                self._robot = nxt
                self._moving = True

                if not move.until_blocked:
                    move.steps -= 1

                    if move.steps <= 0:
                        self._pending.popleft()
                        self._command_index += 1
            elif move.until_blocked:
                self._pending.popleft()
                self._command_index += 1
            else:
                self._pending.clear()
                self._bump = BUMP_SECONDS
                self._executing = False

                if self._log:
                    what = "障害物" if maze_mode.inside(nxt) else "盤の端"
                    self._log[-1].result = f"{self._moved_steps} マス進んで、{what}にぶつかりました"
                    self._log[-1].failed = True

                self._dirty.set()

        if self._executing and not self._moving and not self._pending:
            self._executing = False

            if self._log:
                self._log[-1].result = f"{self._moved_steps} マス進みました"

            self._dirty.set()

    # --- 描画 ---

    def _poll(self) -> None:
        self._take_result()

        if self._dirty.is_set():
            self._dirty.clear()
            self._refresh()

        self.after(POLL_INTERVAL_MS, self._poll)

    def _refresh(self) -> None:
        with self.state.lock:
            busy = self.state.busy

        stage = maze_mode.STAGES[self._stage]

        for index, button in enumerate(self.stage_buttons):
            label = maze_mode.STAGES[index].title

            if self._best[index] > 0:
                label += " " + _star_text(self._best[index])

            if index == self._stage:
                label = "▶ " + label

            button.configure(text=label)

        if self._cleared and self._stage + 1 < len(maze_mode.STAGES):
            self.next_button.pack(side="left", padx=(16, 0))
        else:
            self.next_button.pack_forget()

        self.rule_label.configure(text="S から G まで、障害物を避けて運ぼう。" + stage.rule)
        self.talks_label.configure(text=f"指示: {self._talks} 回")
        self.turns_label.configure(text=f"(最短の道順で曲がる回数: {self._board.min_turns})")
        self.hint_label.configure(text=HINTS[self._stage])

        usable = (not busy and not self._executing and not self._cleared
                  and server_manager.is_chat_available())
        self.send_button.configure(state="normal" if usable else "disabled")

        self._render_log(busy)
        self._draw_board()

    def _render_log(self, busy: bool) -> None:
        self.log_text.configure(state="normal")
        self.log_text.delete("1.0", "end")

        if not self._log:
            self.log_text.insert(
                "end", "指示を入力すると、LLM が命令 (JSON) に訳し、ロボットが動きます。\n", "sub")

        for index, entry in enumerate(self._log):
            self.log_text.insert("end", f"「{entry.text}」\n", "user")

            for command in entry.commands:
                self.log_text.insert("end", command + "\n", "command")

            if index == len(self._log) - 1 and busy:
                self.log_text.insert("end", "命令に訳しています…\n", ("result", "sub"))
            elif entry.result:
                self.log_text.insert("end", entry.result, ("result", "failed" if entry.failed else "ok"))

                if entry.elapsed > 0:
                    self.log_text.insert("end", f" ({entry.elapsed:.1f} 秒で訳しました)", ("result", "sub"))

                self.log_text.insert("end", "\n")

            self.log_text.insert("end", "\n")

        self.log_text.configure(state="disabled")
        self.log_text.see("end")

    def _draw_board(self) -> None:
        colors = self._colors()
        background = colors["background"]
        canvas = self.canvas
        canvas.delete("all")

        size = self._board_size
        cell = size / SIZE

        canvas.create_rectangle(0, 0, size, size, outline="",
                                fill=_blend(colors["text"], background, 0.05))

        # 通ったマス
        trail_color = _blend(colors["ai"], background, 0.18)

        for x, y in set(self._trail):
            canvas.create_rectangle(x * cell + 2, y * cell + 2, (x + 1) * cell - 2, (y + 1) * cell - 2,
                                    outline="", fill=trail_color)

        # 障害物
        block_color = _blend(colors["sub_text"], background, 0.75)

        for x, y in self._board.blocked:
            canvas.create_rectangle(x * cell + 3, y * cell + 3, (x + 1) * cell - 3, (y + 1) * cell - 3,
                                    outline="", fill=block_color)

        # ゴールのマス (枠と G)
        gx, gy = GOAL
        strength = 1.0 if self._cleared else 0.6
        canvas.create_rectangle(gx * cell + 3, gy * cell + 3, (gx + 1) * cell - 3, (gy + 1) * cell - 3,
                                outline=_blend(colors["ai"], background, strength), width=2,
                                fill=_blend(colors["ai"], background, 0.25 * strength))
        canvas.create_text((gx + 1) * cell - cell * 0.12, (gy + 1) * cell - cell * 0.08, text="G",
                           anchor="se", fill=colors["ai"], font=self._bold)

        # スタートのマス
        sx, sy = START
        canvas.create_text(sx * cell + cell * 0.1, (sy + 1) * cell - cell * 0.08, text="S",
                           anchor="sw", fill=colors["sub_text"], font=self._bold)

        # マス目の線
        line = _blend(colors["sub_text"], background, 0.5)

        for i in range(SIZE + 1):
            p = i * cell
            canvas.create_line(p, 0, p, size, fill=line)
            canvas.create_line(0, p, size, p, fill=line)

        # ロボット (動いている間は前のマスとの間を補う)
        x, y = float(self._robot[0]), float(self._robot[1])

        if self._moving:
            t = max(0.0, min(self._progress, 1.0))
            t = t * t * (3 - 2 * t)
            x = self._from[0] + (self._robot[0] - self._from[0]) * t
            y = self._from[1] + (self._robot[1] - self._from[1]) * t

        dx, dy = TRIANGLE[self._facing]
        cx, cy = (x + 0.5) * cell, (y + 0.5) * cell

        if self._bump > 0:
            # ぶつかった向きに小刻みに揺らす
            shake = math.sin(self._bump * 60) * cell * 0.08 * (self._bump / BUMP_SECONDS)
            cx += dx * shake
            cy += dy * shake

        radius = cell * 0.32
        body = colors["error"] if self._bump > 0 else colors["user"]
        canvas.create_oval(cx - radius, cy - radius, cx + radius, cy + radius, outline="", fill=body)

        # 向きを示す三角
        sx_, sy_ = -dy, dx
        tip = (cx + dx * radius * 0.85, cy + dy * radius * 0.85)
        base = (cx + dx * radius * 0.2, cy + dy * radius * 0.2)
        half = radius * 0.4
        canvas.create_polygon(tip[0], tip[1],
                              base[0] + sx_ * half, base[1] + sy_ * half,
                              base[0] - sx_ * half, base[1] - sy_ * half,
                              outline="", fill=background)

        # ゴールしたら、盤の上に結果を重ねる
        if self._cleared:
            line_height = self._bold.metrics("linespace") * 1.2
            top = size / 2 - line_height * 1.8
            canvas.create_rectangle(0, top, size, top + line_height * 3.6, outline="",
                                    fill=_blend(background, colors["text"], 0.85))
            texts = (("ゴール！", colors["ai"]),
                     (_star_text(maze_mode.stars(self._talks)), colors["text"]),
                     (f"{self._talks} 回の指示で到着", colors["sub_text"]))

            for index, (text, color) in enumerate(texts):
                canvas.create_text(size / 2, top + line_height * (0.8 + index), text=text,
                                   fill=color, font=self._bold)

    def shutdown(self) -> None:
        """アプリを閉じるとき: 通信を止める (ワーカーはデーモンスレッドなので待たない)"""

        self.state.stop()
