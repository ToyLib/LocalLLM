"""クイズタブの画面 (Tk)"""

from __future__ import annotations

import threading
import tkinter as tk
import tkinter.font as tkfont
from tkinter import ttk
from typing import Callable

import config
import llm_client
import quiz_mode
import rag
import server_manager
from llm_client import CancelToken, Cancelled, LLMError
from quiz_mode import CHOICE_COUNT, Category, QuizState

# ワーカースレッドからの更新を見に行く間隔 (ミリ秒)
POLL_INTERVAL_MS = 50

NOTICE = ("AI が作った問題です。特に「資料から」以外の分野は、正解や解説が誤っていることがあります。"
          "気になったら調べ直してください。")

RAG_DESCRIPTION = "rag/ フォルダーの資料の内容から出題します (資料の読み込みが終わってから)。"
CLOUD_RAG_WARNING = "資料 (rag/) の内容の一部が外部のサービスに送られます。個人的な内容が含まれていないか確認してください。"

RAG_CUSTOM_DESCRIPTION = "rag/ フォルダーの資料のうち、お題に近い内容から出題します (資料の読み込みが終わってから)。"

# 新しい分野の仮の名前 (変えないと保存できない)
NEW_NAME = "新しい分野"
PROMPT_HELP = ("お題や出題の方針を自由に書きます (例: 「Git のブランチとマージ。初心者向けに、コマンドの意味を中心に」)。"
               "問題の形式 (4 択・正解・解説) はアプリが決めます。"
               "「資料を使う」にしたとき、お題が空なら資料からランダムに、書いてあればお題に近い資料から出題します。")


