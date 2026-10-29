"""RAG のインデックス作成と検索"""

from __future__ import annotations

import json
import logging
import math
import os
import threading
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable

from llm_client import CancelToken, LLMError, embed

log = logging.getLogger(__name__)

# 1チャンクの最大文字数
MAX_CHUNK_CHARS = 400

# 1回の埋め込みリクエストで送るチャンク数
EMBED_BATCH = 16

# 質問に添えるチャンクの最大数と、関係ありとみなす類似度の下限
TOP_K = 3
MIN_SCORE = 0.5

# チャンクの分け方を変えたら上げる (保存済みのインデックスを作り直す)
# (C++ 版と同じ形式で保存する)
INDEX_VERSION = 2

# ルーターに見せる、見出しの無い資料の書き出しの文字数
PREVIEW_CHARS = 40

# このスクリプトのフォルダ (インデックスの保存先)
BASE_DIRECTORY = Path(__file__).resolve().parent

INDEX_FILE = BASE_DIRECTORY / "rag-index.json"


@dataclass
class RagChunk:
    """文書を分割した1かたまり"""

    source: str   # rag/ からの相対パス
    heading: str  # Markdown の見出し (無ければ空)
    text: str
    vector: list[float] = field(default_factory=list)  # 正規化済みの埋め込み


@dataclass
class RagHit:
    """検索で見つかったチャンク"""

    source: str
    heading: str
    text: str
    score: float  # コサイン類似度


class RagState:
    """UIスレッド・インデックス作成スレッド・会話スレッドで共有する状態"""

    def __init__(self, notify: Callable[[], None]) -> None:
        self.lock = threading.Lock()
        self.chunks: list[RagChunk] = []
        self.building = False
        self.status = ""
        self.cancel = CancelToken()
        self.worker: threading.Thread | None = None

        # UIスレッドに再描画を頼む (どのスレッドからでも呼べる)
        self.notify = notify


def rag_directory() -> Path:
    """環境変数 LOCALLLM_RAG_DIR、無ければリポジトリの rag/"""

    if env := os.environ.get("LOCALLLM_RAG_DIR"):
        return Path(env)

    return BASE_DIRECTORY.parent / "rag"


def _set_status(state: RagState, status: str) -> None:
    with state.lock:
        state.status = status
    state.notify()


def _hash_text(text: str) -> str:
    """ファイルの変更検出用 (64bit FNV-1a、C++ 版と同じ値になる)"""

    value = 14695981039346656037

    for c in text.encode("utf-8"):
        value ^= c
        value = (value * 1099511628211) & 0xFFFFFFFFFFFFFFFF

    return f"{value:016x}"


def _read_text_file(path: Path) -> str | None:
    """UTF-8 のテキストファイルを読む (UTF-8 でなければ None)"""

    try:
        data = path.read_bytes()
    except OSError:
        return None

    if data.startswith(b"\xef\xbb\xbf"):
        data = data[3:]

    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError:
        return None

    return text.replace("\r", "")


@dataclass
class _Section:
    """見出しで区切った文書の一部分"""

    heading: str
    paragraphs: list[str] = field(default_factory=list)


def _parse_sections(text: str, markdown: bool) -> list[_Section]:
    """見出しと空行で、セクションと段落に分ける"""

    sections = [_Section("")]

    # 階層ごとの見出し (headings[0] が # の見出し)
    headings: list[str] = []

    paragraph = ""
    in_code_block = False

    def end_paragraph() -> None:
        nonlocal paragraph
        if paragraph.strip(" \t\n"):
            sections[-1].paragraphs.append(paragraph)
        paragraph = ""

    lines = text.split("\n")
    if lines and lines[-1] == "":
        lines.pop()

    for line in lines:
        if markdown and line.startswith("```"):
            in_code_block = not in_code_block

        if markdown and not in_code_block:
            level = len(line) - len(line.lstrip("#"))

            if 1 <= level <= 6 and level < len(line) and line[level] == " ":
                end_paragraph()

                headings = headings[:level - 1]
                headings += [""] * (level - 1 - len(headings))
                headings.append(line[level + 1:])

                # 「文書名 > 章 > 節」の形にする
                path = " > ".join(heading for heading in headings if heading)
                sections.append(_Section(path))
                continue

        if not in_code_block and not line.strip(" \t"):
            end_paragraph()
            continue

        if paragraph:
            paragraph += "\n"
        paragraph += line

    end_paragraph()
    return sections


def _split_by_length(text: str, out: list[str]) -> None:
    """MAX_CHUNK_CHARS 文字ごとに切る"""

    for start in range(0, max(len(text), 1), MAX_CHUNK_CHARS):
        out.append(text[start:start + MAX_CHUNK_CHARS])


