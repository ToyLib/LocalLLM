"""チャット画面の組み立て (Tk)"""

from __future__ import annotations

import sys
import threading
import time
import tkinter as tk
import tkinter.font as tkfont
import webbrowser
from tkinter import messagebox, ttk
from typing import NamedTuple

from common import config
from common import rag
from common import server_manager
from chat.chat import (
    STAGE_LOCAL, STAGE_WEB, ChatState, RagMode, ask_cloud, can_ask_cloud,
    format_status, next_stage, refresh_cloud_status, send_prompt)
from common.llm_client import Citation
from maze.maze_ui import MazeTab
from quiz.quiz_ui import QuizTab
from common.server_manager import ServerStatus, ServerView
from common.settings_ui import COLOR_ENTRIES, SettingsDialog, is_color

# 文字の大きさ (ポイント)。config/ui.json の python_font_size で変えられる
# (font_size は C++ 版のピクセル数なので別にしている)
DEFAULT_FONT_SIZE = 14

# ワーカースレッドからの更新を見に行く間隔 (ミリ秒)
POLL_INTERVAL_MS = 30

# 受信中に、経過時間の表示を進めるために描き直す間隔 (秒)
STATUS_REFRESH_SECONDS = 0.5

# 色の既定値。config/ui.json の python_colors で変えられる
# (背景と文字の既定は OS のテーマの色で、起動時に会話欄から読む)
DEFAULT_COLORS = {
    "user": "#4A90E2",
    "ai": "#3FA34D",
    "sub_text": "#888888",
    "error": "#E05555",
}



class _Rendered(NamedTuple):
    """画面に表示した1件分 (変わった所から下だけを描き直す)"""

    role: str
    agent: str
    content: str
    note: str
    thinking: bool
    status: str = ""
    superseded: bool = False

    # ChatGPT の元の回答 (書き直したときだけ) と、開いているか
    cloud_original: str = ""
    original_open: bool = False

    # 聞き直す前の回答との違いのまとめ (まとめていなければ空)
    compare_verdict: str = ""
    compare_points: tuple[str, ...] = ()

    # 回答を作った段階と、Web 検索の出典
    stage: str = STAGE_LOCAL
    citations: tuple[Citation, ...] = ()

    # 「ChatGPT に聞き直す」ボタン (最新の回答だけ): 出すか、Web 検索か、押せるか、資料を使ったか
    cloud_button: bool = False
    web_button: bool = False
    cloud_enabled: bool = False
    used_rag: bool = False
    rag_allowed: bool = False
    cloud_name: str = ""
    has_cloud_key: bool = False


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


