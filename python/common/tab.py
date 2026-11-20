"""メインウィンドウのタブ (チャット・クイズ・迷路) の共通の形

C++ 版の common/tab.h (Tab / UiContext) に当たる。
Application が Notebook に並べ、設定の保存と終了をすべてのタブに知らせる。
"""

from __future__ import annotations

import threading
import tkinter as tk
import tkinter.font as tkfont
from collections.abc import Callable
from dataclasses import dataclass
from tkinter import ttk


@dataclass(frozen=True)
class UiContext:
    """どのタブにも渡すもの (フォントと色は Application が持ち、設定画面で変わる)"""

    font: tkfont.Font
    bold: tkfont.Font
    small: tkfont.Font

    # 画面の色 (設定画面を開いている間は、編集中の色)
    colors: Callable[[], dict[str, str]]


class Tab(ttk.Frame):
    """タブの共通の形

    C++ 版は選ばれているタブの Draw を毎フレーム呼ぶが、Tk は部品が状態を持つので Draw は無い。
    代わりに、各タブが after で _dirty を見に行き、変わったときだけ部品を書き換える。
    閉じているタブの仕事も、ワーカースレッドで動き続ける。
    """

    # タブに出す名前
    label = ""

    def __init__(self, parent: tk.Misc, context: UiContext) -> None:
        super().__init__(parent)

        self._context = context

        # ワーカースレッドから立てて、UI スレッドで見に行く
        self._dirty = threading.Event()

    def wake(self) -> None:
        """描き直しを頼む (どのスレッドから呼んでもよい)"""

        self._dirty.set()

    def apply_colors(self) -> None:
        """色が変わったときに呼ぶ (context.colors() の色を部品に付け直す)"""

    def on_settings_saved(self) -> None:
        """設定画面で保存したあとに呼ぶ (設定を読み直す)"""

    def shutdown(self) -> None:
        """終了するときに呼ぶ (通信を止める。ワーカーはデーモンスレッドなので待たない)"""
