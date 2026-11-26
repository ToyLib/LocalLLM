"""箱庭タブ。LLM の問い合わせだけを別スレッドで実行する。"""

import queue
import threading
import time
import tkinter as tk
from tkinter import ttk

from common import server_manager
from common.llm_client import CancelToken
from common.tab import Tab, UiContext
from common.widgets import set_enabled
from sandbox.sandbox_mode import EXAMPLES, Harbor, interpret
from sandbox.sandbox_scene import HarborCanvas

FRAME_MS = 33


class SandboxTab(Tab):
    label = "箱庭"

    def __init__(self, parent: tk.Misc, context: UiContext) -> None:
        super().__init__(parent, context)
        self._harbor = Harbor()
        self._cancel: CancelToken | None = None
        self._results: queue.SimpleQueue[tuple[str, str]] = queue.SimpleQueue()
        self._last_frame = time.monotonic()
        self._error = ""
        self._input = tk.StringVar(master=self)
        self._input.trace_add("write", lambda *_: self.wake())

        controls = ttk.Frame(self, padding=(8, 6))
        controls.pack(side="top", fill="x")
        self._presets = []
        for caption, command in (("昼の港", "day"), ("吹雪の夜", "blizzard_night")):
            button = ttk.Button(controls, text=caption, command=lambda key=command: self._apply(key))
            button.pack(side="left", padx=(0, 6))
            self._presets.append(button)

        bottom = ttk.Frame(self, padding=(8, 4, 8, 8))
        bottom.pack(side="bottom", fill="x")
        self._entry = ttk.Entry(bottom, textvariable=self._input, font=context.font)
        self._entry.pack(side="left", expand=True, fill="x")
        self._entry.bind("<Return>", self._send)
        self._send_button = ttk.Button(bottom, text="指示する", command=self._send)
        self._send_button.pack(side="right", padx=(6, 0))

        self._status = ttk.Label(self, padding=(8, 0), font=context.small)
        self._status.pack(side="bottom", fill="x")

        self._canvas = HarborCanvas(self, highlightthickness=0, width=480, height=270)
        self._canvas.pack(side="top", fill="both", expand=True, padx=8)
        self._canvas.bind("<Configure>", self._resize)
        self._canvas.bind("<Map>", lambda _: self.wake())

        self._available = server_manager.is_chat_available()
        self._refresh_controls()
        self._timer = self.after(FRAME_MS, self._poll)

    def _resize(self, event: tk.Event) -> None:
        self._status.configure(wraplength=event.width)
        self.wake()

    def _apply(self, command: str) -> None:
        self._harbor.apply(command)
        self._error = EXAMPLES if command == "unsupported" else ""
        self.wake()

    def _send(self, _event: tk.Event | None = None) -> None:
        text = self._input.get().strip()
        if not text or self._cancel is not None or not server_manager.is_chat_available():
            return
        cancel = CancelToken()
        self._cancel = cancel
        self._error = ""
        try:
            threading.Thread(target=self._interpret, args=(text, cancel), daemon=True).start()
        except RuntimeError as exc:
            self._cancel = None
            self._error = f"指示を送信できませんでした。{exc}"
        self._refresh_controls()

    def _interpret(self, text: str, cancel: CancelToken) -> None:
        try:
            command = interpret(text, cancel)
        except Exception as exc:
            self._results.put(("", f"風景を切り替えられませんでした。{exc}"))
        else:
            self._results.put((command, ""))

    def _take_result(self) -> None:
        try:
            command, error = self._results.get_nowait()
        except queue.Empty:
            return
        self._cancel = None
        if error:
            self._error = error
        else:
            self._apply(command)
            if command != "unsupported":
                self._input.set("")
        self.wake()

    def _refresh_controls(self) -> None:
        busy = self._cancel is not None
        for button in self._presets:
            set_enabled(button, not busy)
        self._entry.configure(state="readonly" if busy else "normal")
        set_enabled(self._send_button, not busy and self._available and bool(self._input.get().strip()))

        if busy:
            status = "ローカルLLMが指示を読み取っています…"
        elif self._error:
            status = self._error
        elif not self._available:
            status = "ローカルLLMに接続できません。ボタンで切り替えられます。"
        else:
            status = EXAMPLES
        colors = self._context.colors()
        self._status.configure(text=status, foreground=colors["error" if self._error else "sub_text"])

    def _poll(self) -> None:
        now = time.monotonic()
        elapsed = min(now - self._last_frame, 0.05)
        self._last_frame = now
        self._take_result()
        if self._canvas.winfo_ismapped():
            available = server_manager.is_chat_available()
            if available != self._available:
                self._available = available
                self.wake()
            dirty = self._dirty.is_set()
            if dirty:
                self._dirty.clear()
                self._refresh_controls()
            animating = self._harbor.animating
            if animating:
                self._harbor.advance(elapsed)
            if dirty or animating:
                self._canvas.draw(self._harbor)
        self._timer = self.after(FRAME_MS, self._poll)

    def apply_colors(self) -> None:
        self._canvas.configure(background=self._context.colors()["background"])
        self.wake()

    def shutdown(self) -> None:
        self.after_cancel(self._timer)
        if self._cancel is not None:
            self._cancel.cancel()
