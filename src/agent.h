#pragma once

#include "llm_client.h"

#include <atomic>
#include <string>
#include <vector>

// 役割 (system プロンプト) と設定を持つエージェント
// 同じ LLM サーバーを、役割を変えて呼び分ける
struct Agent
{
    std::string name;
    std::string systemPrompt;
    LLMOptions options;
};

// エージェントに会話 (system を除いた user / assistant) を渡して回答させる
// cancel が true になると中断して std::runtime_error を投げる
std::string RunAgent(
    const Agent& agent,
    const json& messages,
    const DeltaCallback& onDelta,
    const std::atomic<bool>& cancel);

// エージェントに JSON で答えさせる (agent.options.schema が必要)
json RunAgentJson(
    const Agent& agent,
    const json& messages,
    const std::atomic<bool>& cancel);

// ルーターの判断
struct RouteDecision
{
    bool useRag = false;
    std::string reason;
};

// 質問に rag/ の資料が必要かをルーターに判断させる
// history は直前までの会話 (指示語を含む質問の判断に使う)
// sources は rag/ にある資料の名前
RouteDecision RouteQuestion(
    const json& history,
    const std::string& question,
    const std::vector<std::string>& sources,
    const std::atomic<bool>& cancel);