def _split_into_chunks(source: str, text: str, markdown: bool) -> list[RagChunk]:
    chunks: list[RagChunk] = []

    for section in _parse_sections(text, markdown):
        # 長すぎる段落は行で、それでも長い行は文字数で分ける
        units: list[str] = []

        for paragraph in section.paragraphs:
            if len(paragraph) <= MAX_CHUNK_CHARS:
                units.append(paragraph)
                continue

            for line in paragraph.split("\n"):
                if len(line) <= MAX_CHUNK_CHARS:
                    units.append(line)
                else:
                    _split_by_length(line, units)

        # 上限を超えない範囲で段落をまとめる
        # (見出しの無いテキストは話題が混ざらないように段落ごとに分ける)
        current = ""

        for unit in units:
            if current and (not markdown or
                            len(current) + len(unit) > MAX_CHUNK_CHARS):
                chunks.append(RagChunk(source, section.heading, current))
                current = ""

            if current:
                current += "\n\n"
            current += unit

        if current:
            chunks.append(RagChunk(source, section.heading, current))

    return chunks


def _embedding_text(chunk: RagChunk) -> str:
    """埋め込みに使う文章 (見出しを付けると、どの話題か伝わりやすい)"""

    title = chunk.heading or Path(chunk.source).stem
    return title + "\n" + chunk.text


def _normalize(vector: list[float]) -> list[float]:
    norm = math.sqrt(sum(value * value for value in vector))
    return [value / norm for value in vector] if norm > 0.0 else vector


# rag/ からの相対パス → (ハッシュ, チャンク)
_IndexMap = dict[str, tuple[str, list[RagChunk]]]


def _load_index(path: Path) -> _IndexMap:
    try:
        data = json.loads(path.read_bytes())
    except (OSError, ValueError):
        return {}

    if not isinstance(data, dict) or data.get("version") != INDEX_VERSION:
        return {}

    index: _IndexMap = {}

    try:
        for source, entry in data["files"].items():
            index[source] = (
                str(entry["hash"]),
                [RagChunk(source, str(chunk["heading"]), str(chunk["text"]),
                          [float(v) for v in chunk["vector"]])
                 for chunk in entry["chunks"]],
            )
    except (KeyError, TypeError, ValueError, AttributeError):
        # 壊れていたら作り直す
        return {}

    return index


def _save_index(path: Path, index: _IndexMap) -> None:
    files = {
        source: {
            "hash": file_hash,
            "chunks": [
                {"heading": chunk.heading, "text": chunk.text,
                 "vector": chunk.vector}
                for chunk in chunks
            ],
        }
        for source, (file_hash, chunks) in index.items()
    }

    data = {"version": INDEX_VERSION, "files": files}

    # 書き込み途中で落ちても壊れないように、一時ファイルから置き換える
    temp = path.with_name(path.name + ".tmp")
    temp.write_text(
        json.dumps(data, ensure_ascii=False, separators=(",", ":")),
        encoding="utf-8")
    os.replace(temp, path)


def _find_documents(directory: Path) -> list[Path]:
    """rag/ の中の .md / .txt を集める (隠しファイルと README.md は除く)"""

    files: list[Path] = []

    for root, dirs, names in os.walk(directory):
        dirs[:] = [name for name in dirs if not name.startswith(".")]
        root_path = Path(root)

        for name in names:
            path = root_path / name

            if name.startswith(".") or not path.is_file():
                continue

            # rag/README.md はフォルダの説明なので対象外
            if path == directory / "README.md":
                continue

            if path.suffix.lower() in (".md", ".markdown", ".txt"):
                files.append(path)

    return sorted(files)


