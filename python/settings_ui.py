"""設定画面 (Tk)

画面の設定 (config/ui.json)、system プロンプト (config/agents.json)、
サーバーの URL (config/server.json) を編集して保存する
"""

from __future__ import annotations

import tkinter as tk
import tkinter.font as tkfont
from collections.abc import Callable
from tkinter import ttk
from typing import Any

import config

ERROR_COLOR = "#e05555"
NOTE_COLOR = "#888888"

# 文字の大きさを選べる範囲 (ポイント)
MIN_FONT_SIZE = 8
MAX_FONT_SIZE = 40

# 設定画面で編集するエージェントと、画面に出す説明
AGENT_ENTRIES = (
    ("router", "ルーター"),
    ("assistant", "資料ありの回答役"),
    ("general", "資料なしの回答役"),
)


def prompt_to_text(prompt: Any) -> str:
    """system_prompt (文字列か文字列の配列) を、編集用に1要素1行の文字列にする"""

    if isinstance(prompt, str):
        return prompt

    if isinstance(prompt, list):
        return "\n".join(line for line in prompt if isinstance(line, str))

    return ""


def text_to_prompt_lines(text: str) -> list[str]:
    """編集した文字列を、1行1要素の配列にする (末尾の空行は捨てる)"""

    lines = [line.removesuffix("\r") for line in text.split("\n")]

    while lines and not lines[-1]:
        lines.pop()

    return lines


