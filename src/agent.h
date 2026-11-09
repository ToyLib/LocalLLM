#pragma once

#include "llm_client.h"

#include <atomic>
#include <string>
#include <vector>

// 役割 (system プロンプト) と設定を持つエージェント
// 同じ LLM サーバーを、役割を変えて呼び分ける
struct Agent
{
    // config/agents.json のキー (assistant / general など)
    std::string key;

    std::string name;
    std::string systemPrompt;
    LLMOptions options;

    // ChatGPT に聞き直した回答を、この回答役の口調で書き直すか (agents.json の rewrite)
    bool rewrite = true;
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

// 資料を参照する質問に、資料に基づいて普通の口調で答える
Agent CreateAssistant();

// 資料の要らない質問 (雑談や一般的な質問) に答える
Agent CreateGeneral();

// key (assistant / general) の回答役
Agent CreateAnswerer(const std::string& key);

// ChatGPT (server.json の cloud) に聞き直すエージェント
// webSearch なら Web 検索用のモデルで答えさせる
Agent CreateCloud(bool webSearch = false);

// クラウドの回答 answer を、回答役 persona の口調で書き直す (ローカルの LLM で)
std::string RewriteAnswer(
    const Agent& persona,
    const std::string& question,
    const std::string& answer,
    const DeltaCallback& onDelta,
    const std::atomic<bool>& cancel);

// ローカルの回答と聞き直した回答の違いのまとめ (ローカルの LLM が作る目安)
struct Comparison
{
    // ほぼ同じ / 情報が増えた / 食い違いあり
    std::string verdict;

    // 違いの要点 (最大3つ)
    std::vector<std::string> points;
};

// 同じ質問へのローカルの回答 localAnswer と、クラウドの回答 cloudAnswer の違いを
// ローカルの LLM でまとめる
// webSearch なら、Web 検索なしの回答 localAnswer と Web 検索ありの回答 cloudAnswer を比べる
// onDelta には受け取った JSON の断片が来る (進み具合の表示に使う)
Comparison CompareAnswers(
    const std::string& question,
    const std::string& localAnswer,
    const std::string& cloudAnswer,
    const DeltaCallback& onDelta,
    const std::atomic<bool>& cancel,
    bool webSearch = false);

// agents.json に key が無いときに使う名前と system プロンプト (cloud / rewriter / comparer / web_comparer のみ。ほかは空)
std::string DefaultAgentName(const std::string& key);
std::string DefaultAgentPrompt(const std::string& key);

// ルーターの判断
struct RouteDecision
{
    bool useRag = false;
    std::string reason;
};

// 質問に rag/ の資料が必要かをルーターに判断させる
// history は直前までの会話 (指示語を含む質問の判断に使う)
// sources は rag/ にある資料の説明 (DescribeRagSources の結果)
RouteDecision RouteQuestion(
    const json& history,
    const std::string& question,
    const std::vector<std::string>& sources,
    const std::atomic<bool>& cancel);
