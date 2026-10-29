"""チャット画面の組み立て (Tk)"""

from __future__ import annotations

import threading
import tkinter as tk
import tkinter.font as tkfont
from tkinter import ttk

import rag
from chat import ChatState, RagMode, send_prompt

FONT_SIZE = 14

# ワーカースレッドからの更新を見に行く間隔 (ミリ秒)
POLL_INTERVAL_MS = 30

USER_COLOR = "#4a90e2"
AI_COLOR = "#3fa34d"
NOTE_COLOR = "#888888"
ERROR_COLOR = "#e05555"

# 画面に表示した1件分 (変わった所から下だけを描き直す)
_Rendered = tuple[str, str, str, str, bool]


class ChatApp:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root

        # ワーカースレッドから立てて、UIスレッドで見に行く
        self._dirty = threading.Event()

        self.state = ChatState(self._dirty.set)
        self.rag = rag.RagState(self._dirty.set)

        self._rendered: list[_Rendered] = []
        self._busy: bool | None = None

        root.title("Local LLM Chat")
        root.geometry("900x700")
        root.minsize(480, 360)
        root.protocol("WM_DELETE_WINDOW", self._close)

        base = tkfont.nametofont("TkDefaultFont").actual()["family"]
        self.font = tkfont.Font(family=base, size=FONT_SIZE)
        self.bold = tkfont.Font(family=base, size=FONT_SIZE, weight="bold")
        self.small = tkfont.Font(family=base, size=FONT_SIZE - 3)

        self._build_rag_bar()
        self._build_messages()
        self._build_input()

        rag.start_indexing(self.rag)
        self._dirty.set()
        self._poll()

    # ---- 画面の部品 ----

    def _build_rag_bar(self) -> None:
        """RAG のオン/オフとインデックスの状態"""

        bar = ttk.Frame(self.root, padding=(8, 6))
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

        self.rag_status = ttk.Label(bar, foreground=NOTE_COLOR)
        self.rag_status.pack(side="left", padx=(8, 0))

    def _build_messages(self) -> None:
        """会話表示"""

        frame = ttk.Frame(self.root, padding=(8, 0))
        frame.pack(side="top", fill="both", expand=True)

        self.text = tk.Text(
            frame, wrap="char", font=self.font, padx=8, pady=8,
            borderwidth=1, relief="solid", state="disabled", cursor="arrow")
        scrollbar = ttk.Scrollbar(
            frame, orient="vertical", command=self.text.yview)
        self.text.configure(yscrollcommand=scrollbar.set)

        scrollbar.pack(side="right", fill="y")
        self.text.pack(side="left", fill="both", expand=True)

        self.text.tag_configure("user", foreground=USER_COLOR, font=self.bold)
        self.text.tag_configure("ai", foreground=AI_COLOR, font=self.bold)
        self.text.tag_configure(
            "thinking", foreground=NOTE_COLOR)
        self.text.tag_configure(
            "note", foreground=NOTE_COLOR, font=self.small,
            lmargin1=12, lmargin2=12)
        self.text.tag_configure(
            "separator", foreground=NOTE_COLOR, font=self.small)
        self.text.tag_configure("content", spacing3=4)

        # 右クリックでコピー (macOS は Button-2 と Control+クリック)
        self.menu = tk.Menu(self.root, tearoff=False)
        self.menu.add_command(label="コピー", command=self._copy_message)
        self._menu_index = -1

        for sequence in ("<Button-3>", "<Button-2>", "<Control-Button-1>"):
            self.text.bind(sequence, self._show_menu)

    def _build_input(self) -> None:
        self.error_label = ttk.Label(
            self.root, foreground=ERROR_COLOR, padding=(8, 4, 8, 0),
            wraplength=860)
        self.error_label.pack(side="top", fill="x")

        frame = ttk.Frame(self.root, padding=8)
        frame.pack(side="top", fill="x")

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

    # ---- 操作 ----

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

        if not prompt.strip():
            return

        self.input.delete("1.0", "end")
        send_prompt(self.state, self.rag, prompt)
        self._dirty.set()

    def _cancel(self) -> None:
        self.state.cancel.cancel()

    def _clear(self) -> None:
        with self.state.lock:
            self.state.messages.clear()
            self.state.error = ""
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
        self.root.destroy()

    # ---- 描画 ----

    def _poll(self) -> None:
        if self._dirty.is_set():
            self._dirty.clear()
            self._refresh()
        self.root.after(POLL_INTERVAL_MS, self._poll)

    def _refresh(self) -> None:
        with self.state.lock:
            busy = self.state.busy
            error = self.state.error
            restored = self.state.restored_prompt
            self.state.restored_prompt = ""
            snapshot: list[_Rendered] = [
                (m.role, m.agent, m.content, m.note,
                 busy and not m.content)
                for m in self.state.messages
            ]

        with self.rag.lock:
            building = self.rag.building
            status = self.rag.status

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

        self.text.configure(state="normal")

        start = f"msg{first}.start" if first < len(self._rendered) else "end-1c"
        # "end" まで消すと Tk は末尾の改行を残すために手前の改行を消すので、
        # 末尾の改行の手前まで消す
        self.text.delete(start, "end-1c")

        for i in range(first, len(snapshot)):
            self._insert_message(i, snapshot[i])

        self.text.configure(state="disabled")
        self._rendered = snapshot

        if at_bottom:
            self.text.see("end")

    def _insert_message(self, index: int, message: _Rendered) -> None:
        role, agent, content, note, thinking = message
        text = self.text
        tag = f"msg{index}"

        text.mark_set(f"msg{index}.start", "end-1c")
        text.mark_gravity(f"msg{index}.start", "left")

        # AI の回答には、答えたエージェントの名前を出す
        if role == "user":
            text.insert("end", "You\n", ("user", tag))
        else:
            text.insert("end", (agent or "AI") + "\n", ("ai", tag))

        if thinking:
            text.insert("end", "考え中...\n", ("thinking", tag))
        else:
            text.insert("end", content + "\n", ("content", tag))

        if note:
            text.insert("end", note + "\n", ("note", tag))

        text.insert("end", "─" * 40 + "\n", ("separator",))


def main() -> None:
    root = tk.Tk()
    ChatApp(root)
    root.mainloop()