class SettingsDialog:
    """設定画面 (モーダル)

    font_size: 適用中の文字の大きさ
    apply_font_size: 文字の大きさを画面に反映する (見ながら調整できるように、変えるたびに呼ぶ)
    """

    def __init__(
        self,
        parent: tk.Tk,
        font: tkfont.Font,
        font_size: int,
        apply_font_size: Callable[[int], None],
    ) -> None:
        self.parent = parent
        self.font = font
        self.initial_font_size = font_size
        self.apply_font_size = apply_font_size

        self.window = tk.Toplevel(parent)
        self.window.title("設定")
        self.window.transient(parent)
        self.window.geometry(
            f"{max(parent.winfo_width() - 80, 520)}x"
            f"{max(parent.winfo_height() - 80, 420)}")
        self.window.protocol("WM_DELETE_WINDOW", self._cancel)

        self._load()
        self._build()

        # 閉じるまで後ろのチャット画面は操作させない
        self.window.wait_visibility()
        self.window.grab_set()

    # ---- 読み込み ----

    def _load(self) -> None:
        errors: list[str] = []

        def load(file_name: str) -> dict[str, Any]:
            try:
                return config.load_for_edit(file_name)
            except config.ConfigError as e:
                errors.append(str(e))
                return {}

        # 設定画面で扱わない項目もそのまま残して保存する
        self.ui_json = load("ui.json")
        self.server_json = load("server.json")
        self.agents_json = load("agents.json")

        self.load_error = "\n".join(errors)

        if self.load_error:
            self.load_error += "\n(ファイルを直すまで保存できません)"

    # ---- 画面の部品 ----

    def _build(self) -> None:
        frame = ttk.Frame(self.window, padding=8)
        frame.pack(fill="both", expand=True)

        if self.load_error:
            ttk.Label(
                frame, text=self.load_error, foreground=ERROR_COLOR,
                wraplength=600).pack(side="top", fill="x", pady=(0, 6))

        # ボタンは下端 (ウィンドウが狭いときもボタンが隠れないように先に詰める)
        footer = ttk.Frame(frame)
        footer.pack(side="bottom", fill="x", pady=(8, 0))

        ttk.Button(footer, text="キャンセル", command=self._cancel).pack(
            side="right")
        save_button = ttk.Button(footer, text="保存", command=self._save)
        save_button.pack(side="right", padx=(0, 6))

        if self.load_error:
            save_button.state(["disabled"])

        self.save_error = ttk.Label(footer, foreground=ERROR_COLOR)
        self.save_error.pack(side="left", fill="x", expand=True)

        notebook = ttk.Notebook(frame)
        notebook.pack(side="top", fill="both", expand=True)

        notebook.add(self._build_ui_tab(notebook), text="画面")
        notebook.add(self._build_prompt_tab(notebook), text="システムプロンプト")
        notebook.add(self._build_server_tab(notebook), text="サーバー")

    def _build_ui_tab(self, parent: ttk.Notebook) -> ttk.Frame:
        tab = ttk.Frame(parent, padding=12)

        ttk.Label(tab, text="文字の大きさ (ポイント)").grid(
            row=0, column=0, sticky="w")

        self.font_size = tk.IntVar(value=self.initial_font_size)
        ttk.Spinbox(
            tab, from_=MIN_FONT_SIZE, to=MAX_FONT_SIZE, width=6,
            textvariable=self.font_size, command=self._on_font_size).grid(
            row=0, column=1, sticky="w", padx=(8, 0))
        self.font_size.trace_add("write", lambda *_: self._on_font_size())

        ttk.Label(
            tab, foreground=NOTE_COLOR,
            text="Python 版の python_font_size です (C++ 版の画面の設定は C++ 版の設定画面で変えます)",
        ).grid(row=1, column=0, columnspan=2, sticky="w", pady=(8, 0))

        return tab

    def _build_prompt_tab(self, parent: ttk.Notebook) -> ttk.Frame:
        tab = ttk.Frame(parent, padding=8)

        ttk.Label(
            tab, foreground=NOTE_COLOR,
            text="1行が system_prompt の配列の1要素です。送るときは行をそのままつなげます (改行は入りません)。\n"
                 "保存すると次の質問から反映されます。",
        ).pack(side="top", fill="x", pady=(0, 6))

        agents = ttk.Notebook(tab)
        agents.pack(side="top", fill="both", expand=True)

        # key → (名前, プロンプトの入力欄, 元が文字列だったか)
        self.agent_widgets: dict[str, tuple[tk.StringVar, tk.Text, bool]] = {}

        for key, label in AGENT_ENTRIES:
            entry = self.agents_json.get(key)
            entry = entry if isinstance(entry, dict) else {}
            prompt = entry.get("system_prompt")
            is_string = isinstance(prompt, str)

            page = ttk.Frame(agents, padding=8)
            agents.add(page, text=label)

            row = ttk.Frame(page)
            row.pack(side="top", fill="x", pady=(0, 6))
            ttk.Label(row, text="名前").pack(side="left")
            name = tk.StringVar(value=str(entry.get("name", key)))
            ttk.Entry(row, textvariable=name).pack(
                side="left", fill="x", expand=True, padx=(8, 0))

            if is_string:
                ttk.Label(
                    page, foreground=NOTE_COLOR,
                    text="(この system_prompt は1つの文字列で書かれています。改行もそのまま送ります)",
                ).pack(side="top", fill="x", pady=(0, 6))

            box = ttk.Frame(page)
            box.pack(side="top", fill="both", expand=True)

            text = tk.Text(
                box, wrap="char", font=self.font, padx=6, pady=6,
                borderwidth=1, relief="solid", undo=True, height=8)
            scrollbar = ttk.Scrollbar(
                box, orient="vertical", command=text.yview)
            text.configure(yscrollcommand=scrollbar.set)
            scrollbar.pack(side="right", fill="y")
            text.pack(side="left", fill="both", expand=True)
            text.insert("1.0", prompt_to_text(prompt))
            text.edit_reset()

            self.agent_widgets[key] = (name, text, is_string)

        return tab

    def _build_server_tab(self, parent: ttk.Notebook) -> ttk.Frame:
        tab = ttk.Frame(parent, padding=12)
        tab.columnconfigure(1, weight=1)

        self.urls: dict[str, tk.StringVar] = {}

        for row, (key, label) in enumerate((("chat_url", "チャット"),
                                            ("embed_url", "埋め込み (RAG)"))):
            value = self.server_json.get(key)
            variable = tk.StringVar(value=value if isinstance(value, str) else "")
            ttk.Label(tab, text=label).grid(row=row, column=0, sticky="w")
            ttk.Entry(tab, textvariable=variable).grid(
                row=row, column=1, sticky="ew", padx=(8, 0), pady=2)
            self.urls[key] = variable

        ttk.Label(
            tab, foreground=NOTE_COLOR,
            text="保存すると次の質問から反映されます。",
        ).grid(row=2, column=0, columnspan=2, sticky="w", pady=(8, 0))

        return tab

    # ---- 操作 ----

    def _current_font_size(self) -> int | None:
        try:
            size = self.font_size.get()
        except tk.TclError:
            # 入力途中で数字になっていない
            return None

        return size if MIN_FONT_SIZE <= size <= MAX_FONT_SIZE else None

    def _on_font_size(self) -> None:
        # 見ながら調整できるように、すぐ反映する
        if (size := self._current_font_size()) is not None:
            self.apply_font_size(size)

    def _save(self) -> None:
        size = self._current_font_size()

        if size is None:
            self.save_error.configure(
                text=f"文字の大きさは {MIN_FONT_SIZE}〜{MAX_FONT_SIZE} にしてください")
            return

        self.ui_json["python_font_size"] = size

        for key, variable in self.urls.items():
            self.server_json[key] = variable.get()

        for key, (name, text, is_string) in self.agent_widgets.items():
            entry = self.agents_json.get(key)

            if not isinstance(entry, dict):
                entry = self.agents_json[key] = {}

            prompt = text.get("1.0", "end-1c")
            entry["name"] = name.get()
            entry["system_prompt"] = (
                prompt if is_string else text_to_prompt_lines(prompt))

        try:
            config.save("ui.json", self.ui_json)
            config.save("server.json", self.server_json)
            config.save("agents.json", self.agents_json)
        except config.ConfigError as e:
            self.save_error.configure(text=str(e))
            return

        self.apply_font_size(size)
        self._close()

    def _cancel(self) -> None:
        # 見ながら変えた文字の大きさを元に戻す
        self.apply_font_size(self.initial_font_size)
        self._close()

    def _close(self) -> None:
        self.window.grab_release()
        self.window.destroy()
        self.parent.focus_set()
