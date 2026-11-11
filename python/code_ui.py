"""コーディング練習タブの画面 (Tk)"""

from __future__ import annotations

import threading
import tkinter as tk
import tkinter.font as tkfont
from tkinter import ttk
from typing import Callable

import code_mode
from code_mode import CodeState

# ワーカースレッドからの更新を見に行く間隔 (ミリ秒)
POLL_INTERVAL_MS = 50

NOTICE = ("生成されたコードはあなたのパソコンでそのまま動きます。"
          "内容を読んでから実行してください (自己責任で使ってください)。")


class CodeTab(ttk.Frame):
    """プロンプトからコードを作り、解説を読み、実行して、編集して試す画面"""

    def __init__(
        self,
        parent: tk.Misc,
        font: tkfont.Font,
        colors: Callable[[], dict[str, str]],
    ) -> None:
        super().__init__(parent)

        self._font = font
        self._colors = colors
        self._dirty = threading.Event()
        self.state = CodeState(self._dirty.set)

        # コードと実行結果は等幅の字体で、大きさは会話の文字に合わせる
        fixed = tkfont.nametofont("TkFixedFont").actual()["family"]
        self.code_font = tkfont.Font(family=fixed, size=font.cget("size"))

        # 画面に出した値 (変わったときだけ描き直す)
        self._shown_version = -1
        self._shown_explanation: str | None = None
        self._shown_output: str | None = None

        self._build()
        self._poll()

    # --- 画面の部品 ---

    def _build(self) -> None:
        ttk.Label(self, text=NOTICE, padding=(8, 6, 8, 0),
                  wraplength=860).pack(side="top", fill="x")

        # 作りたいもの
        top = ttk.Frame(self, padding=(8, 6))
        top.pack(side="top", fill="x")
        self.generate_button = ttk.Button(
            top, text="作る", width=6, command=self._generate)
        self.generate_button.pack(side="right", fill="y", padx=(8, 0))
        self.prompt = tk.Text(
            top, height=3, wrap="char", font=self._font, padx=6, pady=6,
            borderwidth=1, relief="solid", undo=True)
        self.prompt.pack(side="left", fill="x", expand=True)
        self.prompt.bind("<Return>", self._on_return)
        self.prompt.bind("<Shift-Return>", lambda _e: None)

        # 状態とエラーは一番下
        self.status = ttk.Label(self, padding=(8, 0, 8, 6), wraplength=860)
        self.status.pack(side="bottom", fill="x")

        # 実行結果
        output_frame = ttk.LabelFrame(self, text="実行結果", padding=4)
        output_frame.pack(side="bottom", fill="x", padx=8)
        self.output = tk.Text(
            output_frame, height=8, wrap="char", font=self.code_font, padx=6,
            pady=4, borderwidth=1, relief="solid", state="disabled")
        output_scroll = ttk.Scrollbar(
            output_frame, orient="vertical", command=self.output.yview)
        self.output.configure(yscrollcommand=output_scroll.set)
        output_scroll.pack(side="right", fill="y")
        self.output.pack(side="left", fill="x", expand=True)

        # コードの操作ボタン
        buttons = ttk.Frame(self, padding=(8, 6, 8, 4))
        buttons.pack(side="bottom", fill="x")
        self.run_button = ttk.Button(buttons, text="実行", command=self._run)
        self.run_button.pack(side="left")
        self.stop_button = ttk.Button(buttons, text="停止", command=self._stop)
        self.stop_button.pack(side="left", padx=(6, 0))
        self.revert_button = ttk.Button(
            buttons, text="元に戻す", command=self._revert)
        self.revert_button.pack(side="left", padx=(6, 0))
        self.explain_button = ttk.Button(
            buttons, text="解説を作り直す", command=self._explain)
        self.explain_button.pack(side="left", padx=(6, 0))

        # コードと解説は左右に並べる
        panes = ttk.PanedWindow(self, orient="horizontal")
        panes.pack(side="top", fill="both", expand=True, padx=8)

        code_frame = ttk.LabelFrame(panes, text="コード (編集できます)", padding=4)
        explain_frame = ttk.LabelFrame(panes, text="解説", padding=4)
        panes.add(code_frame, weight=3)
        panes.add(explain_frame, weight=2)

        scroll = ttk.Scrollbar(code_frame, orient="vertical")
        scroll.pack(side="right", fill="y")

        # 行番号 (解説の「N 行目」と見比べられるように)
        self.gutter = tk.Text(
            code_frame, width=4, wrap="none", font=self.code_font, padx=4,
            pady=6, borderwidth=0, state="disabled", cursor="arrow",
            takefocus=0)
        self.gutter.pack(side="left", fill="y")
        self.gutter.tag_configure("number", justify="right")

        self.code = tk.Text(
            code_frame, wrap="none", font=self.code_font, padx=6, pady=6,
            borderwidth=1, relief="solid", undo=True, tabs=("4c",))
        self.code.pack(side="left", fill="both", expand=True)
        self.code.configure(yscrollcommand=lambda *a: self._on_code_scroll(scroll, *a))
        scroll.configure(command=self._scroll_code)
        self.code.bind("<<Modified>>", self._on_code_modified)

        explain_scroll = ttk.Scrollbar(explain_frame, orient="vertical")
        explain_scroll.pack(side="right", fill="y")
        self.explanation = tk.Text(
            explain_frame, wrap="char", font=self._font, padx=8, pady=8,
            borderwidth=1, relief="solid", state="disabled", cursor="arrow",
            yscrollcommand=explain_scroll.set)
        explain_scroll.configure(command=self.explanation.yview)
        self.explanation.pack(side="left", fill="both", expand=True)

        # 色は ChatApp が apply_colors で付ける
        self._update_buttons()

    def apply_font_size(self, size: int) -> None:
        """会話の文字の大きさに合わせる (呼び出し元が font を変えたあとに呼ぶ)"""

        self.code_font.configure(size=size)

    def apply_colors(self) -> None:
        """色を会話画面に合わせる"""

        colors = self._colors()

        for widget in (self.prompt, self.code, self.explanation, self.output):
            widget.configure(
                background=colors["background"], foreground=colors["text"],
                insertbackground=colors["text"])

        self.gutter.configure(
            background=colors["background"], foreground=colors["sub_text"])
        self.status.configure(foreground=colors["sub_text"])

    # --- 操作 ---

    def _on_return(self, _event: tk.Event) -> str:
        self._generate()
        return "break"

    def _generate(self) -> None:
        prompt = self.prompt.get("1.0", "end-1c").strip()

        if not prompt or self.state.busy or self.state.running:
            return

        # 編集中のコードを、修正の頼みの土台にする
        with self.state.lock:
            self.state.code = self.code.get("1.0", "end-1c")

        code_mode.start_generate(self.state, prompt)

    def _explain(self) -> None:
        with self.state.lock:
            self.state.code = self.code.get("1.0", "end-1c")

        code_mode.start_explain(self.state)

    def _run(self) -> None:
        code_mode.start_run(self.state, self.code.get("1.0", "end-1c"))

    def _stop(self) -> None:
        if self.state.busy:
            code_mode.cancel_generate(self.state)
        else:
            code_mode.stop_run(self.state)

    def _revert(self) -> None:
        code_mode.revert(self.state)

    def shutdown(self) -> None:
        code_mode.shutdown(self.state)

    # --- コード欄と行番号 ---

    def _scroll_code(self, *args: str) -> None:
        self.code.yview(*args)
        self.gutter.yview(*args)

    def _on_code_scroll(self, scroll: ttk.Scrollbar, first: str, last: str) -> None:
        scroll.set(first, last)
        self.gutter.yview_moveto(first)

    def _on_code_modified(self, _event: tk.Event) -> None:
        if self.code.edit_modified():
            self.code.edit_modified(False)
            self._update_gutter()
            self._update_buttons()

    def _update_gutter(self) -> None:
        lines = int(self.code.index("end-1c").split(".")[0])
        text = "\n".join(str(n) for n in range(1, lines + 1))

        self.gutter.configure(state="normal")
        self.gutter.delete("1.0", "end")
        self.gutter.insert("1.0", text, "number")
        self.gutter.configure(state="disabled")
        self.gutter.yview_moveto(self.code.yview()[0])

    # --- 描画 ---

    def _poll(self) -> None:
        if self._dirty.is_set():
            self._dirty.clear()
            self._refresh()

        self.after(POLL_INTERVAL_MS, self._poll)

    def _refresh(self) -> None:
        state = self.state

        with state.lock:
            code = state.code
            version = state.code_version
            explanation = state.explanation
            output = state.output
            run_status = state.run_status
            error = state.error
            generating = state.generating
            explaining = state.explaining

        # LLM の生成や「元に戻す」で変わったときだけ、コード欄を書き換える
        # (ユーザーが編集中の内容は上書きしない)
        if version != self._shown_version:
            self._shown_version = version
            self.code.delete("1.0", "end")
            self.code.insert("1.0", code)
            self.code.edit_reset()

            # 生成中は最終行を追いかける (横にはスクロールしない)
            self.code.xview_moveto(0)

            if generating:
                self.code.yview_moveto(1.0)

            self._update_gutter()

        if explanation != self._shown_explanation:
            self._set_text(self.explanation, explanation)
            self._shown_explanation = explanation

        if output != self._shown_output:
            self._set_text(self.output, output, follow=True)
            self._shown_output = output

        colors = self._colors()

        if error:
            self.status.configure(text=error, foreground=colors["error"])
        else:
            if generating:
                text = "コードを作っています…"
            elif explaining:
                text = "解説を作っています…"
            else:
                text = run_status

            self.status.configure(text=text, foreground=colors["sub_text"])

        self._update_buttons()

    @staticmethod
    def _set_text(widget: tk.Text, text: str, follow: bool = False) -> None:
        widget.configure(state="normal")
        widget.delete("1.0", "end")
        widget.insert("1.0", text)

        if follow:
            widget.see("end")

        widget.configure(state="disabled")

    def _update_buttons(self) -> None:
        state = self.state

        with state.lock:
            busy = state.busy
            running = state.running
            can_revert = (state.code != state.original_code
                          or self.code.get("1.0", "end-1c") != state.original_code)
            has_original = bool(state.original_code)

        idle = not busy and not running
        self.generate_button.state(["!disabled"] if idle else ["disabled"])
        self.run_button.state(["!disabled"] if idle else ["disabled"])
        self.explain_button.state(["!disabled"] if idle else ["disabled"])
        self.stop_button.state(
            ["!disabled"] if busy or running else ["disabled"])
        self.revert_button.state(
            ["!disabled"] if idle and has_original and can_revert else ["disabled"])
