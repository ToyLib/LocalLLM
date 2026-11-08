"""設定画面 (Tk)

画面の設定 (config/ui.json)、system プロンプト (config/agents.json)、
サーバーの URL (config/server.json) を編集して保存する
"""

from __future__ import annotations

import os
import re
import tkinter as tk
import tkinter.font as tkfont
from collections.abc import Callable
from tkinter import colorchooser, messagebox
from tkinter import ttk
from typing import Any

import config
from agent import DEFAULT_AGENTS
from llm_client import CloudSettings

ERROR_COLOR = "#e05555"
NOTE_COLOR = "#888888"

# 文字の大きさを選べる範囲 (ポイント)
MIN_FONT_SIZE = 8
MAX_FONT_SIZE = 40

# config/ui.json の python_colors のキーと、画面に出す説明 (色は "#RRGGBB")
COLOR_ENTRIES = (
    ("background", "背景 (会話欄・入力欄)"),
    ("text", "文字"),
    ("sub_text", "補足の文字"),
    ("user", "送信者名 (You)"),
    ("ai", "回答者名"),
    ("error", "エラー"),
)

_COLOR_PATTERN = re.compile(r"#[0-9A-Fa-f]{6}")


def is_color(value: Any) -> bool:
    """"#RRGGBB" の形の文字列か"""

    return isinstance(value, str) and _COLOR_PATTERN.fullmatch(value) is not None


# 設定画面で編集するエージェントと、画面に出す説明
AGENT_ENTRIES = (
    ("router", "ルーター"),
    ("assistant", "資料ありの回答役"),
    ("general", "資料なしの回答役"),
    ("cloud", "ChatGPT"),
    ("rewriter", "口調の書き直し"),
    ("comparer", "違いのまとめ"),
)

# 回答役 (人格のテンプレートと、ChatGPT の回答の書き直しを選べる)
ANSWERERS = ("assistant", "general")


def mask_key(key: str) -> str:
    """API キーの末尾だけを見せる"""

    return "…" + key[-4:] if len(key) > 8 else "…"


