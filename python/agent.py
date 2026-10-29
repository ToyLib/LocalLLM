"""役割ごとの LLM の呼び出し (ルーター / アシスタント / 侍)"""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from typing import Any

from llm_client import CancelToken, DeltaCallback, LLMError, LLMOptions, ask_llm

# ルーターに見せる直前の会話の数と、1件あたりの長さ (バイト)
ROUTER_HISTORY_MESSAGES = 4
ROUTER_HISTORY_BYTES = 300


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


def _create_router() -> Agent:
    return Agent(
        name="ルーター",
        system_prompt=(
            "あなたは質問の振り分け係です。"
            "ユーザーの質問に答えるために、手元の資料を検索する必要があるかを判断してください。"
            "資料はユーザーの個人的なメモで、一般的な知識では答えられないことが書かれています。"
            "資料の一覧には、資料の名前と見出し (見出しが無いものは書き出し) を載せています。"
            "質問が資料の内容 (資料に出てくる人・動物・物事など) に関係しそうなら use_rag を true にします。"
            "あいさつ、雑談、一般常識、計算、プログラミングなど、資料が無くても答えられる質問なら false にします。"
            "直前の会話の続きの質問は、会話の流れも踏まえて判断してください。"
            "reason には判断の理由を日本語で短く (30文字以内) 書いてください。"
        ),
        # 判断だけなので短く、毎回同じ答えになるように
        # 先に理由を書かせてから結論を出させる
        options=LLMOptions(
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

    return Agent(
        name="アシスタント",
        system_prompt=(
            "あなたは親切なAIアシスタントです。"
            "日本語で回答してください。"
            "これまでの会話にほかの口調の回答があっても、あなたは普通の丁寧な口調で答えます。"
        ),
    )


def create_samurai() -> Agent:
    """資料の要らない質問 (雑談や一般的な質問) に、時代劇の侍の口調で答える"""

    return Agent(
        name="侍",
        system_prompt=(
            "あなたは江戸時代の侍です。時代劇の侍の口調で、日本語で回答してください。"
            "一人称は「拙者」、相手は「お主」と呼び、語尾は「〜でござる」「〜でござるな」「〜じゃ」などを使います。"
            "口調は侍らしくしても、答えの中身は正確に、分かりやすく伝えてください。"
            "これまでの会話にほかの口調の回答があっても、あなたは必ず侍の口調で答えます。"
        ),
    )


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