def _build_index(state: RagState) -> None:
    directory = rag_directory()

    if not directory.is_dir():
        with state.lock:
            state.chunks = []
            state.status = f"フォルダがありません: {directory}"
        return

    previous = _load_index(INDEX_FILE)
    next_index: _IndexMap = {}
    skipped = 0

    for path in _find_documents(directory):
        source = path.relative_to(directory).as_posix()
        text = _read_text_file(path)

        if text is None:
            log.info("RAG: skipped (not UTF-8): %s", source)
            skipped += 1
            continue

        file_hash = _hash_text(text)

        # 変更の無いファイルは埋め込み済みのチャンクを使い回す
        found = previous.get(source)
        if found and found[0] == file_hash:
            next_index[source] = found
            continue

        markdown = path.suffix.lower() != ".txt"
        next_index[source] = (
            file_hash, _split_into_chunks(source, text, markdown))

    # まだ埋め込んでいないチャンクを埋め込む
    pending = [chunk
               for _, chunks in next_index.values()
               for chunk in chunks
               if not chunk.vector]

    for i in range(0, len(pending), EMBED_BATCH):
        _set_status(state, f"インデックス作成中 {i} / {len(pending)}")

        batch = pending[i:i + EMBED_BATCH]
        vectors = embed([_embedding_text(chunk) for chunk in batch],
                        state.cancel)

        for chunk, vector in zip(batch, vectors):
            chunk.vector = _normalize(vector)

    _save_index(INDEX_FILE, next_index)

    chunks = [chunk for _, file_chunks in next_index.values()
              for chunk in file_chunks]

    status = f"{len(next_index)} ファイル / {len(chunks)} チャンク"

    if skipped > 0:
        status += f" (UTF-8 でない {skipped} ファイルを除外)"

    with state.lock:
        state.chunks = chunks
        state.status = status


def start_indexing(state: RagState) -> None:
    """rag/ を走査してインデックスをバックグラウンドで作る
    (変更の無いファイルは保存済みのインデックスを再利用する)"""

    join_worker(state)

    with state.lock:
        state.building = True
        state.status = "インデックスを準備中..."

    state.cancel.reset()

    def run() -> None:
        try:
            _build_index(state)
        except Exception as e:
            # 失敗しても前回までのインデックスは使えるように残す
            with state.lock:
                state.status = f"インデックス作成に失敗: {e}"

        with state.lock:
            state.building = False
        state.notify()

    state.worker = threading.Thread(target=run, daemon=True)
    state.worker.start()


def join_worker(state: RagState) -> None:
    if state.worker is not None:
        state.worker.join()
        state.worker = None


def search(state: RagState, query: str, cancel: CancelToken) -> list[RagHit]:
    """質問に近いチャンクを返す (埋め込みサーバーに問い合わせるので
    ワーカースレッドから呼ぶ)"""

    with state.lock:
        if not state.chunks:
            raise LLMError("インデックスがありません")

    query_vector = _normalize(embed([query], cancel)[0])

    with state.lock:
        # 正規化済みなので内積がコサイン類似度
        hits = [
            RagHit(chunk.source, chunk.heading, chunk.text,
                   sum(a * b for a, b in zip(chunk.vector, query_vector)))
            for chunk in state.chunks
            if len(chunk.vector) == len(query_vector)
        ]

    hits.sort(key=lambda hit: hit.score, reverse=True)

    # 調整用に上位の類似度をログに出す
    for hit in hits[:5]:
        log.info("RAG: %.3f %s / %s", hit.score, hit.source, hit.heading)

    return [hit for hit in hits if hit.score >= MIN_SCORE][:TOP_K]


def describe_sources(state: RagState) -> list[str]:
    """インデックスにある資料ごとに「名前: 見出し / 見出し …」の 1 行を返す
    (見出しが無い資料は書き出し。ルーターに資料の中身の見当をつけさせる)"""

    # 資料ごとの見出し (階層の最後の部分) と、最初のチャンクの本文
    sources: dict[str, tuple[list[str], str]] = {}

    with state.lock:
        for chunk in state.chunks:
            headings, _ = sources.setdefault(chunk.source, ([], chunk.text))

            if not chunk.heading:
                continue

            heading = chunk.heading.rsplit(" > ", 1)[-1]

            if heading not in headings:
                headings.append(heading)

    descriptions = []

    for name in sorted(sources):
        headings, first_text = sources[name]

        if headings:
            description = " / ".join(headings)
        else:
            # 見出しが無いときは書き出しを見せる
            preview = first_text.replace("\r", " ").replace("\n", " ")
            if len(preview) > PREVIEW_CHARS:
                preview = preview[:PREVIEW_CHARS] + "…"
            description = preview

        descriptions.append(f"{name}: {description}")

    return descriptions


def build_prompt(hits: list[RagHit], question: str) -> str:
    """検索結果を参考資料として質問に添えたプロンプトを作る"""

    prompt = (
        "以下の資料を参考にして、最後の質問に答えてください。"
        "資料に関係する質問なら資料に基づいて答え、"
        "資料に書かれていないことは推測しないでください。"
        "資料が質問と関係なければ、資料は無視して普通に答えてください。\n\n"
    )

    for i, hit in enumerate(hits):
        prompt += f"[資料{i + 1}] {hit.source}"

        if hit.heading:
            prompt += f" ({hit.heading})"

        prompt += f"\n{hit.text}\n\n"

    prompt += "質問: " + question
    return prompt
