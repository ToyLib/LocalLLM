"""チャットタブの画面 (Tk)"""

from __future__ import annotations

import time
import tkinter as tk
import webbrowser
from tkinter import messagebox, ttk
from typing import NamedTuple

from common import rag
from common import server_manager
from chat.chat import (
    STAGE_LOCAL, STAGE_WEB, ChatState, RagMode, ask_cloud, can_ask_cloud,
    format_status, next_stage, refresh_cloud_status, send_prompt)
from common.llm_client import Citation
from common.tab import Tab, UiContext
from common.widgets import editable, scrolled_text, set_enabled

# ワーカースレッドからの更新を見に行く間隔 (ミリ秒)
POLL_INTERVAL_MS = 30

# 受信中に、経過時間の表示を進めるために描き直す間隔 (秒)
STATUS_REFRESH_SECONDS = 0.5


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


class ChatTab(Tab):
    """ローカルの LLM と話す画面 (資料の行・会話の表示・入力欄)"""

    label = "チャット"

    def __init__(
        self, parent: tk.Misc, context: UiContext, rag_state: rag.RagState,
    ) -> None:
        super().__init__(parent, context)

        self.font = context.font
        self.bold = context.bold
        self.small = context.small

        self.state = ChatState(self._dirty.set)
        self.rag = rag_state

        self._rendered: list[_Rendered] = []
        self._busy: bool | None = None
        self._last_refresh = 0.0

        # ChatGPT の元の回答を開いているメッセージの番号
        self._open_originals: set[int] = set()

        # 最新の回答の下に埋め込んだ「ChatGPT に聞き直す」ボタン
        self._cloud_button: ttk.Button | None = None

        refresh_cloud_status(self.state)

        # 入力欄は会話表示より先に下から詰める
        # (ウィンドウが狭いときに、入力欄やボタンではなく会話表示が縮むように)
        self._build_rag_bar()
        self._build_input()
        self._build_messages()

        # 色は Application が apply_colors で付けたあとに、最初の描画をする
        self._dirty.set()
        self.after(POLL_INTERVAL_MS, self._poll)

    # --- 画面の部品 ---

    def _build_rag_bar(self) -> None:
        """RAG のオン/オフとインデックスの状態"""

        bar = ttk.Frame(self, padding=(8, 6))
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

    def _build_messages(self) -> None:
        """会話表示"""

        frame = ttk.Frame(self, padding=(8, 0))
        frame.pack(side="top", fill="both", expand=True)

        self.text = scrolled_text(
            frame, wrap="char", font=self.font, padx=8, pady=8,
            borderwidth=1, relief="solid", state="disabled", cursor="arrow")

        # 色は apply_colors で付ける
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
        self.menu = tk.Menu(self, tearoff=False)
        self.menu.add_command(label="コピー", command=self._copy_message)
        self._menu_index = -1

        for sequence in ("<Button-3>", "<Button-2>", "<Control-Button-1>"):
            self.text.bind(sequence, self._show_menu)

    def _build_input(self) -> None:
        self.error_label = ttk.Label(
            self, padding=(8, 4, 8, 0), wraplength=860)

        frame = ttk.Frame(self, padding=8)
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

    def apply_colors(self) -> None:
        """色を Application が決めた色に合わせる"""

        colors = self._context.colors()

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

    def on_settings_saved(self) -> None:
        """保存したら、API キーの有無を読み直す"""

        refresh_cloud_status(self.state)
        self._dirty.set()

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
                default="no", parent=self.winfo_toplevel()):
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

        self.clipboard_clear()
        self.clipboard_append(content)

    def shutdown(self) -> None:
        # 通信中なら止める (ワーカーはデーモンスレッドなので待たない)
        self.state.cancel.cancel()

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
        self.after(POLL_INTERVAL_MS, self._poll)

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

        self.rag_status.configure(text=status)
        set_enabled(self.reload_button, not building)
        self.error_label.configure(text=f"Error: {error}" if error else "")

        if busy != self._busy:
            self._busy = busy
            if busy:
                self.send_button.configure(text="中断", command=self._cancel)
            else:
                self.send_button.configure(text="送信", command=self._send)
            set_enabled(self.clear_button, not busy)

        set_enabled(self.send_button, busy or server_manager.is_chat_available())

        if restored:
            self.input.delete("1.0", "end")
            self.input.insert("1.0", restored)
            self.input.focus_set()

        self._render_messages(snapshot)

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

        with editable(self.text):
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
