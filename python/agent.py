"""役割ごとの LLM の呼び出し (ルーター / アシスタント / 侍)"""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from typing import Any

import config
from llm_client import (
    CancelToken, DeltaCallback, LLMError, LLMOptions, ask_llm, load_cloud_settings)

# agents.json に cloud / rewriter が無いとき (前の版の設定ファイル) に使う名前と system プロンプト
DEFAULT_AGENTS = {
    "cloud": (
        "ChatGPT",
        "あなたは親切で正確なAIアシスタントです。日本語で、分かりやすく回答してください。",
    ),
    "rewriter": (
        "書き直し",
        "これから渡す「回答」を、あなたの口調で言い直してください。"
        "内容・数値・手順・固有名詞・コードは変えず、足したり削ったりしないでください。"
        "コードやコマンドはそのまま書き写します。"
        "言い直した回答だけを書き、前置きや説明は付けないでください。",
    ),
}

# 書き直しの最大トークン数 (ChatGPT の長い回答も入るように)
REWRITE_MAX_TOKENS = 4096

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

    # config/agents.json のキー (assistant / general など)
    key: str = ""

    # ChatGPT に聞き直した回答を、この回答役の口調で書き直すか (agents.json の rewrite)
    rewrite: bool = True


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


def _load_agent(key: str, options: LLMOptions | None = None) -> Agent:
    """config/agents.json から key のエージェントの名前と system プロンプトを読む
    質問のたびに読み直すので、ファイルを書き換えればアプリを再起動しなくても反映される
    system_prompt は文字列か、文字列の配列 (つなげて1つにする)"""

    entry = config.load("agents.json").get(key)

    if not isinstance(entry, dict):
        raise config.ConfigError(f"設定ファイル agents.json に {key} がありません")

    prompt = entry.get("system_prompt")

    if isinstance(prompt, list):
        if not all(isinstance(line, str) for line in prompt):
            raise config.ConfigError(f"{key} の system_prompt に文字列以外が含まれています")
        prompt = "".join(prompt)

    if not isinstance(prompt, str):
        raise config.ConfigError(f"{key} の system_prompt がありません")

    rewrite = entry.get("rewrite", key != "assistant")

    return Agent(
        name=str(entry.get("name", key)),
        system_prompt=prompt,
        options=options or LLMOptions(),
        key=key,
        rewrite=rewrite if isinstance(rewrite, bool) else key != "assistant",
    )


def _load_agent_or_default(key: str) -> Agent:
    """key のエージェントを読む。agents.json に無ければ既定のもの
    (設定ファイルが前の版のままでも動くように)"""

    if key in config.load("agents.json"):
        return _load_agent(key)

    name, prompt = DEFAULT_AGENTS[key]
    return Agent(name=name, system_prompt=prompt, key=key)


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


def create_answerer(key: str) -> Agent:
    """key (assistant / general) の回答役"""

    if key not in ("assistant", "general"):
        raise LLMError(f"回答役ではありません: {key}")

    return _load_agent(key)


def create_cloud() -> Agent:
    """ChatGPT (server.json の cloud) に聞き直すエージェント
    system プロンプトは agents.json の cloud (無ければ既定のもの)"""

    cloud = load_cloud_settings()
    agent = _load_agent_or_default("cloud")

    # 名前は server.json の cloud.name に合わせる
    agent.name = cloud.name
    agent.options = LLMOptions(cloud=True)
    return agent


def rewrite_answer(
    persona: Agent,
    question: str,
    answer: str,
    on_delta: DeltaCallback | None,
    cancel: CancelToken,
) -> str:
    """クラウドの回答 answer を、回答役 persona の口調で書き直す (ローカルの LLM で)
    書き直しの指示は agents.json の rewriter (無ければ既定のもの)"""

    rewriter = _load_agent_or_default("rewriter")

    agent = Agent(
        name=persona.name,
        system_prompt=persona.system_prompt + "\n\n" + rewriter.system_prompt,
        options=LLMOptions(max_tokens=REWRITE_MAX_TOKENS, disable_thinking=True),
        key=persona.key,
        rewrite=persona.rewrite,
    )

    content = f"## 質問\n{question}\n\n## 回答\n{answer}"
    return run_agent(agent, [{"role": "user", "content": content}], on_delta, cancel)


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
