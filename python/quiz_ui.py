"""クイズタブの画面 (Tk)"""

from __future__ import annotations

import threading
import tkinter as tk
import tkinter.font as tkfont
from tkinter import ttk
from typing import Callable

import quiz_mode
import rag
import server_manager
from quiz_mode import CHOICE_COUNT, Category, QuizState

# ワーカースレッドからの更新を見に行く間隔 (ミリ秒)
POLL_INTERVAL_MS = 50

NOTICE = ("AI が作った問題です。特に「資料から」以外の分野は、正解や解説が誤っていることがあります。"
          "気になったら調べ直してください。")

RAG_DESCRIPTION = "rag/ フォルダーの資料の内容から出題します (資料の読み込みが終わってから)。"


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
        self._shown_phase = ""

        # 出題中: 画面に出している問題 (変わったら、選択を外す)
        self._shown_question: object = None
        self._choice = tk.IntVar(value=-1)

        # 結果: 振り返りを描いた内容 (変わったときだけ描き直す)
        self._shown_review: tuple | None = None

        self._wrapped: list[ttk.Label | tk.Radiobutton] = []

        self._build_select()
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

        self.category_note = self._label(self.select_frame)
        self.category_note.pack(side="top", anchor="w", pady=(4, 0))

        self.select_error = self._label(self.select_frame)
        self.select_error.pack(side="top", anchor="w", pady=(12, 0))

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
        quiz_mode.shutdown(self.state)

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
            self.category_box.configure(values=[c.name for c in self._categories])

            if self._categories and self.category_box.current() < 0:
                self.category_box.current(0)

    def _refresh(self) -> None:
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
        elif category.source == "rag":
            note = RAG_DESCRIPTION
        else:
            note = category.guide

        self.category_note.configure(text=note, foreground=colors["sub_text"])
        self.select_error.configure(text=error, foreground=colors["error"])

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