class QuizTab(ttk.Frame):
    """分野を選んで、4 択を 10 問解いて、点数を見る画面"""

    def __init__(
        self,
        parent: tk.Misc,
        font: tkfont.Font,
        bold: tkfont.Font,
        colors: Callable[[], dict[str, str]],
        rag_state: rag.RagState,
    ) -> None:
        super().__init__(parent)

        self._font = font
        self._bold = bold
        self._colors = colors
        self._rag = rag_state
        self._dirty = threading.Event()
        self.state = QuizState(self._dirty.set)

        self._categories: list[Category] = []
        self._custom_error = ""
        self._shown_phase = ""

        # 分野の編集画面 (選択の画面の代わりに出す)
        self._editing = False
        self._edit_items: list[Category] = []
        self._edit_index = -1
        self._test_session = 0
        self._test_cancel = CancelToken()
        self._test_result = ""
        self._test_running = False

        # 出題中: 画面に出している問題 (変わったら、選択を外す)
        self._shown_question: object = None
        self._choice = tk.IntVar(value=-1)

        # 結果: 振り返りを描いた内容 (変わったときだけ描き直す)
        self._shown_review: tuple | None = None

        self._wrapped: list[ttk.Label | tk.Radiobutton] = []

        self._build_select()
        self._build_edit()
        self._build_play()
        self._build_result()
        self.bind("<Configure>", self._on_resize)

        self._show_phase(quiz_mode.PHASE_SELECT)
        # 色は ChatApp が apply_colors で付けたあとに、最初の描画をする
        self._dirty.set()
        self.after(POLL_INTERVAL_MS, self._poll)

    # --- 画面の部品 ---

    def _label(self, parent: tk.Misc, **options: object) -> ttk.Label:
        label = ttk.Label(parent, font=self._font, **options)
        self._wrapped.append(label)
        return label

    def _build_select(self) -> None:
        self.select_frame = ttk.Frame(self, padding=16)

        self._label(self.select_frame, text=NOTICE).pack(side="top", anchor="w")

        row = ttk.Frame(self.select_frame)
        row.pack(side="top", anchor="w", pady=(16, 4))
        ttk.Label(row, text="分野:", font=self._font).pack(side="left")

        self.category_box = ttk.Combobox(row, state="readonly", width=28, font=self._font)
        self.category_box.pack(side="left", padx=(8, 12))
        self.category_box.bind("<<ComboboxSelected>>", lambda _e: self._dirty.set())

        self.start_button = ttk.Button(row, text="開始", command=self._start)
        self.start_button.pack(side="left")
        ttk.Button(row, text="分野を編集…", command=self._open_editor).pack(
            side="left", padx=(24, 0))

        self.category_note = self._label(self.select_frame)
        self.category_note.pack(side="top", anchor="w", pady=(4, 0))

        self.select_error = self._label(self.select_frame)
        self.select_error.pack(side="top", anchor="w", pady=(12, 0))

    def _build_edit(self) -> None:
        self.edit_frame = ttk.Frame(self, padding=16)

        ttk.Label(self.edit_frame, text="自作の分野", font=self._bold).pack(
            side="top", anchor="w")
        self._label(self.edit_frame, text=PROMPT_HELP).pack(
            side="top", anchor="w", pady=(4, 8))

        row = ttk.Frame(self.edit_frame)
        row.pack(side="bottom", anchor="w", pady=(12, 0))
        ttk.Button(row, text="保存して戻る", command=self._edit_save).pack(side="left")
        ttk.Button(row, text="破棄して戻る", command=self._close_editor).pack(
            side="left", padx=(8, 0))

        body = ttk.Frame(self.edit_frame)
        body.pack(side="top", fill="both", expand=True)

        left = ttk.Frame(body)
        left.pack(side="left", fill="y")
        self.edit_list = tk.Listbox(
            left, font=self._font, width=20, height=8, exportselection=False,
            activestyle="none")
        self.edit_list.pack(side="top", fill="y", expand=True)
        self.edit_list.bind("<<ListboxSelect>>", self._on_edit_select)
        buttons = ttk.Frame(left)
        buttons.pack(side="top", pady=(6, 0))
        ttk.Button(buttons, text="追加", command=self._edit_add).pack(side="left")
        ttk.Button(buttons, text="削除", command=self._edit_delete).pack(
            side="left", padx=(6, 0))

        right = ttk.Frame(body)
        right.pack(side="left", fill="both", expand=True, padx=(16, 0))

        row = ttk.Frame(right)
        row.pack(side="top", fill="x")
        ttk.Label(row, text="分野名:", font=self._font).pack(side="left")
        self.edit_name = ttk.Entry(row, font=self._font)
        self.edit_name.pack(side="left", fill="x", expand=True, padx=(8, 0))

        # 名前を打つのに合わせて、左の一覧の表示も変える
        self.edit_name.bind("<KeyRelease>", lambda _e: self._store_edit_fields())

        ttk.Label(right, text="お題・出題の方針:", font=self._font).pack(
            side="top", anchor="w", pady=(8, 2))
        self.edit_prompt = tk.Text(
            right, wrap="word", font=self._font, height=7, padx=6, pady=6,
            borderwidth=1, relief="solid", undo=True)
        self.edit_prompt.pack(side="top", fill="both", expand=True)

        self.edit_rag = tk.BooleanVar(value=False)
        self.edit_cloud = tk.BooleanVar(value=False)
        self.rag_check = ttk.Checkbutton(
            right, text="資料 (rag/) を使う", variable=self.edit_rag,
            command=self._on_exclusive)
        self.rag_check.pack(side="top", anchor="w", pady=(6, 0))
        self.cloud_check = ttk.Checkbutton(
            right, variable=self.edit_cloud, command=self._on_exclusive)
        self.cloud_check.pack(side="top", anchor="w")
        self.cloud_warning = self._label(right, text=CLOUD_RAG_WARNING)

        row = ttk.Frame(right)
        row.pack(side="top", anchor="w", pady=(6, 0))
        self.test_button = ttk.Button(row, text="テスト出題", command=self._edit_test)
        self.test_button.pack(side="left")
        self.edit_message = self._label(row)
        self.edit_message.pack(side="left", padx=(12, 0))

        self.test_output = tk.Text(
            right, wrap="word", font=self._font, height=6, padx=6, pady=6,
            borderwidth=1, relief="solid", state="disabled", cursor="arrow")
        self.test_output.pack(side="top", fill="x", pady=(6, 0))

    def _build_play(self) -> None:
        self.play_frame = ttk.Frame(self, padding=16)

        self.header = ttk.Label(self.play_frame, font=self._bold)
        self.header.pack(side="top", anchor="w")

        self.question_label = self._label(self.play_frame)
        self.question_label.configure(font=self._bold)
        self.question_label.pack(side="top", anchor="w", pady=(12, 8))

        self.choice_buttons: list[tk.Radiobutton] = []

        for i in range(CHOICE_COUNT):
            button = tk.Radiobutton(
                self.play_frame, variable=self._choice, value=i, font=self._font,
                anchor="w", justify="left", command=self._dirty.set)
            button.pack(side="top", anchor="w", fill="x", pady=2)
            self.choice_buttons.append(button)
            self._wrapped.append(button)

        row = ttk.Frame(self.play_frame)
        row.pack(side="top", anchor="w", pady=(12, 8))
        self.answer_button = ttk.Button(row, text="回答する", command=self._answer)
        self.answer_button.pack(side="left")
        self.next_button = ttk.Button(row, text="次の問題へ", command=self._next)
        self.next_button.pack(side="left", padx=(8, 0))
        self.quit_button = ttk.Button(row, text="やめる", command=self._quit)
        self.quit_button.pack(side="left", padx=(24, 0))

        self.verdict = ttk.Label(self.play_frame, font=self._bold)
        self.verdict.pack(side="top", anchor="w")
        self.explanation = self._label(self.play_frame)
        self.explanation.pack(side="top", anchor="w", pady=(4, 0))
        self.play_status = self._label(self.play_frame)
        self.play_status.pack(side="top", anchor="w", pady=(8, 0))

    def _build_result(self) -> None:
        self.result_frame = ttk.Frame(self, padding=16)

        self.score_label = ttk.Label(self.result_frame, font=self._bold)
        self.score_label.pack(side="top", anchor="w")
        self.comment_label = self._label(self.result_frame)
        self.comment_label.pack(side="top", anchor="w", pady=(8, 12))

        row = ttk.Frame(self.result_frame)
        row.pack(side="bottom", anchor="w", pady=(12, 0))
        ttk.Button(row, text="もう一度", command=self._again).pack(side="left")
        ttk.Button(row, text="分野を変える", command=self._quit).pack(
            side="left", padx=(8, 0))

        ttk.Label(self.result_frame, text="間違えた問題", font=self._bold).pack(
            side="top", anchor="w")

        frame = ttk.Frame(self.result_frame)
        frame.pack(side="top", fill="both", expand=True, pady=(4, 0))
        scroll = ttk.Scrollbar(frame, orient="vertical")
        scroll.pack(side="right", fill="y")
        self.review = tk.Text(
            frame, wrap="word", font=self._font, padx=8, pady=8, borderwidth=1,
            relief="solid", state="disabled", cursor="arrow", height=8,
            yscrollcommand=scroll.set)
        scroll.configure(command=self.review.yview)
        self.review.pack(side="left", fill="both", expand=True)
        self.review.tag_configure("title", font=self._bold)

    def apply_colors(self) -> None:
        """色を会話画面に合わせる"""

        colors = self._colors()
        self.review.configure(
            background=colors["background"], foreground=colors["text"])
        self.review.tag_configure("wrong", foreground=colors["error"])
        self.review.tag_configure("right", foreground=colors["ai"])
        self.review.tag_configure("sub", foreground=colors["sub_text"])

        for widget in (self.edit_prompt, self.test_output):
            widget.configure(background=colors["background"], foreground=colors["text"],
                             insertbackground=colors["text"])

        self.edit_list.configure(
            background=colors["background"], foreground=colors["text"])
        self._dirty.set()

    def _on_resize(self, event: tk.Event) -> None:
        """文章の折り返す幅を、ウィンドウの幅に合わせる"""

        width = max(event.width - 64, 200)

        for widget in self._wrapped:
            widget.configure(wraplength=width)

    # --- 操作 ---

    def _selected_category(self) -> Category | None:
        index = self.category_box.current()
        return self._categories[index] if 0 <= index < len(self._categories) else None

    def _start(self) -> None:
        category = self._selected_category()

        if category is not None:
            self._choice.set(-1)
            quiz_mode.start(self.state, category, self._rag)

    def _answer(self) -> None:
        choice = self._choice.get()

        if choice >= 0:
            quiz_mode.answer(self.state, choice)

    def _next(self) -> None:
        quiz_mode.next_question(self.state)

    def _quit(self) -> None:
        quiz_mode.quit_quiz(self.state)

    def _again(self) -> None:
        category = self.state.category

        if category is not None:
            self._choice.set(-1)
            quiz_mode.start(self.state, category, self._rag)

    def shutdown(self) -> None:
        self._test_cancel.cancel()
        quiz_mode.shutdown(self.state)

    # --- 分野の編集 ---

    def _open_editor(self) -> None:
        try:
            self._edit_items = quiz_mode.load_custom_categories()
        except config.ConfigError as e:
            # 壊れたファイルを上書きして中身を消さないように、編集させない
            self._custom_error = f"{e}。直すか削除してください"
            self._dirty.set()
            return

        self._custom_error = ""
        self._editing = True
        self._edit_index = -1
        self._test_result = ""
        self._test_running = False
        self.select_frame.pack_forget()
        self.edit_frame.pack(side="top", fill="both", expand=True)
        self._fill_edit_list(0 if self._edit_items else -1)
        self._dirty.set()

    def _close_editor(self) -> None:
        self._test_session += 1
        self._test_cancel.cancel()
        self._editing = False
        self.edit_frame.pack_forget()

        # 選択の画面を、分野を読み直して出し直す
        self._shown_phase = ""
        self._dirty.set()

    def _fill_edit_list(self, select: int) -> None:
        self.edit_list.delete(0, "end")

        for item in self._edit_items:
            self.edit_list.insert("end", item.name)

        self._edit_index = -1

        if 0 <= select < len(self._edit_items):
            self.edit_list.selection_set(select)
            self._load_edit_fields(select)
        else:
            self._load_edit_fields(-1)

    def _on_exclusive(self) -> None:
        """クラウドの名前を出し、資料とクラウドの両方がオンのときは警告を出す"""

        cloud_name = llm_client.load_cloud_settings().name
        self.cloud_check.configure(
            text=f"{cloud_name} で出題する (お題と問題を外部のサービスに送ります。利用料がかかります)")

        allow = llm_client.load_cloud_settings().allow_rag

        if allow:
            # 設定で許可されている: 両方オンにできるが、資料の一部が外に出るので警告する
            self.rag_check.state(["!disabled"])
            self.cloud_check.state(["!disabled"])
        else:
            # 資料を外に送らない設定 (既定): どちらか一方だけ
            self.rag_check.state(["disabled"] if self.edit_cloud.get() else ["!disabled"])
            self.cloud_check.state(["disabled"] if self.edit_rag.get() else ["!disabled"])

        if allow and self.edit_rag.get() and self.edit_cloud.get():
            self.cloud_warning.pack(side="top", anchor="w", pady=(2, 0), after=self.cloud_check)
        else:
            self.cloud_warning.pack_forget()

        self._dirty.set()

    def _load_edit_fields(self, index: int) -> None:
        """index の分野を右の入力欄に出す (-1 なら空にして使えなくする)"""

        self._edit_index = index
        state = "normal" if index >= 0 else "disabled"
        self.edit_name.configure(state=state)
        self.edit_prompt.configure(state=state)
        self.edit_name.delete(0, "end")
        self.edit_prompt.delete("1.0", "end")
        self.edit_rag.set(False)
        self.edit_cloud.set(False)
        self._test_result = ""
        self._test_session += 1
        self._test_cancel.cancel()
        self._test_running = False

        if index >= 0:
            item = self._edit_items[index]
            self.edit_name.insert(0, item.name)
            self.edit_prompt.insert("1.0", item.guide)
            self.edit_rag.set(item.use_rag)

            # 資料を外に送らない設定のときは、資料と両方オンにしない (保存済みでも外す)
            allow = llm_client.load_cloud_settings().allow_rag
            self.edit_cloud.set(item.use_cloud and (allow or not item.use_rag))

        self._on_exclusive()
        self._dirty.set()

    def _read_edit_fields(self) -> Category | None:
        """入力欄の内容 (選んでいなければ None)"""

        if self._edit_index < 0:
            return None

        return Category(
            key=f"custom{self._edit_index}",
            name=self.edit_name.get().strip()[:quiz_mode.MAX_NAME_LENGTH],
            source="custom",
            guide=self.edit_prompt.get("1.0", "end").strip()[:quiz_mode.MAX_PROMPT_LENGTH],
            use_rag=self.edit_rag.get(),
            use_cloud=self.edit_cloud.get())

    def _store_edit_fields(self) -> None:
        """入力欄の内容を、一覧のデータに書き戻す"""

        item = self._read_edit_fields()

        if item is not None:
            self._edit_items[self._edit_index] = item

            # 一覧の表示 (名前) も更新する
            self.edit_list.delete(self._edit_index)
            self.edit_list.insert(self._edit_index, item.name or "(名前なし)")
            self.edit_list.selection_set(self._edit_index)

    def _on_edit_select(self, _event: tk.Event) -> None:
        selection = self.edit_list.curselection()

        if not selection or selection[0] == self._edit_index:
            return

        # 移る前に、いまの入力を保存しておく (一覧の選択は、書き戻しで動かさない)
        self._store_edit_fields()
        self.edit_list.selection_clear(0, "end")
        self.edit_list.selection_set(selection[0])
        self._load_edit_fields(selection[0])

    def _edit_add(self) -> None:
        self._store_edit_fields()
        self._edit_items.append(Category(
            key="", name=NEW_NAME, source="custom", guide="", use_rag=False))
        self._fill_edit_list(len(self._edit_items) - 1)
        self.edit_name.focus_set()
        self.edit_name.select_range(0, "end")

    def _edit_delete(self) -> None:
        if self._edit_index < 0:
            return

        del self._edit_items[self._edit_index]
        self._fill_edit_list(min(self._edit_index, len(self._edit_items) - 1))

    def _validate_edit(self, item: Category, position: int) -> str:
        """保存できない理由 (問題なければ空)"""

        label = f"{position + 1} 番目の分野"

        if not item.name or item.name == NEW_NAME:
            return f"{label}の名前を入れてください"

        if not item.guide and not item.use_rag:
            return f"「{item.name}」のお題を書くか、「資料を使う」にしてください"

        return ""

    def _edit_save(self) -> None:
        self._store_edit_fields()

        for i, item in enumerate(self._edit_items):
            reason = self._validate_edit(item, i)

            if reason:
                self.edit_list.selection_clear(0, "end")
                self.edit_list.selection_set(i)
                self._load_edit_fields(i)
                self._test_result = f"保存できません: {reason}"
                self._dirty.set()
                return

        try:
            quiz_mode.save_custom_categories(self._edit_items)
        except config.ConfigError as e:
            self._test_result = f"保存できません: {e}"
            self._dirty.set()
            return

        self._close_editor()

    def _edit_test(self) -> None:
        item = self._read_edit_fields()

        if item is None or self._test_running:
            return

        reason = self._validate_edit(item, self._edit_index)

        if reason:
            self._test_result = reason
            self._dirty.set()
            return

        self._test_session += 1
        session = self._test_session
        self._test_cancel.cancel()
        cancel = self._test_cancel = CancelToken()
        self._test_running = True
        self._test_result = ""
        self._dirty.set()

        def run() -> None:
            try:
                question = quiz_mode.make_test_question(item, self._rag, cancel)
                lines = [f"Q. {question.text}"]
                lines += [f"{'○' if i == question.answer else '　'} {i + 1}. {c}"
                          for i, c in enumerate(question.choices)]
                lines.append(f"解説: {question.explanation}")

                if question.topic:
                    lines.append(f"(トピック: {question.topic})")

                if question.origin:
                    lines.append(f"(出典: {question.origin})")

                text = "\n".join(lines)
            except Cancelled:
                return
            except LLMError as e:
                text = f"作れませんでした: {e}"
            except Exception as e:  # ワーカーが黙って終わらないように
                text = f"想定外のエラー: {e}"

            if self._test_session == session:
                self._test_result = text
                self._test_running = False
                self._dirty.set()

        threading.Thread(target=run, daemon=True).start()

    # --- 描画 ---

    def _poll(self) -> None:
        if self._dirty.is_set():
            self._dirty.clear()
            self._refresh()

        self.after(POLL_INTERVAL_MS, self._poll)

    def _show_phase(self, phase: str) -> None:
        if phase == self._shown_phase:
            return

        self._shown_phase = phase

        for frame in (self.select_frame, self.play_frame, self.result_frame):
            frame.pack_forget()

        frame = {quiz_mode.PHASE_SELECT: self.select_frame,
                 quiz_mode.PHASE_PLAYING: self.play_frame,
                 quiz_mode.PHASE_RESULT: self.result_frame}[phase]
        frame.pack(side="top", fill="both", expand=True)

        if phase == quiz_mode.PHASE_SELECT:
            # 設定ファイルを直したあとにも反映されるように、選択に戻るたびに読み直す
            self._categories = quiz_mode.load_categories()
            self._custom_error = ""

            try:
                self._categories += quiz_mode.load_custom_categories()
            except config.ConfigError as e:
                self._custom_error = f"自作の分野を読めません: {e}"
            self.category_box.configure(
                values=[f"{c.name} (自作)" if c.custom else c.name for c in self._categories])

            if self._categories and self.category_box.current() < 0:
                self.category_box.current(0)

    def _refresh_edit(self) -> None:
        colors = self._colors()
        selected = self._edit_index >= 0
        usable = selected and server_manager.is_chat_available() and not self._test_running
        self.test_button.state(["!disabled"] if usable else ["disabled"])

        if self._test_running:
            note = "作っています…"
        elif selected and not server_manager.is_chat_available():
            note = "サーバーの準備ができるとテストできます"
        else:
            note = ""

        self.edit_message.configure(text=note, foreground=colors["sub_text"])
        self.cloud_warning.configure(foreground=colors["error"])
        self.test_output.configure(state="normal")
        self.test_output.delete("1.0", "end")
        self.test_output.insert("end", self._test_result)
        self.test_output.configure(state="disabled")

    def _refresh(self) -> None:
        if self._editing:
            self._refresh_edit()
            return

        state = self.state

        with state.lock:
            phase = state.phase
            total = state.total
            index = state.index
            question = state.questions[index] if index < len(state.questions) else None
            answered = state.answered
            chosen = state.answers[index] if answered and index < len(state.answers) else -1
            score = state.score()
            answered_count = len(state.answers)
            generating = state.generating
            error = state.error
            comment = state.comment
            pending = state.comment_pending
            misses = state.misses()
            ready_next = index + 1 < len(state.questions)
            is_last = state.is_last()

        self._show_phase(phase)
        colors = self._colors()

        if phase == quiz_mode.PHASE_SELECT:
            self._refresh_select(error, colors)
        elif phase == quiz_mode.PHASE_PLAYING:
            self._refresh_play(
                total, index, question, answered, chosen, score, generating,
                ready_next, is_last, colors)
        else:
            self._refresh_result(
                score, answered_count, comment, pending, misses, colors)

    def _refresh_select(self, error: str, colors: dict[str, str]) -> None:
        category = self._selected_category()

        if not self._categories:
            note = "分野がありません。config/quiz.json の categories を確認してください。"
        elif category is None:
            note = ""
        elif category.custom:
            note = category.guide
            if category.use_rag:
                note = (note + "\n" if note else "") + (
                    RAG_CUSTOM_DESCRIPTION if note else RAG_DESCRIPTION)
            if category.cloud:
                note = (note + "\n" if note else "") + (
                    f"{llm_client.load_cloud_settings().name} で出題します"
                    "(お題と問題を外部のサービスに送ります)。")
        elif category.source == "rag":
            note = RAG_DESCRIPTION
        else:
            note = category.guide

        warning = ""

        if category is not None and category.custom and category.use_cloud and category.from_rag:
            warning = CLOUD_RAG_WARNING if category.cloud else "資料を使う分野では、設定で「資料 (rag/) の内容を送ってよい」をオンにしないと、ChatGPT は使いません (ローカルで出題します)。"
        self.category_note.configure(text=note, foreground=colors["sub_text"])
        self.select_error.configure(
            text=error or self._custom_error or warning, foreground=colors["error"])

        # ローカルの LLM に頼む分野は、サーバーの準備ができるまで始められない
        usable = category is not None and server_manager.is_chat_available()
        self.start_button.state(["!disabled"] if usable else ["disabled"])

    def _refresh_play(
        self, total: int, index: int, question: quiz_mode.Question | None,
        answered: bool, chosen: int, score: int, generating: bool,
        ready_next: bool, is_last: bool, colors: dict[str, str],
    ) -> None:
        header = f"Q{index + 1} / {total}    正解 {score}"

        if question is not None and question.topic:
            header += f"    [{question.topic}]"

        self.header.configure(text=header)

        if question is not self._shown_question:
            self._shown_question = question
            self._choice.set(-1)

        if question is None:
            self.question_label.configure(text="問題を作っています…")

            for button in self.choice_buttons:
                button.configure(text="", state="disabled")

            self.verdict.configure(text="")
            self.explanation.configure(text="")
            self.play_status.configure(text="")
            self.answer_button.state(["disabled"])
            self.next_button.state(["disabled"])
            return

        self.question_label.configure(text=question.text)

        for i, button in enumerate(self.choice_buttons):
            color = colors["text"]

            if answered and i == question.answer:
                color = colors["ai"]
            elif answered and i == chosen:
                color = colors["error"]

            button.configure(
                text=f"{i + 1}. {question.choices[i]}",
                state="disabled" if answered else "normal",
                disabledforeground=color if answered else colors["sub_text"])

        if answered:
            correct = chosen == question.answer
            self.verdict.configure(
                text="○ 正解です！" if correct
                else f"× 不正解です。正解は {question.answer + 1}. です",
                foreground=colors["ai"] if correct else colors["error"])
            detail = question.explanation

            if question.origin:
                detail += f"\n(出典: {question.origin})"

            self.explanation.configure(text=detail)
        else:
            self.verdict.configure(text="")
            self.explanation.configure(text="")

        status = "次の問題を準備中…" if answered and not ready_next and not is_last else ""
        self.play_status.configure(text=status, foreground=colors["sub_text"])
        self.next_button.configure(text="結果を見る" if is_last else "次の問題へ")

        can_answer = not answered and self._choice.get() >= 0
        can_next = answered and (ready_next or is_last)
        self.answer_button.state(["!disabled"] if can_answer else ["disabled"])
        self.next_button.state(["!disabled"] if can_next else ["disabled"])

    def _refresh_result(
        self, score: int, count: int, comment: str, pending: bool,
        misses: list[tuple[int, quiz_mode.Question, int]], colors: dict[str, str],
    ) -> None:
        percent = round(100 * score / count) if count else 0
        self.score_label.configure(text=f"{score} / {count} 問正解 ({percent}%)")
        self.comment_label.configure(
            text=comment + ("…" if pending else ""))

        signature = tuple((i, q.text, a) for i, q, a in misses)

        if signature == self._shown_review:
            return

        self._shown_review = signature
        self.review.configure(state="normal")
        self.review.delete("1.0", "end")

        if not misses:
            self.review.insert("end", "間違えた問題はありません。全問正解です！", "right")

        for i, q, a in misses:
            self.review.insert("end", f"Q{i + 1}. {q.text}\n", "title")
            self.review.insert(
                "end", f"  あなたの答え: {a + 1}. {q.choices[a]}\n", "wrong")
            self.review.insert(
                "end", f"  正解: {q.answer + 1}. {q.choices[q.answer]}\n", "right")

            if q.explanation:
                self.review.insert("end", f"  {q.explanation}\n", "sub")

            if q.origin:
                self.review.insert("end", f"  (出典: {q.origin})\n", "sub")

            self.review.insert("end", "\n")

        self.review.configure(state="disabled")
