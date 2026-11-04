"""役割ごとの LLM の呼び出し (ルーター / アシスタント / 侍)"""

from __future__ import annotations

import json
import os
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from llm_client import CancelToken, DeltaCallback, LLMError, LLMOptions, ask_llm

# ルーターに見せる直前の会話の数と、1件あたりの長さ (バイト)
ROUTER_HISTORY_MESSAGES = 4
ROUTER_HISTORY_BYTES = 300

# リポジトリのフォルダ (このスクリプトの1つ上)
BASE_DIRECTORY = Path(__file__).resolve().parent.parent


@dataclass
class Agent:
    """役割 (system プロンプト) と設定を持つエージェント
    同じ LLM サーバーを、役割を変えて呼び分ける"""

    name: str
    system_prompt: str
    options: LLMOptions = field(default_factory=LLMOptions)


@dataclass
class RouteDecision:
    """ルーターの判断"""

    use_rag: bool
    reason: str


def _truncate(text: str, max_bytes: int) -> str:
    """UTF-8 の文字の途中で切らないように、先頭 max_bytes までに縮める"""

    data = text.encode("utf-8")

    if len(data) <= max_bytes:
        return text

    return data[:max_bytes].decode("utf-8", errors="ignore") + "…"


def agents_file() -> Path:
    """エージェントの設定ファイル
    環境変数 LOCALLLM_AGENTS_FILE、無ければリポジトリの config/agents.json"""

    if env := os.environ.get("LOCALLLM_AGENTS_FILE"):
        return Path(env)

    return BASE_DIRECTORY / "config" / "agents.json"


def _load_agent(key: str, options: LLMOptions | None = None) -> Agent:
    """設定ファイルから key のエージェントの名前と system プロンプトを読む
    質問のたびに読み直すので、ファイルを書き換えればアプリを再起動しなくても反映される
    system_prompt は文字列か、文字列の配列 (つなげて1つにする)"""

    path = agents_file()

    try:
        config = json.loads(path.read_bytes())
    except OSError as e:
        raise LLMError(f"エージェントの設定ファイルを開けません: {path}") from e
    except ValueError as e:
        raise LLMError(
            f"エージェントの設定ファイルを JSON として読めません: {path}") from e

    entry = config.get(key) if isinstance(config, dict) else None

    if not isinstance(entry, dict):
        raise LLMError(f"エージェントの設定ファイルに {key} がありません: {path}")

    prompt = entry.get("system_prompt")

    if isinstance(prompt, list):
        if not all(isinstance(line, str) for line in prompt):
            raise LLMError(f"{key} の system_prompt に文字列以外が含まれています")
        prompt = "".join(prompt)

    if not isinstance(prompt, str):
        raise LLMError(f"{key} の system_prompt がありません")

    return Agent(
        name=str(entry.get("name", key)),
        system_prompt=prompt,
        options=options or LLMOptions(),
    )


def _create_router() -> Agent:
    return _load_agent(
        "router",
        # 判断だけなので短く、毎回同じ答えになるように
        # 先に理由を書かせてから結論を出させる
        LLMOptions(
            max_tokens=128,
            temperature=0.0,
            disable_thinking=True,
            schema={
                "type": "object",
                "properties": {
                    "reason": {"type": "string"},
                    "use_rag": {"type": "boolean"},
                },
                "required": ["reason", "use_rag"],
            },
        ),
    )


def create_assistant() -> Agent:
    """資料を参照する質問に、資料に基づいて普通の口調で答える"""

    return _load_agent("assistant")


def create_general() -> Agent:
    """資料の要らない質問 (雑談や一般的な質問) に答える
    口調は config/agents.json の system プロンプトで決まる (既定は時代劇の侍の口調)"""

    return _load_agent("general")


def run_agent(
    agent: Agent,
    messages: list[dict[str, str]],
    on_delta: DeltaCallback | None,
    cancel: CancelToken,
) -> str:
    """エージェントに会話 (system を除いた user / assistant) を渡して回答させる"""

    request = [{"role": "system", "content": agent.system_prompt}, *messages]
    return ask_llm(request, on_delta, cancel, agent.options)


def run_agent_json(
    agent: Agent,
    messages: list[dict[str, str]],
    cancel: CancelToken,
) -> dict[str, Any]:
    """エージェントに JSON で答えさせる (agent.options.schema が必要)"""

    answer = run_agent(agent, messages, None, cancel)

    try:
        result = json.loads(answer)
    except json.JSONDecodeError:
        result = None

    if not isinstance(result, dict):
        raise LLMError(f"{agent.name}の回答を JSON として読めませんでした")

    return result


def route_question(
    history: list[dict[str, str]],
    question: str,
    sources: list[str],
    cancel: CancelToken,
) -> RouteDecision:
    """質問に rag/ の資料が必要かをルーターに判断させる
    history は直前までの会話 (指示語を含む質問の判断に使う)
    sources は rag/ にある資料の説明 (rag.describe_sources の結果)"""

    content = "## 資料の一覧\n"

    for source in sources:
        content += f"- {source}\n"

    if history:
        content += "\n## 直前の会話\n"

        for message in history[-ROUTER_HISTORY_MESSAGES:]:
            role = "ユーザー" if message.get("role") == "user" else "AI"
            content += (
                f"{role}: "
                f"{_truncate(message.get('content', ''), ROUTER_HISTORY_BYTES)}\n")

    content += "\n## 質問\n" + question

    result = run_agent_json(
        _create_router(), [{"role": "user", "content": content}], cancel)

    use_rag = result.get("use_rag")

    if not isinstance(use_rag, bool):
        raise LLMError("ルーターの回答に use_rag がありません")

    return RouteDecision(use_rag, str(result.get("reason", "")))
