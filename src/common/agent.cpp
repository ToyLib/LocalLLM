// 役割ごとの LLM の呼び出し (ルーター / 回答役 / 聞き直し / 書き直し / 違いのまとめ)

#include "common/agent.h"

#include "common/config.h"

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

constexpr const char* DEFAULT_COMPARER_PROMPT =
    "同じ質問への「ローカルの回答」と「聞き直した回答」を比べて、内容の違いを短くまとめてください。"
    "口調・言い回し・長さ・書き方の違いは書かず、内容の違いだけを書きます。"
    "points には違いを最大3つ、1つ40文字くらいで書きます。"
    "食い違いは「ローカルは〜、聞き直した回答は〜」、片方にだけある情報は「聞き直した回答だけ: 〜」のように、どちらの回答の話かが分かるように書きます。"
    "聞き直した回答にだけある情報、数値や事実の食い違いを優先します。"
    "違いが無ければ「内容の違いはありません」とだけ書きます。"
    "回答に書いていないことを推測で書かないでください。"
    "verdict は、食い違う点が1つでもあれば「食い違いあり」、"
    "食い違いは無く、聞き直した回答のほうが情報が多ければ「情報が増えた」、"
    "内容がほぼ同じなら「ほぼ同じ」にします。";

constexpr const char* DEFAULT_WEB_COMPARER_PROMPT =
    "同じ質問への「Web 検索なしの回答」と「Web 検索ありの回答」を比べて、内容の違いを短くまとめてください。"
    "口調・言い回し・長さ・書き方の違いは書かず、内容の違いだけを書きます。[1] のような出典の番号は無視します。"
    "points には違いを最大3つ、1つ40文字くらいで書きます。"
    "食い違いは「検索なしは〜、検索ありは〜」、片方にだけある情報は「検索ありだけ: 〜」のように、どちらの回答の話かが分かるように書きます。"
    "検索ありの回答にだけある情報、数値や事実の食い違いを優先します。"
    "違いが無ければ「内容の違いはありません」とだけ書きます。"
    "回答に書いていないことを推測で書かないでください。"
    "verdict は、食い違う点が1つでもあれば「食い違いあり」、"
    "食い違いは無く、検索ありの回答のほうが情報が多ければ「情報が増えた」、"
    "内容がほぼ同じなら「ほぼ同じ」にします。";

// 違いのまとめの判定
constexpr const char* VERDICTS[] = {"ほぼ同じ", "情報が増えた", "食い違いあり"};

// 違いのまとめで、1つの回答を見せる最大の長さ (バイト。日本語で 1,200 文字くらい)
// llama-server のコンテキスト (既定 4096 トークン) に2つの回答が収まるように
constexpr size_t COMPARE_ANSWER_BYTES = 3600;
constexpr size_t COMPARE_QUESTION_BYTES = 600;

// 書き直しの最大トークン数 (ChatGPT の長い回答も入るように)
constexpr int REWRITE_MAX_TOKENS = 4096;

// ルーターに見せる直前の会話の数と、1件あたりの長さ (バイト)
constexpr size_t ROUTER_HISTORY_MESSAGES = 4;
constexpr size_t ROUTER_HISTORY_BYTES = 300;

// config/agents.json から key のエージェントの名前と system プロンプトを読む
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

// key のエージェントを読む。agents.json に無ければ name と prompt を使う (前の版の設定ファイル向け)
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

// 改行やタブを空白にして、続く空白を1つにまとめ、前後の空白を取る
// (違いのまとめの要点の途中に改行が入ることがあるので)
std::string ToSingleLine(const std::string& text)
{
    std::string line;
    bool space = false;

    for (const char c : text)
    {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
        {
            space = !line.empty();
            continue;
        }

        if (space)
        {
            line += ' ';
            space = false;
        }

        line += c;
    }

    return line;
}

Agent CreateRouter()
{
    Agent agent = LoadAgent("router");

    // 判断だけなので短く、毎回同じ答えになるように
    // (gpt-oss は推論のトークンもここに含まれるので、少し余裕を持たせる)
    agent.options.maxTokens = 256;
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

    if (key == "comparer")
    {
        return "違いのまとめ";
    }

    if (key == "web_comparer")
    {
        return "Web 検索の違いのまとめ";
    }

    return key == "rewriter" ? "書き直し" : "";
}

std::string DefaultAgentPrompt(const std::string& key)
{
    if (key == "cloud")
    {
        return DEFAULT_CLOUD_PROMPT;
    }

    if (key == "comparer")
    {
        return DEFAULT_COMPARER_PROMPT;
    }

    if (key == "web_comparer")
    {
        return DEFAULT_WEB_COMPARER_PROMPT;
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

Agent CreateCloud(bool webSearch)
{
    const CloudSettings cloud = LoadCloudSettings();
    Agent agent = LoadAgentOr("cloud", cloud.name, DefaultAgentPrompt("cloud"));

    // 名前は server.json の cloud.name に合わせる
    agent.name = cloud.name;
    agent.options.cloud = true;
    agent.options.webSearch = webSearch;
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

Comparison CompareAnswers(
    const std::string& question,
    const std::string& localAnswer,
    const std::string& cloudAnswer,
    const DeltaCallback& onDelta,
    const std::atomic<bool>& cancel,
    bool webSearch)
{
    const std::string key = webSearch ? "web_comparer" : "comparer";
    Agent agent = LoadAgentOr(key, DefaultAgentName(key), DefaultAgentPrompt(key));

    agent.options.maxTokens = 512;
    agent.options.temperature = 0.0f;
    agent.options.disableThinking = true;
    agent.options.cloud = false;

    // 先に違いを書かせてから判定させる
    agent.options.schema = {
        {"type", "object"},
        {"properties", {
            {"points", {
                {"type", "array"},
                {"items", {{"type", "string"}, {"minLength", 1}}},
                // 空の配列だけ返すことがあるので、少なくとも1つは書かせる
                {"minItems", 1},
                {"maxItems", 3}
            }},
            {"verdict", {{"type", "string"}, {"enum", VERDICTS}}}
        }},
        {"required", {"points", "verdict"}}
    };

    const std::string before = webSearch ? "Web 検索なしの回答" : "ローカルの回答";
    const std::string after = webSearch ? "Web 検索ありの回答" : "聞き直した回答";

    const std::string content =
        "## 質問\n" + Truncate(question, COMPARE_QUESTION_BYTES) +
        "\n\n## " + before + "\n" + Truncate(localAnswer, COMPARE_ANSWER_BYTES) +
        "\n\n## " + after + "\n" + Truncate(cloudAnswer, COMPARE_ANSWER_BYTES);

    const std::string answer = RunAgent(
        agent,
        json::array({{{"role", "user"}, {"content", content}}}),
        onDelta,
        cancel);

    const json result = json::parse(answer, nullptr, false);

    if (result.is_discarded() || !result.is_object() ||
        !result.contains("verdict") || !result["verdict"].is_string())
    {
        throw std::runtime_error("違いのまとめを JSON として読めませんでした");
    }

    Comparison comparison;
    comparison.verdict = result["verdict"].get<std::string>();

    if (result.contains("points") && result["points"].is_array())
    {
        for (const auto& point : result["points"])
        {
            if (!point.is_string())
            {
                continue;
            }

            const std::string line = ToSingleLine(point.get<std::string>());

            if (!line.empty())
            {
                comparison.points.push_back(line);
            }
        }
    }

    return comparison;
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