def describe_api_key(name: str) -> str:
    """API キーの状態 (値は末尾4文字だけ)"""

    value, source = config.find_api_key(name)

    if source == config.API_KEY_ENVIRONMENT:
        return f"設定済み ({mask_key(value)})。環境変数 {name} から読んでいます"

    if source == config.API_KEY_SECRETS_FILE:
        return f"設定済み ({mask_key(value)})。秘密ファイルから読んでいます"

    return "未設定"


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
    colors: 適用中の色 (COLOR_ENTRIES のキー → "#RRGGBB")
    default_colors: 「色を既定に戻す」で使う色
    apply_colors: 色を画面に反映する (変えるたびに呼ぶ)
    on_reset: 設定を既定値に戻したあとに呼ぶ (画面の設定を読み直して、設定画面を開き直す)
    on_saved: 保存したあとに呼ぶ (API キーの有無を調べ直す)
    """

    def __init__(
        self,
        parent: tk.Tk,
        font: tkfont.Font,
        font_size: int,
        apply_font_size: Callable[[int], None],
        colors: dict[str, str],
        default_colors: dict[str, str],
        apply_colors: Callable[[dict[str, str]], None],
        on_reset: Callable[[], None],
        on_saved: Callable[[], None] | None = None,
    ) -> None:
        self.parent = parent
        self.font = font
        self.initial_font_size = font_size
        self.apply_font_size = apply_font_size
        self.initial_colors = dict(colors)
        self.default_colors = default_colors
        self.apply_colors = apply_colors
        self.on_reset = on_reset
        self.on_saved = on_saved

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

        # テンプレートは読めなくても設定は保存できるように、別に扱う
        self.templates: list[tuple[str, Any]] = []
        self.template_error = ""

        try:
            templates = config.load_for_edit("personas.json").get("templates")
        except config.ConfigError as e:
            self.template_error = str(e)
        else:
            if isinstance(templates, list):
                self.templates = [
                    (str(t.get("name", "(名前なし)")), t["system_prompt"])
                    for t in templates
                    if isinstance(t, dict) and "system_prompt" in t
                ]
            else:
                self.template_error = "personas.json に templates (配列) がありません"

        if self.load_error:
            self.load_error += "\n(ファイルを直すまで保存できません)"

    # ---- 画面の部品 ----

    def _build(self) -> None:
        frame = ttk.Frame(self.window, padding=8)
        frame.pack(fill="both", expand=True)

        ttk.Label(
            frame, foreground=NOTE_COLOR,
            text=f"保存先: {config.config_directory()}",
        ).pack(side="top", fill="x", pady=(0, 6))

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

        # LOCALLLM_CONFIG_DIR を指定しているときは、そのフォルダが既定値なので戻せない
        # (読めないファイルがあるときも、既定値に戻せば直せる)
        if not os.environ.get("LOCALLLM_CONFIG_DIR"):
            ttk.Button(
                footer, text="すべて既定に戻す", command=self._reset,
            ).pack(side="left", padx=(0, 8))

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
            text="Python 版の python_font_size と python_colors です (C++ 版の画面の設定は C++ 版の設定画面で変えます)",
        ).grid(row=1, column=0, columnspan=3, sticky="w", pady=(8, 0))

        ttk.Separator(tab).grid(
            row=2, column=0, columnspan=3, sticky="ew", pady=10)

        # 色: "#RRGGBB" を直接書くか、「選ぶ」で色の選択画面から選ぶ
        self.colors: dict[str, tk.StringVar] = {}
        self.swatches: dict[str, tk.Label] = {}

        for row, (key, label) in enumerate(COLOR_ENTRIES, start=3):
            ttk.Label(tab, text=label).grid(row=row, column=0, sticky="w")

            variable = tk.StringVar(value=self.initial_colors[key])
            ttk.Entry(tab, textvariable=variable, width=9).grid(
                row=row, column=1, sticky="w", padx=(8, 0), pady=2)

            swatch = tk.Label(
                tab, width=3, relief="solid", borderwidth=1,
                background=self.initial_colors[key], cursor="hand2")
            swatch.grid(row=row, column=2, sticky="w", padx=(6, 0))
            swatch.bind("<Button-1>", lambda _e, k=key: self._choose_color(k))

            ttk.Button(
                tab, text="選ぶ", command=lambda k=key: self._choose_color(k),
            ).grid(row=row, column=3, sticky="w", padx=(6, 0))

            variable.trace_add("write", lambda *_: self._on_color())
            self.colors[key] = variable
            self.swatches[key] = swatch

        ttk.Button(
            tab, text="色を既定に戻す", command=self._reset_colors,
        ).grid(row=len(COLOR_ENTRIES) + 3, column=0, sticky="w", pady=(8, 0))

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

        # key → (名前, プロンプトの入力欄)
        self.agent_widgets: dict[str, tuple[tk.StringVar, tk.Text]] = {}
        # key → system_prompt が文字列か (テンプレートを読み込むと変わる)
        self.prompt_is_string: dict[str, bool] = {}
        # 回答役の key → ChatGPT の回答をこの口調で書き直すか
        self.rewrite: dict[str, tk.BooleanVar] = {}

        for key, label in AGENT_ENTRIES:
            entry = self.agents_json.get(key)

            if not isinstance(entry, dict):
                # 前の版の設定ファイルに無いもの (cloud / rewriter / comparer) は、既定のプロンプトから始める
                default = DEFAULT_AGENTS.get(key)
                entry = ({"name": default[0], "system_prompt": [default[1]]}
                         if default else {})

            prompt = entry.get("system_prompt")
            is_string = isinstance(prompt, str)

            page = ttk.Frame(agents, padding=8)
            agents.add(page, text=label)

            name = tk.StringVar(value=str(entry.get("name", key)))

            # 回答役は、人格のテンプレートから選んで直せる
            if key in ANSWERERS:
                self._build_template_picker(page, key, entry.get("name"))

            if key == "cloud":
                ttk.Label(
                    page, foreground=NOTE_COLOR,
                    text="「ChatGPT に聞き直す」で送る system プロンプトです (名前はサーバーのタブで決めます)。",
                ).pack(side="top", fill="x", pady=(0, 6))
            elif key == "rewriter":
                ttk.Label(
                    page, foreground=NOTE_COLOR,
                    text="ChatGPT の回答を書き直すとき、回答役の system プロンプトの後ろに付ける指示です。",
                ).pack(side="top", fill="x", pady=(0, 6))
            elif key == "comparer":
                ttk.Label(
                    page, foreground=NOTE_COLOR,
                    text="ChatGPT に聞き直した後、聞き直す前の回答との違いをローカルの LLM でまとめるときの指示です。",
                ).pack(side="top", fill="x", pady=(0, 6))
            else:
                row = ttk.Frame(page)
                row.pack(side="top", fill="x", pady=(0, 6))
                ttk.Label(row, text="名前").pack(side="left")
                ttk.Entry(row, textvariable=name).pack(
                    side="left", fill="x", expand=True, padx=(8, 0))

            if key in ANSWERERS:
                rewrite = entry.get("rewrite", key != "assistant")
                variable = tk.BooleanVar(
                    value=rewrite if isinstance(rewrite, bool) else key != "assistant")
                ttk.Checkbutton(
                    page, variable=variable,
                    text="ChatGPT に聞き直した回答を、この口調で書き直す",
                ).pack(side="top", anchor="w", pady=(0, 6))
                self.rewrite[key] = variable

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

            self.agent_widgets[key] = (name, text)
            self.prompt_is_string[key] = is_string

        return tab

    def _build_template_picker(
            self, page: ttk.Frame, key: str, current_name: Any) -> None:
        """テンプレートの選択欄と、選んだものを読み込むボタン"""

        if self.template_error:
            ttk.Label(
                page, foreground=NOTE_COLOR,
                text=f"テンプレートを読めません: {self.template_error}",
            ).pack(side="top", fill="x", pady=(0, 6))
            return

        if not self.templates:
            return

        names = [name for name, _prompt in self.templates]

        row = ttk.Frame(page)
        row.pack(side="top", fill="x", pady=(0, 6))
        ttk.Label(row, text="テンプレート").pack(side="left")

        # 今の名前と同じテンプレートを選んだ状態で開く
        choice = ttk.Combobox(row, values=names, state="readonly", width=20)
        choice.current(names.index(current_name) if current_name in names else 0)
        choice.pack(side="left", padx=(8, 0))

        ttk.Button(
            row, text="このテンプレートにする",
            command=lambda: self._apply_template(key, choice.current()),
        ).pack(side="left", padx=(6, 0))

    def _apply_template(self, key: str, index: int) -> None:
        """名前とプロンプトを置き換える (保存するまではファイルは変わらない)"""

        if not 0 <= index < len(self.templates):
            return

        template_name, prompt = self.templates[index]
        name, text = self.agent_widgets[key]

        name.set(template_name)
        text.delete("1.0", "end")
        text.insert("1.0", prompt_to_text(prompt))
        self.prompt_is_string[key] = isinstance(prompt, str)

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

        # ChatGPT (server.json の cloud。無ければ既定値)
        ttk.Separator(tab).grid(
            row=3, column=0, columnspan=2, sticky="ew", pady=10)
        ttk.Label(tab, text="ChatGPT (「聞き直す」ボタンで使う)").grid(
            row=4, column=0, columnspan=2, sticky="w", pady=(0, 4))

        defaults = CloudSettings()
        cloud = self.server_json.get("cloud")
        cloud = cloud if isinstance(cloud, dict) else {}
        self.cloud: dict[str, tk.StringVar] = {}

        for row, (key, label) in enumerate(
                (("name", "表示名"), ("chat_url", "URL"), ("model", "モデル"),
                 ("api_key_name", "API キーの名前")), start=5):
            value = cloud.get(key)
            variable = tk.StringVar(
                value=value if isinstance(value, str) else getattr(defaults, key))
            ttk.Label(tab, text=label).grid(row=row, column=0, sticky="w")
            ttk.Entry(tab, textvariable=variable).grid(
                row=row, column=1, sticky="ew", padx=(8, 0), pady=2)
            self.cloud[key] = variable

        ttk.Label(
            tab, foreground=NOTE_COLOR, wraplength=560, justify="left",
            text="API キーは、この名前の環境変数か、下の秘密ファイルから読みます (設定ファイルには書きません)。",
        ).grid(row=9, column=0, columnspan=2, sticky="w", pady=(4, 4))

        self.key_status = ttk.Label(tab)
        self.key_status.grid(row=10, column=0, columnspan=2, sticky="w")
        self.cloud["api_key_name"].trace_add(
            "write", lambda *_: self._update_key_status())

        ttk.Label(tab, text="新しい API キー").grid(row=11, column=0, sticky="w")
        key_row = ttk.Frame(tab)
        key_row.grid(row=11, column=1, sticky="ew", padx=(8, 0), pady=2)

        self.api_key = tk.StringVar()
        self.api_key_entry = ttk.Entry(key_row, textvariable=self.api_key, show="•")
        self.api_key_entry.pack(side="left", fill="x", expand=True)

        self.delete_api_key = False
        self.delete_button = ttk.Button(
            key_row, text="秘密ファイルから削除", command=self._toggle_delete_key)
        self.delete_button.pack(side="left", padx=(6, 0))

        self.key_hint = ttk.Label(tab, foreground=NOTE_COLOR)
        self.key_hint.grid(row=12, column=0, columnspan=2, sticky="w")

        try:
            secrets = str(config.secrets_file_path())
        except config.ConfigError as e:
            secrets = str(e)

        ttk.Label(
            tab, foreground=NOTE_COLOR, wraplength=560, justify="left",
            text=f"秘密ファイル: {secrets}\nAPI キーは https の URL にだけ送ります。",
        ).grid(row=13, column=0, columnspan=2, sticky="w", pady=(4, 0))

        self._update_key_status()
        self._update_key_hint()

        return tab

    def _update_key_status(self) -> None:
        self.key_status.configure(
            text="API キー: " + describe_api_key(self.cloud["api_key_name"].get()))

    def _update_key_hint(self) -> None:
        if self.delete_api_key:
            self.key_hint.configure(
                text="保存すると、秘密ファイルから API キーを削除します。")
        else:
            self.key_hint.configure(
                text="入力して保存すると、秘密ファイルに登録します (空なら今のまま)。")

    def _toggle_delete_key(self) -> None:
        self.delete_api_key = not self.delete_api_key
        self.api_key.set("")
        self.api_key_entry.state(["disabled" if self.delete_api_key else "!disabled"])
        self.delete_button.configure(
            text="削除をやめる" if self.delete_api_key else "秘密ファイルから削除")
        self._update_key_hint()

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

    def _current_colors(self) -> dict[str, str]:
        """入力中の色。"#RRGGBB" になっていない色は適用中の色のまま"""

        colors = dict(self.initial_colors)

        for key, variable in self.colors.items():
            if is_color(value := variable.get().strip()):
                colors[key] = value

        return colors

    def _on_color(self) -> None:
        # 見ながら調整できるように、すぐ反映する
        colors = self._current_colors()

        for key, swatch in self.swatches.items():
            swatch.configure(background=colors[key])

        self.apply_colors(colors)

    def _choose_color(self, key: str) -> None:
        _rgb, chosen = colorchooser.askcolor(
            self._current_colors()[key], parent=self.window,
            title=dict(COLOR_ENTRIES)[key])

        if chosen:
            self.colors[key].set(chosen.upper())

    def _reset_colors(self) -> None:
        for key, variable in self.colors.items():
            variable.set(self.default_colors[key])

    def _save(self) -> None:
        size = self._current_font_size()

        if size is None:
            self.save_error.configure(
                text=f"文字の大きさは {MIN_FONT_SIZE}〜{MAX_FONT_SIZE} にしてください")
            return

        invalid = [label for key, label in COLOR_ENTRIES
                   if not is_color(self.colors[key].get().strip())]

        if invalid:
            self.save_error.configure(
                text=f"色は #RRGGBB の形で書いてください: {'、'.join(invalid)}")
            return

        colors = self._current_colors()

        self.ui_json["python_font_size"] = size

        saved_colors = self.ui_json.get("python_colors")

        if not isinstance(saved_colors, dict):
            saved_colors = self.ui_json["python_colors"] = {}

        saved_colors.update(colors)

        for key, variable in self.urls.items():
            self.server_json[key] = variable.get()

        # auth など、画面で扱わない項目はそのまま残す
        cloud = self.server_json.get("cloud")

        if not isinstance(cloud, dict):
            cloud = self.server_json["cloud"] = {}

        for key, variable in self.cloud.items():
            cloud[key] = variable.get()

        cloud.setdefault("auth", "bearer")

        for key, (name, text) in self.agent_widgets.items():
            is_string = self.prompt_is_string[key]
            entry = self.agents_json.get(key)

            if not isinstance(entry, dict):
                entry = self.agents_json[key] = {}

            prompt = text.get("1.0", "end-1c")
            entry["name"] = name.get()
            entry["system_prompt"] = (
                prompt if is_string else text_to_prompt_lines(prompt))

            if key in self.rewrite:
                entry["rewrite"] = self.rewrite[key].get()

        try:
            # API キーは設定ファイルではなく秘密ファイルに書く
            key_name = self.cloud["api_key_name"].get()

            if self.delete_api_key:
                config.save_api_key(key_name, "")
            elif key := self.api_key.get().strip():
                config.save_api_key(key_name, key)

            self.api_key.set("")

            config.save("ui.json", self.ui_json)
            config.save("server.json", self.server_json)
            config.save("agents.json", self.agents_json)
        except config.ConfigError as e:
            self.save_error.configure(text=str(e))
            return

        self.apply_font_size(size)
        self.apply_colors(colors)
        self._close()

        if self.on_saved:
            self.on_saved()

    def _reset(self) -> None:
        """ユーザーごとの設定を config/ の内容で上書きする"""

        if not messagebox.askyesno(
                "既定に戻す",
                "画面の設定・システムプロンプト・サーバーの URL・人格のテンプレートを、"
                "すべて既定値 (config/ の内容) に戻します。\n"
                "保存した設定は消えて、元に戻せません。よろしいですか？",
                icon="warning", default="no", parent=self.window):
            return

        try:
            config.reset_to_defaults()
        except config.ConfigError as e:
            self.save_error.configure(text=str(e))
            return

        self._close()
        self.on_reset()

    def _cancel(self) -> None:
        # 見ながら変えた文字の大きさと色を元に戻す
        self.apply_font_size(self.initial_font_size)
        self.apply_colors(self.initial_colors)
        self._close()

    def _close(self) -> None:
        self.window.grab_release()
        self.window.destroy()
        self.parent.focus_set()
