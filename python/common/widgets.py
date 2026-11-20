"""Tk でよく使う書き方をまとめた小さなヘルパー

C++ 版の common/widgets.h (ImGui の Begin/End・Push/Pop の組をスコープにするラッパー) に当たる。
Tk は部品が状態を持つ (保持モード) ので、ImGui のラッパーをそのまま移さず、
このアプリで何度も出てくる書き方だけを関数にしている。

    set_enabled(self.send_button, usable)

    with editable(self.log_text):
        self.log_text.delete("1.0", "end")
        self.log_text.insert("end", text)
"""

from __future__ import annotations

import tkinter as tk
from collections.abc import Iterator
from contextlib import contextmanager
from tkinter import ttk
from typing import Any


def set_enabled(widget: ttk.Widget, enabled: bool) -> None:
    """ttk の部品を押せる (enabled) か、押せない (薄く表示する) ようにする"""

    widget.state(["!disabled"] if enabled else ["disabled"])


@contextmanager
def editable(text: tk.Text) -> Iterator[tk.Text]:
    """読み取り専用の Text を、with の中だけ書き換えられるようにする
    (途中で return や例外で抜けても、読み取り専用に戻す)"""

    text.configure(state="normal")

    try:
        yield text
    finally:
        text.configure(state="disabled")


def scrolled_text(parent: tk.Misc, **options: Any) -> tk.Text:
    """縦のスクロールバー付きの Text を parent に詰める (スクロールバーは右端)

    parent には、この Text とスクロールバーだけを入れる枠を渡す"""

    text = tk.Text(parent, **options)
    scrollbar = ttk.Scrollbar(parent, orient="vertical", command=text.yview)
    text.configure(yscrollcommand=scrollbar.set)

    scrollbar.pack(side="right", fill="y")
    text.pack(side="left", fill="both", expand=True)

    return text
