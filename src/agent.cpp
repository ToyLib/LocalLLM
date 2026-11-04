#include "agent.h"

#include "config.h"

#include <stdexcept>

namespace
{

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
    agent.name = entry.value("name", key);

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

Agent CreateAssistant()
{
    return LoadAgent("assistant");
}

Agent CreateGeneral()
{
    return LoadAgent("general");
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
