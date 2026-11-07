#include "agent.h"

#include "config.h"

#include <stdexcept>

namespace
{

// agents.json に cloud / rewriter が無いとき (前の版の設定ファイル) に使う system プロンプト
constexpr const char* DEFAULT_CLOUD_PROMPT =
    "あなたは親切で正確なAIアシスタントです。日本語で、分かりやすく回答してください。";

constexpr const char* DEFAULT_REWRITER_PROMPT =
    "これから渡す「回答」を、あなたの口調で言い直してください。"
    "内容・数値・手順・固有名詞・コードは変えず、足したり削ったりしないでください。"
    "コードやコマンドはそのまま書き写します。"
    "言い直した回答だけを書き、前置きや説明は付けないでください。";

// 書き直しの最大トークン数 (ChatGPT の長い回答も入るように)
constexpr int REWRITE_MAX_TOKENS = 4096;

// ルーターに見せる直前の会話の数と、1件あたりの長さ (バイト)
constexpr size_t ROUTER_HISTORY_MESSAGES = 4;
constexpr size_t ROUTER_HISTORY_BYTES = 300;

// config/agents.json から key のエージェントの名前と system プロンプトを読む
// 質問のたびに読み直すので、ファイルを書き換えればアプリを再起動しなくても反映される
// system_prompt は文字列か、文字列の配列 (つなげて1つにする)
Agent LoadAgent(const std::string& key)
{
    const json config = LoadConfig("agents.json");

    if (!config.contains(key) || !config[key].is_object())
    {
        throw std::runtime_error(
            "設定ファイル agents.json に " + key + " がありません");
    }

    const json& entry = config[key];

    Agent agent;
    agent.key = key;
    agent.name = entry.value("name", key);
    agent.rewrite = entry.value("rewrite", key != "assistant");

    const json prompt = entry.value("system_prompt", json());

    if (prompt.is_string())
    {
        agent.systemPrompt = prompt.get<std::string>();
    }
    else if (prompt.is_array())
    {
        for (const auto& line : prompt)
        {
            if (!line.is_string())
            {
                throw std::runtime_error(
                    key + " の system_prompt に文字列以外が含まれています");
            }

            agent.systemPrompt += line.get<std::string>();
        }
    }
    else
    {
        throw std::runtime_error(key + " の system_prompt がありません");
    }

    return agent;
}

// key のエージェントを読む。agents.json に無ければ name と prompt を使う
// (設定ファイルが前の版のままでも動くように)
Agent LoadAgentOr(
    const std::string& key, const std::string& name, const std::string& prompt)
{
    const json config = LoadConfig("agents.json");

    if (config.contains(key))
    {
        return LoadAgent(key);
    }

    Agent agent;
    agent.key = key;
    agent.name = name;
    agent.systemPrompt = prompt;
    return agent;
}

// UTF-8 の文字の途中で切らないように、先頭 maxBytes までに縮める
std::string Truncate(const std::string& text, size_t maxBytes)
{
    if (text.size() <= maxBytes)
    {
        return text;
    }

    size_t end = maxBytes;

    // 続きのバイト (10xxxxxx) の途中なら、文字の先頭まで戻る
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)
    {
        --end;
    }

    return text.substr(0, end) + "…";
}

Agent CreateRouter()
{
    Agent agent = LoadAgent("router");

    // 判断だけなので短く、毎回同じ答えになるように
    agent.options.maxTokens = 128;
    agent.options.temperature = 0.0f;
    agent.options.disableThinking = true;

    // 先に理由を書かせてから結論を出させる
    agent.options.schema = {
        {"type", "object"},
        {"properties", {
            {"reason", {{"type", "string"}}},
            {"use_rag", {{"type", "boolean"}}}
        }},
        {"required", {"reason", "use_rag"}}
    };

    return agent;
}

} // namespace

std::string DefaultAgentName(const std::string& key)
{
    if (key == "cloud")
    {
        return "ChatGPT";
    }

    return key == "rewriter" ? "書き直し" : "";
}

