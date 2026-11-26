"""アプリ本体 (Tk)

C++ 版の app/application.cpp に当たる。タブ (チャット・クイズ・迷路) と、
タブが共有するもの (資料・フォント・色・設定) を持ち、終了の順番を決める。
"""

from __future__ import annotations

import sys
import threading
import tkinter as tk
import tkinter.font as tkfont
from tkinter import ttk

from chat.chat_ui import ChatTab
from common import config
from common import rag
from common import server_manager
from common.server_manager import ServerStatus, ServerView
from common.settings_ui import COLOR_ENTRIES, SettingsDialog, is_color
from common.tab import Tab, UiContext
from maze.maze_ui import MazeTab
from quiz.quiz_ui import QuizTab
from sandbox.sandbox_ui import SandboxTab

# 文字の大きさ (ポイント)。config/ui.json の python_font_size で変えられる
# (font_size は C++ 版のピクセル数なので別にしている)
DEFAULT_FONT_SIZE = 14

# サーバーの状態の行を更新しに行く間隔 (ミリ秒)
POLL_INTERVAL_MS = 30

# 色の既定値。config/ui.json の python_colors で変えられる
# (背景と文字の既定は OS のテーマの色で、起動時に Text の色から読む)
DEFAULT_COLORS = {
    "user": "#4A90E2",
    "ai": "#3FA34D",
    "sub_text": "#888888",
    "error": "#E05555",
}


def _font_size(ui: dict) -> int:
    """config/ui.json の python_font_size (無い・正しくないときは既定値)"""

    size = ui.get("python_font_size", DEFAULT_FONT_SIZE)

    if isinstance(size, bool) or not isinstance(size, (int, float)) or size <= 0:
        print("python_font_size が正しくありません (既定値を使います)",
              file=sys.stderr)
        return DEFAULT_FONT_SIZE

    return round(size)


def _colors(ui: dict, defaults: dict[str, str]) -> dict[str, str]:
    """config/ui.json の python_colors (無い・正しくない色は既定値)"""

    colors = dict(defaults)
    configured = ui.get("python_colors", {})

    if not isinstance(configured, dict):
        print("python_colors が正しくありません (既定値を使います)",
              file=sys.stderr)
        return colors

    for key, _label in COLOR_ENTRIES:
        if key not in configured:
            continue

        if is_color(configured[key]):
            colors[key] = configured[key]
        else:
            print(f"python_colors の {key} は \"#RRGGBB\" で書いてください "
                  "(既定値を使います)", file=sys.stderr)

    return colors


def _to_hex(widget: tk.Misc, color: str) -> str:
    """Tk の色 (名前や OS のテーマの色も) を "#RRGGBB" にする"""

    r, g, b = widget.winfo_rgb(color)
    return f"#{r >> 8:02X}{g >> 8:02X}{b >> 8:02X}"


def _theme_colors(root: tk.Tk) -> dict[str, str]:
    """OS のテーマの、Text の背景と文字の色"""

    probe = tk.Text(root)

    try:
        return {
            "background": _to_hex(probe, probe.cget("background")),
            "text": _to_hex(probe, probe.cget("foreground")),
        }
    finally:
        probe.destroy()