class ChatApp:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root

        # ワーカースレッドから立てて、UIスレッドで見に行く
        self._dirty = threading.Event()

        self.state = ChatState(self._dirty.set)
        self.rag = rag.RagState(self._dirty.set)

        self._rendered: list[_Rendered] = []
        self._busy: bool | None = None
        self._last_refresh = 0.0

        # ChatGPT の元の回答を開いているメッセージの番号
        self._open_originals: set[int] = set()

        # 最新の回答の下に埋め込んだ「ChatGPT に聞き直す」ボタン
        self._cloud_button: ttk.Button | None = None

        refresh_cloud_status(self.state)

        root.title("Local LLM Chat")
        root.geometry("900x700")
        root.minsize(480, 360)
        root.protocol("WM_DELETE_WINDOW", self._close)

        ui = config.load_or_empty("ui.json")

        base = tkfont.nametofont("TkDefaultFont").actual()["family"]
        self.font = tkfont.Font(family=base)
        self.bold = tkfont.Font(family=base, weight="bold")
        self.small = tkfont.Font(family=base)
        self._apply_font_size(_font_size(ui))

        # サーバーの状態はどのタブでも見えるように、タブの外 (上) に置く
        self._build_server_bar()

        # 設定はどのタブからでも開けるように、タブの上の行の右端に置く
        top_bar = ttk.Frame(root, padding=(8, 4, 8, 0))
        top_bar.pack(side="top", fill="x")
        ttk.Button(top_bar, text="設定", command=self._open_settings).pack(
            side="right")

        # チャット画面は Notebook の 1 枚目のタブ。部品の親は chat_frame
        self.notebook = ttk.Notebook(root)
        self.notebook.pack(side="top", fill="both", expand=True)
        self.chat_frame = ttk.Frame(self.notebook)
        self.notebook.add(self.chat_frame, text="チャット")


        # 入力欄は会話表示より先に下から詰める
        # (ウィンドウが狭いときに、入力欄やボタンではなく会話表示が縮むように)
        self._build_rag_bar()
        self._build_input()
        self._build_messages()

        self.default_colors = {
            "background": _to_hex(self.text, self.text.cget("background")),
            "text": _to_hex(self.text, self.text.cget("foreground")),
            **DEFAULT_COLORS,
        }

        # クイズは 2 枚目のタブ (チャットとは状態を共有しない。資料は rag を使う)
        self.quiz_tab = QuizTab(
            self.notebook, self.font, self.bold, lambda: self.colors, self.rag)
        self.notebook.add(self.quiz_tab, text="クイズ")

        # 迷路は 3 枚目のタブ (LLM で日本語の指示を移動命令に訳す)
        self.maze_tab = MazeTab(self.notebook, self.font, self.bold, lambda: self.colors)
        self.notebook.add(self.maze_tab, text="迷路")
        self._apply_colors(_colors(ui, self.default_colors))

        # server.json の managed がオンなら llama-server を起動する
        server_manager.start(self._dirty.set)

        rag.start_indexing(self.rag)
        self._dirty.set()
        self._poll()

    # --- 画面の部品 ---

    def _build_rag_bar(self) -> None:
        """RAG のオン/オフとインデックスの状態"""

        bar = ttk.Frame(self.chat_frame, padding=(8, 6))
        bar.pack(side="top", fill="x")

        # 資料を添えるか (自動ならルーターが質問ごとに判断する)
        ttk.Label(bar, text="資料 (RAG):").pack(side="left")

        self.rag_mode = tk.StringVar(value=self.state.rag_mode.value)

        for label, mode in (("自動", RagMode.AUTO),
                            ("常に", RagMode.ALWAYS),
                            ("使わない", RagMode.NEVER)):
            ttk.Radiobutton(
                bar, text=label, value=mode.value, variable=self.rag_mode,
                command=self._on_rag_mode).pack(side="left", padx=(6, 0))

        self.reload_button = ttk.Button(
            bar, text="再読込", command=lambda: rag.start_indexing(self.rag))
        self.reload_button.pack(side="left", padx=(12, 0))

        self.rag_status = ttk.Label(bar)
        self.rag_status.pack(side="left", padx=(8, 0))

    def _build_server_bar(self) -> None:
        """llama-server が使えないときだけ出す状態の行"""

        self.server_bar = ttk.Frame(self.root, padding=(8, 0))
        self.server_bar.pack(side="top", fill="x")
        self.server_bar.columnconfigure(0, weight=1)

        self.server_lines = [
            ttk.Label(self.server_bar, wraplength=860) for _ in range(2)]
        self.server_retry = ttk.Button(
            self.server_bar, text="もう一度起動", command=server_manager.retry)

    def _build_messages(self) -> None:
        """会話表示"""

        frame = ttk.Frame(self.chat_frame, padding=(8, 0))
        frame.pack(side="top", fill="both", expand=True)

        self.text = tk.Text(
            frame, wrap="char", font=self.font, padx=8, pady=8,
            borderwidth=1, relief="solid", state="disabled", cursor="arrow")
        scrollbar = ttk.Scrollbar(
            frame, orient="vertical", command=self.text.yview)
        self.text.configure(yscrollcommand=scrollbar.set)

        scrollbar.pack(side="right", fill="y")
        self.text.pack(side="left", fill="both", expand=True)

        # 色は _apply_colors で付ける
        self.text.tag_configure("user", font=self.bold)
        self.text.tag_configure("ai", font=self.bold)
        self.text.tag_configure(
            "note", font=self.small, lmargin1=12, lmargin2=12)
        self.text.tag_configure("separator", font=self.small)
        self.text.tag_configure("content", spacing3=4)
        self.text.tag_configure("superseded", spacing3=4)
        self.text.tag_configure("toggle", font=self.small, underline=True)
        self.text.tag_configure(
            "comparison", font=self.small, lmargin1=12, lmargin2=24)
        self.text.tag_configure("conflict", font=self.small)
        self.text.tag_configure(
            "original", font=self.small, lmargin1=12, lmargin2=12, spacing3=4)
        self.text.tag_configure(
            "link", font=self.small, underline=True, lmargin1=12, lmargin2=24)
        self.text.tag_bind("toggle", "<Button-1>", self._toggle_original)
        self.text.tag_bind("link", "<Button-1>", self._open_citation)

        for clickable in ("toggle", "link"):
            self.text.tag_bind(
                clickable, "<Enter>", lambda _e: self.text.configure(cursor="hand2"))
            self.text.tag_bind(
                clickable, "<Leave>", lambda _e: self.text.configure(cursor="arrow"))

        # 右クリックでコピー (macOS は Button-2 と Control+クリック)
        self.menu = tk.Menu(self.root, tearoff=False)
        self.menu.add_command(label="コピー", command=self._copy_message)
        self._menu_index = -1

        for sequence in ("<Button-3>", "<Button-2>", "<Control-Button-1>"):
            self.text.bind(sequence, self._show_menu)

    def _build_input(self) -> None:
        self.error_label = ttk.Label(
            self.chat_frame, padding=(8, 4, 8, 0), wraplength=860)

        frame = ttk.Frame(self.chat_frame, padding=8)
        frame.pack(side="bottom", fill="x")

        # エラーは入力欄のすぐ上
        self.error_label.pack(side="bottom", fill="x")

        buttons = ttk.Frame(frame)
        buttons.pack(side="right", fill="y", padx=(8, 0))

        self.send_button = ttk.Button(buttons, width=6)
        self.send_button.pack(side="top", fill="both", expand=True)

        self.clear_button = ttk.Button(
            buttons, text="クリア", width=6, command=self._clear)
        self.clear_button.pack(side="top", fill="both", expand=True,
                               pady=(6, 0))

        # 入力欄 (Enterで送信 / Shift+Enterで改行)
        self.input = tk.Text(
            frame, height=4, wrap="char", font=self.font, padx=6, pady=6,
            borderwidth=1, relief="solid", undo=True)
        self.input.pack(side="left", fill="both", expand=True)
        self.input.bind("<Return>", self._on_return)
        self.input.bind("<Shift-Return>", self._on_shift_return)
        self.input.focus_set()

    # --- 操作 ---

    def _apply_font_size(self, size: int) -> None:
        """文字の大きさを変える (フォントを使っている所はすべて描き直される)"""

        self.font_size = size
        self.font.configure(size=size)
        self.bold.configure(size=size)
        self.small.configure(size=max(size - 3, 1))

    def _apply_colors(self, colors: dict[str, str]) -> None:
        """色を変える"""

        self.colors = dict(colors)

        for widget in (self.text, self.input):
            widget.configure(
                background=colors["background"], foreground=colors["text"],
                insertbackground=colors["text"])

        self.text.tag_configure("user", foreground=colors["user"])
        self.text.tag_configure("ai", foreground=colors["ai"])

        for tag in ("thinking", "note", "separator", "superseded", "toggle", "comparison", "link"):
            self.text.tag_configure(tag, foreground=colors["sub_text"])

        # 違いのまとめの「食い違いあり」は目立たせる
        self.text.tag_configure("conflict", foreground=colors["error"])

        self.rag_status.configure(foreground=colors["sub_text"])
        self.error_label.configure(foreground=colors["error"])
        self.quiz_tab.apply_colors()
        self.maze_tab.apply_colors()

    def _open_settings(self) -> None:
        SettingsDialog(
            self.root, self.font, self.font_size, self._apply_font_size,
            self.colors, self.default_colors, self._apply_colors,
            self._on_settings_reset, self._on_settings_saved)

    def _on_settings_saved(self) -> None:
        """保存したら、API キーの有無とサーバーの設定を読み直す"""

        refresh_cloud_status(self.state)
        server_manager.reload()
        self._dirty.set()

    def _on_settings_reset(self) -> None:
        """設定を既定値に戻したあと、画面の設定を読み直して設定画面を開き直す"""

        refresh_cloud_status(self.state)
        server_manager.reload()
        ui = config.load_or_empty("ui.json")
        self._apply_font_size(_font_size(ui))
        self._apply_colors(_colors(ui, self.default_colors))
        self._open_settings()

    def _on_rag_mode(self) -> None:
        self.state.rag_mode = RagMode(self.rag_mode.get())

    def _on_return(self, _event: tk.Event) -> str:
        if not self.state.busy:
            self._send()
        return "break"

    def _on_shift_return(self, _event: tk.Event) -> str:
        self.input.insert("insert", "\n")
        self.input.see("insert")
        return "break"

    def _send(self) -> None:
        prompt = self.input.get("1.0", "end-1c")

        # サーバーの準備ができるまでは送らない (入力は残す)
        if not prompt.strip() or not server_manager.is_chat_available():
            return

        self.input.delete("1.0", "end")
        send_prompt(self.state, self.rag, prompt)
        self._dirty.set()

    def _cancel(self) -> None:
        self.state.cancel.cancel()

    def _ask_cloud(self, web_search: bool = False) -> None:
        with self.state.lock:
            if not can_ask_cloud(self.state):
                return
            used_rag = self.state.messages[-1].used_rag

            # 資料を外に送らない設定のときは、押せない (ボタンも無効にしている)
            if used_rag and not self.state.allow_rag:
                return

        # 資料を使った回答は、送る前に確認する
        if used_rag and not messagebox.askyesno(
                "資料を送る確認",
                f"資料 (rag/) の内容が {self.state.cloud_name} に送られます。送りますか？\n"
                "(ほかの、資料を使ったやりとりは送りません)",
                default="no", parent=self.root):
            return

        ask_cloud(self.state, web_search)
        self._dirty.set()

    def _open_citation(self, event: tk.Event) -> str:
        index = self.text.index(f"@{event.x},{event.y}")

        for tag in self.text.tag_names(index):
            if tag.startswith("citation"):
                message, number = map(int, tag[len("citation"):].split("_"))
                webbrowser.open(self._rendered[message].citations[number].url)

        return "break"

    def _toggle_original(self, event: tk.Event) -> str:
        index = self.text.index(f"@{event.x},{event.y}")

        for tag in self.text.tag_names(index):
            if tag.startswith("msg"):
                self._open_originals ^= {int(tag[3:])}
                self._dirty.set()

        return "break"

    def _clear(self) -> None:
        with self.state.lock:
            self.state.messages.clear()
            self.state.error = ""
        self._open_originals.clear()
        self.input.focus_set()
        self._dirty.set()

    def _show_menu(self, event: tk.Event) -> str:
        index = self.text.index(f"@{event.x},{event.y}")

        self._menu_index = -1
        for tag in self.text.tag_names(index):
            if tag.startswith("msg"):
                self._menu_index = int(tag[3:])

        if self._menu_index >= 0:
            self.menu.tk_popup(event.x_root, event.y_root)
        return "break"

    def _copy_message(self) -> None:
        with self.state.lock:
            if not 0 <= self._menu_index < len(self.state.messages):
                return
            content = self.state.messages[self._menu_index].content

        self.root.clipboard_clear()
        self.root.clipboard_append(content)

    def _close(self) -> None:
        # 通信中なら止める (ワーカーはデーモンスレッドなので待たない)
        self.state.cancel.cancel()
        self.rag.cancel.cancel()
        self.quiz_tab.shutdown()
        self.maze_tab.shutdown()

        # サーバーの停止に数秒かかることがあるので、先にウィンドウを閉じる
        self.root.withdraw()
        server_manager.stop()
        self.root.destroy()

    # --- 描画 ---

    def _poll(self) -> None:
        # 受信中は、何も届かなくても経過時間が進むように描き直す
        # (表示が変わらなければ _render_messages は何もしない)
        now = time.monotonic()

        if self._busy and now - self._last_refresh >= STATUS_REFRESH_SECONDS:
            self._dirty.set()

        if self._dirty.is_set():
            self._dirty.clear()
            self._last_refresh = now
            self._refresh()
        self.root.after(POLL_INTERVAL_MS, self._poll)

    def _refresh(self) -> None:
        with self.state.lock:
            busy = self.state.busy
            error = self.state.error
            restored = self.state.restored_prompt
            self.state.restored_prompt = ""
            cloud_enabled = self.state.has_cloud_key and can_ask_cloud(self.state)
            last = len(self.state.messages) - 1
            stages = [next_stage(self.state, m) for m in self.state.messages]
            snapshot: list[_Rendered] = [
                _Rendered(
                    m.role, m.agent, m.content, m.note,
                    busy and not m.content and not m.status,
                    status=format_status(m),
                    superseded=m.superseded,
                    cloud_original="" if m.superseded else m.cloud_original,
                    original_open=i in self._open_originals,
                    compare_verdict="" if m.superseded else m.compare_verdict,
                    compare_points=tuple(m.compare_points),
                    stage=m.stage,
                    citations=() if m.superseded else tuple(m.citations),
                    cloud_button=(i == last and m.role == "assistant" and
                                  bool(m.agent_key) and not m.superseded and
                                  bool(stages[i])),
                    web_button=stages[i] == STAGE_WEB,
                    cloud_enabled=(i == last and cloud_enabled and
                                   (not m.used_rag or self.state.allow_rag)),
                    used_rag=m.used_rag,
                    rag_allowed=self.state.allow_rag,
                    cloud_name=self.state.cloud_name,
                    has_cloud_key=self.state.has_cloud_key,
                )
                for i, m in enumerate(self.state.messages)
            ]

        with self.rag.lock:
            building = self.rag.building
            status = self.rag.status

        self._refresh_server_bar()
        self.rag_status.configure(text=status)
        self.reload_button.state(["disabled" if building else "!disabled"])
        self.error_label.configure(text=f"Error: {error}" if error else "")

        if busy != self._busy:
            self._busy = busy
            if busy:
                self.send_button.configure(text="中断", command=self._cancel)
            else:
                self.send_button.configure(text="送信", command=self._send)
            self.clear_button.state(["disabled" if busy else "!disabled"])

        self.send_button.state([
            "disabled" if not busy and not server_manager.is_chat_available()
            else "!disabled"])

        if restored:
            self.input.delete("1.0", "end")
            self.input.insert("1.0", restored)
            self.input.focus_set()

        self._render_messages(snapshot)

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

    def _render_messages(self, snapshot: list[_Rendered]) -> None:
        # 変わった最初のメッセージから下だけを描き直す
        # (ストリーミング中は最後の1件だけになる)
        first = 0
        while (first < len(snapshot) and first < len(self._rendered) and
               snapshot[first] == self._rendered[first]):
            first += 1

        if first == len(snapshot) == len(self._rendered):
            return

        # 一番下までスクロールしていたら追従する
        at_bottom = self.text.yview()[1] >= 0.999

        self.text.configure(state="normal")

        start = f"msg{first}.start" if first < len(self._rendered) else "end-1c"
        # "end" まで消すと Tk は末尾の改行を残すために手前の改行を消すので、
        # 末尾の改行の手前まで消す
        self.text.delete(start, "end-1c")

        # 埋め込んだボタンは、描き直すたびに作り直す
        if self._cloud_button is not None:
            self._cloud_button.destroy()
            self._cloud_button = None

        for i in range(first, len(snapshot)):
            self._insert_message(i, snapshot[i])

        self.text.configure(state="disabled")
        self._rendered = snapshot

        if at_bottom:
            self.text.see("end")

    def _insert_message(self, index: int, message: _Rendered) -> None:
        text = self.text
        tag = f"msg{index}"

        text.mark_set(f"msg{index}.start", "end-1c")
        text.mark_gravity(f"msg{index}.start", "left")

        # AI の回答には、答えたエージェントの名前を出す
        if message.role == "user":
            text.insert("end", "You\n", ("user", tag))
        elif message.superseded:
            # 聞き直す前の回答は、薄く残す
            label = "ローカルの回答" if message.stage == STAGE_LOCAL else "Web 検索なしの回答"
            text.insert(
                "end", f"{message.agent or 'AI'} ({label})\n", ("superseded", tag))
        else:
            text.insert("end", (message.agent or "AI") + "\n", ("ai", tag))

        if message.status:
            text.insert("end", message.status + "\n", ("thinking", tag))

        if message.thinking:
            text.insert("end", "考え中...\n", ("thinking", tag))
        elif message.content or not message.status:
            content_tag = "superseded" if message.superseded else "content"
            text.insert("end", message.content + "\n", (content_tag, tag))

        # 書き直す前の ChatGPT の回答 (書き直しで中身が変わっていないか確かめられるように)
        if message.cloud_original:
            arrow = "▼" if message.original_open else "▶"
            text.insert(
                "end", f"{arrow} {message.cloud_name} の元の回答\n", ("toggle", tag))

            if message.original_open:
                text.insert(
                    "end", message.cloud_original + "\n", ("original", tag))

        # Web 検索の出典 (クリックでブラウザーを開く)
        if message.citations:
            text.insert("end", "出典:\n", ("note", tag))

            for number, citation in enumerate(message.citations):
                text.insert(
                    "end", f"[{number + 1}] {citation.title}\n",
                    ("link", tag, f"citation{index}_{number}"))

        # 聞き直す前の回答との違いのまとめ
        if message.compare_verdict:
            before = "Web 検索なしの回答" if message.stage == STAGE_WEB else "聞き直す前の回答"
            text.insert(
                "end", f"{before}との違い (ローカルの LLM による目安): ",
                ("comparison", tag))
            verdict_tag = "conflict" if message.compare_verdict == "食い違いあり" else "comparison"
            text.insert("end", message.compare_verdict + "\n", (verdict_tag, tag))

            for point in message.compare_points:
                text.insert("end", f"・{point}\n", ("comparison", tag))

        if message.note:
            text.insert("end", message.note + "\n", ("note", tag))

        if message.cloud_button:
            self._insert_cloud_button(message, tag)

        text.insert("end", "─" * 40 + "\n", ("separator",))


    def _insert_cloud_button(self, message: _Rendered, tag: str) -> None:
        """最新の回答の下に「ChatGPT に聞き直す」ボタンを埋め込む"""

        button = ttk.Button(
            self.text,
            text="Web でも調べて比べる" if message.web_button
                 else f"{message.cloud_name} に聞き直す",
            command=lambda web_search=message.web_button: self._ask_cloud(web_search))

        if not message.cloud_enabled:
            button.state(["disabled"])

        self.text.window_create("end", window=button, padx=2, pady=2)
        self._cloud_button = button

        # Tk にはツールチップが無いので、押せない理由などをボタンの横に書く
        if not message.has_cloud_key:
            hint = "  設定画面で API キーを登録すると使えます"
        elif message.used_rag and message.rag_allowed:
            hint = "  資料 (rag/) の内容も送ります (送る前に確認します)"
        elif message.used_rag:
            hint = "  資料を使った回答は、資料を外に送らないため聞き直せません"
        else:
            hint = ""

        self.text.insert("end", hint + "\n", ("note", tag))


def main() -> None:
    root = tk.Tk()
    ChatApp(root)

    try:
        root.mainloop()
    finally:
        # Ctrl+C などでも、起動したサーバーを残さない
        server_manager.stop()