std::string DefaultAgentPrompt(const std::string& key)
{
    if (key == "cloud")
    {
        return DEFAULT_CLOUD_PROMPT;
    }

    return key == "rewriter" ? DEFAULT_REWRITER_PROMPT : "";
}

Agent CreateAssistant()
{
    return LoadAgent("assistant");
}

Agent CreateGeneral()
{
    return LoadAgent("general");
}

Agent CreateAnswerer(const std::string& key)
{
    if (key != "assistant" && key != "general")
    {
        throw std::runtime_error("回答役ではありません: " + key);
    }

    return LoadAgent(key);
}

Agent CreateCloud()
{
    const CloudSettings cloud = LoadCloudSettings();
    Agent agent = LoadAgentOr("cloud", cloud.name, DefaultAgentPrompt("cloud"));

    // 名前は server.json の cloud.name に合わせる
    agent.name = cloud.name;
    agent.options.cloud = true;
    return agent;
}

std::string RewriteAnswer(
    const Agent& persona,
    const std::string& question,
    const std::string& answer,
    const DeltaCallback& onDelta,
    const std::atomic<bool>& cancel)
{
    const Agent rewriter = LoadAgentOr(
        "rewriter", DefaultAgentName("rewriter"), DefaultAgentPrompt("rewriter"));

    Agent agent = persona;
    agent.systemPrompt += "\n\n" + rewriter.systemPrompt;
    agent.options.maxTokens = REWRITE_MAX_TOKENS;
    agent.options.disableThinking = true;
    agent.options.cloud = false;

    const std::string content =
        "## 質問\n" + question + "\n\n## 回答\n" + answer;

    return RunAgent(
        agent,
        json::array({{{"role", "user"}, {"content", content}}}),
        onDelta,
        cancel);
}

std::string RunAgent(
    const Agent& agent,
    const json& messages,
    const DeltaCallback& onDelta,
    const std::atomic<bool>& cancel)
{
    json request = json::array({
        {
            {"role", "system"},
            {"content", agent.systemPrompt}
        }
    });

    for (const auto& message : messages)
    {
        request.push_back(message);
    }

    return AskLLM(request, onDelta, cancel, agent.options);
}

json RunAgentJson(
    const Agent& agent,
    const json& messages,
    const std::atomic<bool>& cancel)
{
    const std::string answer = RunAgent(agent, messages, {}, cancel);

    json result = json::parse(answer, nullptr, false);

    if (result.is_discarded() || !result.is_object())
    {
        throw std::runtime_error(
            agent.name + "の回答を JSON として読めませんでした");
    }

    return result;
}

RouteDecision RouteQuestion(
    const json& history,
    const std::string& question,
    const std::vector<std::string>& sources,
    const std::atomic<bool>& cancel)
{
    std::string content = "## 資料の一覧\n";

    for (const auto& source : sources)
    {
        content += "- " + source + "\n";
    }

    if (!history.empty())
    {
        content += "\n## 直前の会話\n";

        const size_t first = history.size() > ROUTER_HISTORY_MESSAGES
            ? history.size() - ROUTER_HISTORY_MESSAGES
            : 0;

        for (size_t i = first; i < history.size(); ++i)
        {
            const auto& message = history[i];
            const std::string role =
                message.value("role", "") == "user" ? "ユーザー" : "AI";

            content += role + ": " +
                Truncate(message.value("content", ""), ROUTER_HISTORY_BYTES) +
                "\n";
        }
    }

    content += "\n## 質問\n" + question;

    const json result = RunAgentJson(
        CreateRouter(),
        json::array({{{"role", "user"}, {"content", content}}}),
        cancel);

    if (!result.contains("use_rag") || !result["use_rag"].is_boolean())
    {
        throw std::runtime_error("ルーターの回答に use_rag がありません");
    }

    RouteDecision decision;
    decision.useRag = result["use_rag"].get<bool>();
    decision.reason = result.value("reason", "");

    return decision;
}