class Application:
    """ウィンドウ・タブ・共有の状態を持つ (Tk のメインループは main() が回す)"""

    def __init__(self, root: tk.Tk) -> None:
        self.root = root

        # サーバーの状態が変わったら立てて、UI スレッドで見に行く
        self._dirty = threading.Event()

        # タブが参照するので、タブより先に作る
        self.rag = rag.RagState(self._wake)
        self._tabs: list[Tab] = []
        self._chat: ChatTab | None = None

        root.title("Local LLM Chat")
        root.geometry("900x700")
        root.minsize(480, 360)
        root.protocol("WM_DELETE_WINDOW", self.shutdown)

        ui = config.load_or_empty("ui.json")

        base = tkfont.nametofont("TkDefaultFont").actual()["family"]
        self.font = tkfont.Font(family=base)
        self.bold = tkfont.Font(family=base, weight="bold")
        self.small = tkfont.Font(family=base)
        self._apply_font_size(_font_size(ui))

        self.default_colors = {**_theme_colors(root), **DEFAULT_COLORS}
        self.colors = _colors(ui, self.default_colors)

        # サーバーの状態はどのタブでも見えるように、タブの外 (上) に置く
        self._build_server_bar()

        # 設定はどのタブからでも開けるように、タブの上の行の右端に置く
        top_bar = ttk.Frame(root, padding=(8, 4, 8, 0))
        top_bar.pack(side="top", fill="x")
        ttk.Button(top_bar, text="設定", command=self._open_settings).pack(
            side="right")

        # チャット・クイズ・迷路はタブで切り替える (先頭のタブを最初に開く)
        self.notebook = ttk.Notebook(root)
        self.notebook.pack(side="top", fill="both", expand=True)

        context = UiContext(self.font, self.bold, self.small, lambda: self.colors)
        self._chat = ChatTab(self.notebook, context, self.rag)
        self._tabs = [
            self._chat,
            QuizTab(self.notebook, context, self.rag),
            MazeTab(self.notebook, context),
            SandboxTab(self.notebook, context),
        ]

        for tab in self._tabs:
            self.notebook.add(tab, text=tab.label)

        self._apply_colors(self.colors)

        # server.json の managed がオンなら llama-server を起動する
        server_manager.start(self._wake)

        # rag/ の資料のインデックスをバックグラウンドで作る
        rag.start_indexing(self.rag)
        self._wake()
        self._poll()

    def _wake(self) -> None:
        """サーバーの状態や資料が変わったとき (ワーカースレッドからも呼ばれる)

        資料の状態と、サーバーを待つ送信ボタンはチャットのタブが出すので、チャットも描き直す
        (クイズと迷路は、自分の操作やワーカーで描き直す)"""

        self._dirty.set()

        if self._chat is not None:
            self._chat.wake()

    # --- サーバーの状態 ---

    def _build_server_bar(self) -> None:
        """llama-server が使えないときだけ出す状態の行"""

        self.server_bar = ttk.Frame(self.root, padding=(8, 0))
        self.server_bar.pack(side="top", fill="x")
        self.server_bar.columnconfigure(0, weight=1)

        self.server_lines = [
            ttk.Label(self.server_bar, wraplength=860) for _ in range(2)]
        self.server_retry = ttk.Button(
            self.server_bar, text="もう一度起動", command=server_manager.retry)

    def _poll(self) -> None:
        if self._dirty.is_set():
            self._dirty.clear()
            self._refresh_server_bar()

        self.root.after(POLL_INTERVAL_MS, self._poll)

    def _refresh_server_bar(self) -> None:
        """読み込み中と失敗のときだけ、サーバーの状態を出す"""

        view = server_manager.view()
        items: tuple[tuple[str, ServerView], ...] = (
            ("チャットのサーバー", view.chat), ("埋め込みのサーバー", view.embed))
        failed = False

        for row, (label, (name, server)) in enumerate(zip(self.server_lines, items)):
            if not view.enabled or server.status not in (
                    ServerStatus.STARTING, ServerStatus.FAILED):
                label.grid_remove()
                continue

            if server.status == ServerStatus.FAILED:
                failed = True
                text = f"{name}: {server.detail}"

                if server.log_tail:
                    text += "\n" + server.log_tail

                label.configure(text=text, foreground=self.colors["error"])
            else:
                label.configure(
                    text=f"{name}: {server.status.value} ({server.detail})",
                    foreground=self.colors["sub_text"])

            label.grid(row=row, column=0, sticky="w")

        if failed:
            self.server_retry.grid(row=2, column=0, sticky="w", pady=(2, 0))
        else:
            self.server_retry.grid_remove()

    # --- フォントと色 ---

    def _apply_font_size(self, size: int) -> None:
        """文字の大きさを変える (フォントを使っている所はすべて描き直される)"""

        self.font_size = size
        self.font.configure(size=size)
        self.bold.configure(size=size)
        self.small.configure(size=max(size - 3, 1))

    def _apply_colors(self, colors: dict[str, str]) -> None:
        """色を変えて、すべてのタブに付け直させる"""

        self.colors = dict(colors)

        for tab in self._tabs:
            tab.apply_colors()

        self._dirty.set()

    # --- 設定 ---

    def _open_settings(self) -> None:
        SettingsDialog(
            self.root, self.font, self.font_size, self._apply_font_size,
            self.colors, self.default_colors, self._apply_colors,
            self._on_settings_reset, self._on_settings_saved)

    def _on_settings_saved(self) -> None:
        """保存したら、タブに設定を読み直させ、サーバーの設定も読み直す"""

        for tab in self._tabs:
            tab.on_settings_saved()

        server_manager.reload()
        self._dirty.set()

    def _on_settings_reset(self) -> None:
        """設定を既定値に戻したあと、画面の設定を読み直して設定画面を開き直す"""

        self._on_settings_saved()
        ui = config.load_or_empty("ui.json")
        self._apply_font_size(_font_size(ui))
        self._apply_colors(_colors(ui, self.default_colors))
        self._open_settings()

    # --- 終了 ---

    def shutdown(self) -> None:
        """タブの通信と資料のインデックス作成を止め、サーバーを止めてウィンドウを壊す"""

        # ワーカーはデーモンスレッドなので、中断を頼むだけで待たない
        for tab in self._tabs:
            tab.shutdown()

        self.rag.cancel.cancel()

        # サーバーの停止に数秒かかることがあるので、先にウィンドウを閉じる
        self.root.withdraw()
        server_manager.stop()
        self.root.destroy()


def main() -> None:
    root = tk.Tk()
    Application(root)

    try:
        root.mainloop()
    finally:
        # Ctrl+C などでも、起動したサーバーを残さない
        server_manager.stop()
