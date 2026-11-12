// クイズ: 分野の読み込み、出題 (JSON)、採点、先読み、結果のメッセージ

#include "quiz_mode.h"

#include "agent.h"
#include "chat.h"
#include "config.h"
#include "llm_client.h"

#include <algorithm>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>

using nlohmann::ordered_json;

namespace
{

constexpr const char* QUIZ_CONFIG = "quiz.json";

// 自作の分野 (ユーザーごとの設定フォルダー。同梱の quiz.json とは分けて、更新で消えないようにする)
constexpr const char* CUSTOM_CONFIG = "quiz_custom.json";

// 1 問を作るのを試す回数
constexpr int MAX_ATTEMPTS = 3;

// 既出の問題として次の出題に渡す件数
constexpr size_t PREVIOUS_QUESTIONS = 10;

// ねぎらいに添える、間違えた問題の件数
constexpr size_t COMMENT_MISSES = 3;

// quiz.json に無いときの既定値
constexpr int DEFAULT_QUESTION_COUNT = 10;
constexpr int DEFAULT_MAX_TOKENS = 1024;
constexpr const char* DEFAULT_COMMENT_PERSONA = "assistant";

constexpr const char* DEFAULT_GENERATE_PROMPT =
    "あなたはクイズの出題者です。指定された分野とトピック (または資料) から、4 択の問題を 1 問だけ作ってください。"
    "correct には正解の選択肢、wrong には誤りの選択肢を 3 つ、explanation には正解の理由を書いてください。"
    "explanation で選択肢の番号や記号 (1・A など) は使わず、内容を言葉で書いてください。";

constexpr const char* DEFAULT_TOPICS_PROMPT =
    "あなたはクイズの出題者です。指定されたお題について、4 択の問題の題材になる小さなトピックを、"
    "重ならないように指定された個数だけ挙げてください。1 つは短い名詞句にします。";

using Results = std::vector<std::pair<float, std::string>>;

Results DefaultResults()
{
    return {
        {100.0f, "全問正解です。お見事でした！"},
        {80.0f, "すばらしい成績です。よくがんばりました！"},
        {50.0f, "いい調子です。間違えた問題を見直すと、さらに伸びます。"},
        {0.0f, "おつかれさまでした。間違えたところが伸びしろです。もう一度挑戦してみましょう。"},
    };
}

struct QuizSettings
{
    int questionCount = DEFAULT_QUESTION_COUNT;
    int maxTokens = DEFAULT_MAX_TOKENS;
    std::string commentPersona = DEFAULT_COMMENT_PERSONA;
    std::string generatePrompt = DEFAULT_GENERATE_PROMPT;

    // 自作の分野で、お題から出題のトピックを作らせるプロンプト
    std::string topicsPrompt = DEFAULT_TOPICS_PROMPT;

    // (正解率の下限 %, メッセージ)。下限の大きい順
    Results results = DefaultResults();
};

int PositiveInt(const json& config, const char* key, int fallback)
{
    const auto found = config.find(key);

    if (found == config.end() || !found->is_number_integer() || found->get<int>() <= 0)
    {
        return fallback;
    }

    return found->get<int>();
}

// 文字列か、文字列の配列 (改行でつなげて 1 つにする)
std::string PromptText(const json& config, const char* key, const char* fallback)
{
    const auto found = config.find(key);

    if (found == config.end())
    {
        return fallback;
    }

    if (found->is_string() && !found->get<std::string>().empty())
    {
        return found->get<std::string>();
    }

    if (found->is_array() && !found->empty())
    {
        std::string text;

        for (const auto& line : *found)
        {
            if (!line.is_string())
            {
                return fallback;
            }

            if (!text.empty())
            {
                text += "\n";
            }

            text += line.get<std::string>();
        }

        return text;
    }

    return fallback;
}

Results ReadResults(const json& config)
{
    const auto found = config.find("results");

    if (found == config.end() || !found->is_array())
    {
        return DefaultResults();
    }

    Results results;

    for (const auto& item : *found)
    {
        if (item.is_object() && item.contains("text") && item["text"].is_string() &&
            item.contains("min_percent") && item["min_percent"].is_number())
        {
            results.emplace_back(item["min_percent"].get<float>(), item["text"].get<std::string>());
        }
    }

    if (results.empty())
    {
        return DefaultResults();
    }

    std::sort(results.begin(), results.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    return results;
}

QuizSettings LoadQuizSettings()
{
    const json config = LoadConfigOrEmpty(QUIZ_CONFIG);

    QuizSettings settings;
    settings.questionCount = PositiveInt(config, "question_count", DEFAULT_QUESTION_COUNT);
    settings.maxTokens = PositiveInt(config, "max_tokens", DEFAULT_MAX_TOKENS);
    settings.commentPersona = ConfigStringOr(config, "comment_persona", DEFAULT_COMMENT_PERSONA);
    settings.generatePrompt = PromptText(config, "generate_prompt", DEFAULT_GENERATE_PROMPT);
    settings.topicsPrompt = PromptText(config, "topics_prompt", DEFAULT_TOPICS_PROMPT);
    settings.results = ReadResults(config);
    return settings;
}

bool IsCancelled(const std::exception& e)
{
    return std::string(e.what()) == "Cancelled";
}

std::string Trim(const std::string& text)
{
    const size_t first = text.find_first_not_of(" \t\r\n");

    if (first == std::string::npos)
    {
        return "";
    }

    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

// 問題の JSON の形 (llama-server が、この形に制限して出力する)
// 正解の番号ではなく、正解の文と誤りの文を別々に出させる
// (小さなモデルは番号を 1 始まりで答えたり、解説の番号とずれたりするため。並べ替えはアプリがする)
json QuestionSchema()
{
    return {
        {"type", "object"},
        {"properties",
         {{"question", {{"type", "string"}}},
          {"correct", {{"type", "string"}}},
          {"wrong",
           {{"type", "array"},
            {"items", {{"type", "string"}}},
            {"minItems", QUIZ_CHOICE_COUNT - 1},
            {"maxItems", QUIZ_CHOICE_COUNT - 1}}},
          {"explanation", {{"type", "string"}}}}},
        {"required", json::array({"question", "correct", "wrong", "explanation"})}};
}

// お題からトピックを作らせるときの JSON の形
json TopicsSchema()
{
    return {
        {"type", "object"},
        {"properties",
         {{"topics",
           {{"type", "array"}, {"items", {{"type", "string"}}}, {"minItems", 1}, {"maxItems", 20}}}}},
        {"required", json::array({"topics"})}};
}

// LLM が返した JSON を検証して、選択肢を並べ替えた問題にする
// 正しくなければ std::runtime_error
QuizQuestion ValidateQuestion(const json& raw, std::mt19937& random)
{
    const auto text = raw.find("question");
    const auto correct = raw.find("correct");
    const auto wrong = raw.find("wrong");

    if (text == raw.end() || !text->is_string() || Trim(text->get<std::string>()).empty())
    {
        throw std::runtime_error("問題文がありません");
    }

    if (correct == raw.end() || !correct->is_string() ||
        Trim(correct->get<std::string>()).empty() ||
        wrong == raw.end() || !wrong->is_array() ||
        wrong->size() != static_cast<size_t>(QUIZ_CHOICE_COUNT - 1))
    {
        throw std::runtime_error("選択肢が正しくありません");
    }

    std::vector<std::string> choices = {Trim(correct->get<std::string>())};

    for (const auto& item : *wrong)
    {
        if (!item.is_string() || Trim(item.get<std::string>()).empty())
        {
            throw std::runtime_error("選択肢が正しくありません");
        }

        choices.push_back(Trim(item.get<std::string>()));
    }

    if (std::set<std::string>(choices.begin(), choices.end()).size() !=
        static_cast<size_t>(QUIZ_CHOICE_COUNT))
    {
        throw std::runtime_error("選択肢が重複しています");
    }

    // 正解は先頭に置かれているので、並べ替えて位置を覚える
    std::vector<int> order(QUIZ_CHOICE_COUNT);

    for (int i = 0; i < QUIZ_CHOICE_COUNT; ++i)
    {
        order[i] = i;
    }

    std::shuffle(order.begin(), order.end(), random);

    QuizQuestion question;
    question.text = Trim(text->get<std::string>());

    for (int i = 0; i < QUIZ_CHOICE_COUNT; ++i)
    {
        question.choices.push_back(choices[order[i]]);

        if (order[i] == 0)
        {
            question.answer = i;
        }
    }

    const auto explanation = raw.find("explanation");

    if (explanation != raw.end() && explanation->is_string())
    {
        question.explanation = Trim(explanation->get<std::string>());
    }

    return question;
}

// 出題ごとの予定 (トピックと、資料のチャンク)
struct PlanItem
{
    std::string topic;
    std::optional<RagChunk> chunk;
};

// 自作の分野のお題から、出題のトピックを作らせる (偏りと重複を防ぐ)
// 作れなかったときは空 (既出の問題を渡す方式だけで進める)。中断されたら例外をそのまま投げる
std::vector<std::string> ExpandTopics(
    const QuizSettings& settings, const QuizCategory& category, int count,
    const std::atomic<bool>& cancel)
{
    Agent agent;
    agent.name = "トピック";
    agent.systemPrompt = settings.topicsPrompt;
    agent.options.maxTokens = settings.maxTokens;
    agent.options.temperature = 0.8f;
    agent.options.disableThinking = true;
    agent.options.schema = TopicsSchema();

    json raw;

    try
    {
        raw = RunAgentJson(
            agent,
            json::array({{{"role", "user"},
                          {"content", "お題: " + category.guide + "\n個数: " + std::to_string(count)}}}),
            cancel);
    }
    catch (const std::exception& e)
    {
        if (IsCancelled(e))
        {
            throw;
        }

        return {};
    }

    std::vector<std::string> topics;
    const auto found = raw.find("topics");

    if (found == raw.end() || !found->is_array())
    {
        return topics;
    }

    for (const auto& item : *found)
    {
        if (!item.is_string())
        {
            continue;
        }

        const std::string topic = Trim(item.get<std::string>());

        if (!topic.empty() && std::find(topics.begin(), topics.end(), topic) == topics.end())
        {
            topics.push_back(topic);
        }
    }

    if (topics.size() > static_cast<size_t>(count))
    {
        topics.resize(static_cast<size_t>(count));
    }

    return topics;
}

std::vector<PlanItem> MakePlan(
    const QuizCategory& category, RagState& rag, int count, const QuizSettings& settings,
    std::mt19937& random, const std::atomic<bool>& cancel)
{
    std::vector<PlanItem> plan;

    if (category.FromRag())
    {
        std::vector<RagChunk> chunks =
            category.guide.empty()
                ? SampleRagChunks(rag, static_cast<size_t>(count))
                : TopRagChunks(rag, category.guide, static_cast<size_t>(count), cancel);

        for (auto& chunk : chunks)
        {
            plan.push_back({"", std::move(chunk)});
        }

        if (plan.empty())
        {
            throw std::runtime_error(
                "資料 (rag/) のインデックスがありません。資料を置いて、読み込みが終わってから試してください");
        }

        return plan;
    }

    std::vector<std::string> topics;

    if (category.IsCustom())
    {
        topics = ExpandTopics(settings, category, count, cancel);
    }
    else
    {
        topics = category.topics;
        std::shuffle(topics.begin(), topics.end(), random);
    }

    for (int i = 0; i < count; ++i)
    {
        // トピックが足りなければ循環させる
        plan.push_back({topics.empty() ? "" : topics[static_cast<size_t>(i) % topics.size()], {}});
    }

    return plan;
}

std::string OriginOf(const RagChunk& chunk)
{
    return chunk.source + (chunk.heading.empty() ? "" : " / " + chunk.heading);
}

std::string BuildPrompt(
    const QuizCategory& category, const PlanItem& item, const std::vector<std::string>& previous)
{
    std::string prompt = "分野: " + category.name + "\n";

    if (item.chunk)
    {
        if (category.IsCustom() && !category.guide.empty())
        {
            prompt += "出題の方針: " + category.guide + "\n";
        }

        prompt += "資料 (" + OriginOf(*item.chunk) + "):\n" + item.chunk->text;
    }
    else
    {
        if (!category.guide.empty())
        {
            prompt += "出題範囲: " + category.guide + "\n";
        }

        if (!item.topic.empty())
        {
            prompt += "トピック: " + item.topic;
        }
    }

    if (!previous.empty())
    {
        prompt += "\n\nすでに出した問題 (これらと同じ内容は避けてください):";

        const size_t first =
            previous.size() > PREVIOUS_QUESTIONS ? previous.size() - PREVIOUS_QUESTIONS : 0;

        for (size_t i = first; i < previous.size(); ++i)
        {
            prompt += "\n- " + previous[i];
        }
    }

    return prompt;
}

QuizQuestion MakeQuestion(
    const QuizSettings& settings, const QuizCategory& category, const PlanItem& item,
    const std::vector<std::string>& previous, std::mt19937& random,
    const std::atomic<bool>& cancel)
{
    Agent agent;
    agent.name = "出題";
    agent.systemPrompt = settings.generatePrompt;
    agent.options.maxTokens = settings.maxTokens;
    agent.options.temperature = 0.8f;
    agent.options.disableThinking = true;
    agent.options.schema = QuestionSchema();

    const json raw = RunAgentJson(
        agent,
        json::array({{{"role", "user"}, {"content", BuildPrompt(category, item, previous)}}}),
        cancel);

    QuizQuestion question = ValidateQuestion(raw, random);
    question.topic = item.topic;

    if (item.chunk)
    {
        question.origin = OriginOf(*item.chunk);
    }

    return question;
}

// 問題を 1 問ずつ作り続ける (ユーザーが解いている間に先へ進む)
void GenerateAll(
    QuizState& state, int session, std::shared_ptr<std::atomic<bool>> cancel,
    QuizCategory category, RagState& rag, QuizSettings settings)
{
    std::string error;
    std::mt19937 random{std::random_device{}()};

    try
    {
        const std::vector<PlanItem> plan = MakePlan(category, rag, settings.questionCount, settings, random, *cancel);

        {
            std::lock_guard lock(state.mutex);

            if (state.session != session)
            {
                return;
            }

            // 資料が少ないときは、あるだけにする
            state.total = static_cast<int>(plan.size());
        }

        std::vector<std::string> previous;

        for (const auto& item : plan)
        {
            std::optional<QuizQuestion> question;

            for (int attempt = 0; attempt < MAX_ATTEMPTS && !question; ++attempt)
            {
                try
                {
                    question = MakeQuestion(settings, category, item, previous, random, *cancel);
                }
                catch (const std::exception& e)
                {
                    if (IsCancelled(e))
                    {
                        return;
                    }

                    error = e.what();
                }
            }

            {
                std::lock_guard lock(state.mutex);

                if (state.session != session)
                {
                    return;
                }

                if (!question)
                {
                    // 作れなかった問題は飛ばして、数を減らす
                    --state.total;
                }
                else
                {
                    previous.push_back(question->text);
                    state.questions.push_back(std::move(*question));
                }
            }

            WakeUI();
        }
    }
    catch (const std::exception& e)
    {
        if (IsCancelled(e))
        {
            return;
        }

        error = e.what();
    }

    {
        std::lock_guard lock(state.mutex);

        if (state.session != session)
        {
            return;
        }

        state.generating = false;

        if (state.questions.empty())
        {
            // 1 問も作れなかったら、分野の選択に戻す
            state.phase = QuizPhase::Select;
            state.error = error.empty() ? "問題を作れませんでした" : error;
        }
    }

    WakeUI();
}

// 分野の編集画面のテスト用に、1 問だけ作る (ワーカースレッドから呼ぶ)
QuizQuestion MakeTestQuestion(
    const QuizCategory& category, RagState& rag, const std::atomic<bool>& cancel)
{
    const QuizSettings settings = LoadQuizSettings();
    std::mt19937 random{std::random_device{}()};
    const PlanItem item = MakePlan(category, rag, 1, settings, random, cancel).front();
    std::string error = "問題を作れませんでした";

    for (int attempt = 0; attempt < MAX_ATTEMPTS; ++attempt)
    {
        try
        {
            return MakeQuestion(settings, category, item, {}, random, cancel);
        }
        catch (const std::exception& e)
        {
            if (IsCancelled(e))
            {
                throw;
            }

            error = e.what();
        }
    }

    throw std::runtime_error(error);
}

void RunTest(
    QuizState& state, int session, std::shared_ptr<std::atomic<bool>> cancel,
    QuizCategory category, RagState& rag)
{
    std::string text;

    try
    {
        const QuizQuestion question = MakeTestQuestion(category, rag, *cancel);
        text = "Q. " + question.text;

        for (int i = 0; i < QUIZ_CHOICE_COUNT; ++i)
        {
            text += std::string("\n") + (i == question.answer ? "○" : "　") + " " +
                    std::to_string(i + 1) + ". " + question.choices[i];
        }

        text += "\n解説: " + question.explanation;

        if (!question.topic.empty())
        {
            text += "\n(トピック: " + question.topic + ")";
        }

        if (!question.origin.empty())
        {
            text += "\n(出典: " + question.origin + ")";
        }
    }
    catch (const std::exception& e)
    {
        if (IsCancelled(e))
        {
            return;
        }

        text = std::string("作れませんでした: ") + e.what();
    }

    {
        std::lock_guard lock(state.mutex);

        if (state.testSession == session)
        {
            state.testResult = text;
            state.testRunning = false;
        }
    }

    WakeUI();
}

// 回答役の口調で、結果をねぎらうひと言を作って置き換える (失敗したら固定文のまま)
void MakeComment(
    QuizState& state, int session, std::shared_ptr<std::atomic<bool>> cancel,
    QuizSettings settings, std::string categoryName, int score, int total,
    std::vector<std::string> misses)
{
    std::string text;

    try
    {
        std::string persona;

        try
        {
            persona = CreateAnswerer(settings.commentPersona).systemPrompt;
        }
        catch (const std::exception&)
        {
            persona = "あなたは親切なAIアシスタントです。日本語で答えてください。";
        }

        Agent agent;
        agent.name = "ねぎらい";
        agent.systemPrompt = persona +
            "\n\nあなたはクイズの結果を伝える係です。次の結果を見て、受験者をねぎらうひと言を、"
            "日本語で 1〜2 文 (80 文字以内) で書いてください。"
            "点数を繰り返すだけにせず、前向きな言葉にしてください。";
        agent.options.maxTokens = 200;
        agent.options.temperature = 0.7f;
        agent.options.disableThinking = true;

        std::string content =
            "分野: " + categoryName + "\n点数: " + std::to_string(score) + " / " + std::to_string(total);

        if (!misses.empty())
        {
            content += "\n間違えた問題:";

            for (const auto& miss : misses)
            {
                content += "\n- " + miss;
            }
        }

        text = Trim(RunAgent(
            agent, json::array({{{"role", "user"}, {"content", content}}}), {}, *cancel));
    }
    catch (const std::exception&)
    {
        text.clear();
    }

    {
        std::lock_guard lock(state.mutex);

        if (state.session == session)
        {
            if (!text.empty())
            {
                state.comment = text;
            }

            state.commentPending = false;
        }
    }

    WakeUI();
}

// 結果にする (mutex をロックして呼ぶ)。固定のメッセージをすぐ出し、LLM のひと言を作る
void Finish(QuizState& state, const QuizSettings& settings)
{
    state.phase = QuizPhase::Result;
    const int total = static_cast<int>(state.answers.size());
    const int score = state.Score();
    state.comment = QuizTierMessage(settings.results, score, total);

    if (settings.commentPersona.empty())
    {
        return;
    }

    std::vector<std::string> misses;

    for (size_t i = 0; i < state.answers.size() && misses.size() < COMMENT_MISSES; ++i)
    {
        if (state.questions[i].answer != state.answers[i])
        {
            misses.push_back(state.questions[i].text);
        }
    }

    state.commentPending = true;

    if (state.commentWorker.joinable())
    {
        state.commentWorker.join();
    }

    state.commentWorker = std::thread(
        MakeComment, std::ref(state), state.session, state.cancel, settings,
        state.category.name, score, total, std::move(misses));
}

// 進行中のワーカーを止めて、終わるのを待つ (mutex は持たずに呼ぶ)
void StopWorkers(QuizState& state)
{
    {
        std::lock_guard lock(state.mutex);
        *state.cancel = true;
    }

    if (state.worker.joinable())
    {
        state.worker.join();
    }

    if (state.commentWorker.joinable())
    {
        state.commentWorker.join();
    }
}

} // namespace

int QuizState::Score() const
{
    int score = 0;

    for (size_t i = 0; i < answers.size() && i < questions.size(); ++i)
    {
        if (questions[i].answer == answers[i])
        {
            ++score;
        }
    }

    return score;
}

std::vector<QuizCategory> LoadQuizCategories()
{
    std::vector<QuizCategory> categories;
    const json config = LoadConfigOrEmpty(QUIZ_CONFIG);
    const auto entries = config.find("categories");

    if (entries == config.end() || !entries->is_array())
    {
        return categories;
    }

    for (const auto& entry : *entries)
    {
        if (!entry.is_object() || !entry.contains("key") || !entry["key"].is_string() ||
            !entry.contains("name") || !entry["name"].is_string() ||
            !entry.contains("source") || !entry["source"].is_string())
        {
            continue;
        }

        const std::string source = entry["source"].get<std::string>();

        if (source != "llm" && source != "rag")
        {
            continue;
        }

        QuizCategory category;
        category.key = entry["key"].get<std::string>();
        category.name = entry["name"].get<std::string>();
        category.source = source;
        category.guide = ConfigStringOr(entry, "guide", "");

        if (entry.contains("topics") && entry["topics"].is_array())
        {
            for (const auto& topic : entry["topics"])
            {
                if (topic.is_string() && !topic.get<std::string>().empty())
                {
                    category.topics.push_back(topic.get<std::string>());
                }
            }
        }

        categories.push_back(std::move(category));
    }

    return categories;
}

std::vector<QuizCategory> LoadCustomQuizCategories()
{
    const ordered_json config = LoadConfigForEdit(CUSTOM_CONFIG);
    std::vector<QuizCategory> categories;
    const auto entries = config.find("categories");

    if (entries == config.end() || !entries->is_array())
    {
        return categories;
    }

    for (const auto& entry : *entries)
    {
        if (!entry.is_object() || !entry.contains("name") || !entry["name"].is_string())
        {
            continue;
        }

        const std::string name = Trim(entry["name"].get<std::string>());

        if (name.empty())
        {
            continue;
        }

        QuizCategory category;
        category.key = "custom" + std::to_string(categories.size());
        category.name = name.substr(0, std::min(name.size(), QUIZ_MAX_NAME_BYTES));
        category.source = "custom";

        if (entry.contains("prompt") && entry["prompt"].is_string())
        {
            const std::string prompt = Trim(entry["prompt"].get<std::string>());
            category.guide = prompt.substr(0, std::min(prompt.size(), QUIZ_MAX_PROMPT_BYTES));
        }

        category.useRag =
            entry.contains("use_rag") && entry["use_rag"].is_boolean() && entry["use_rag"].get<bool>();
        categories.push_back(std::move(category));
    }

    return categories;
}

void SaveCustomQuizCategories(const std::vector<QuizCategory>& categories)
{
    ordered_json entries = ordered_json::array();

    for (const auto& category : categories)
    {
        entries.push_back({{"name", category.name}, {"prompt", category.guide}, {"use_rag", category.useRag}});
    }

    SaveConfig(CUSTOM_CONFIG, {{"categories", entries}});
}

void StartQuizTest(QuizState& state, const QuizCategory& category, RagState& rag)
{
    StopQuizTest(state);

    int session;
    std::shared_ptr<std::atomic<bool>> cancel;
    {
        std::lock_guard lock(state.mutex);
        session = ++state.testSession;
        cancel = state.testCancel = std::make_shared<std::atomic<bool>>(false);
        state.testRunning = true;
        state.testResult.clear();
    }

    state.testWorker = std::thread(RunTest, std::ref(state), session, cancel, category, std::ref(rag));
    WakeUI();
}

void StopQuizTest(QuizState& state)
{
    {
        std::lock_guard lock(state.mutex);
        *state.testCancel = true;
        ++state.testSession;
        state.testRunning = false;
        state.testResult.clear();
    }

    if (state.testWorker.joinable())
    {
        state.testWorker.join();
    }
}

std::string QuizTierMessage(const Results& results, int score, int total)
{
    const float percent = total > 0 ? 100.0f * static_cast<float>(score) / static_cast<float>(total) : 0.0f;

    for (const auto& [minimum, text] : results)
    {
        if (percent >= minimum)
        {
            return text;
        }
    }

    return results.empty() ? std::string() : results.back().second;
}

void StartQuiz(QuizState& state, const QuizCategory& category, RagState& rag)
{
    const QuizSettings settings = LoadQuizSettings();

    StopWorkers(state);

    int session;
    {
        std::lock_guard lock(state.mutex);
        session = ++state.session;
        state.cancel = std::make_shared<std::atomic<bool>>(false);
        state.phase = QuizPhase::Playing;
        state.category = category;
        state.total = settings.questionCount;
        state.questions.clear();
        state.answers.clear();
        state.index = 0;
        state.answered = false;
        state.generating = true;
        state.error.clear();
        state.comment.clear();
        state.commentPending = false;
    }

    state.worker = std::thread(
        GenerateAll, std::ref(state), session, state.cancel, category, std::ref(rag), settings);
    WakeUI();
}

void AnswerQuiz(QuizState& state, int choice)
{
    {
        std::lock_guard lock(state.mutex);

        if (state.phase != QuizPhase::Playing || state.answered ||
            state.index >= static_cast<int>(state.questions.size()) ||
            choice < 0 || choice >= QUIZ_CHOICE_COUNT)
        {
            return;
        }

        state.answers.push_back(choice);
        state.answered = true;
    }

    WakeUI();
}

void NextQuestion(QuizState& state)
{
    const QuizSettings settings = LoadQuizSettings();

    {
        std::lock_guard lock(state.mutex);

        if (state.phase != QuizPhase::Playing || !state.answered)
        {
            return;
        }

        if (state.IsLast())
        {
            Finish(state, settings);
        }
        else if (state.index + 1 < static_cast<int>(state.questions.size()))
        {
            ++state.index;
            state.answered = false;
        }
    }

    WakeUI();
}

void QuitQuiz(QuizState& state)
{
    StopWorkers(state);

    {
        std::lock_guard lock(state.mutex);
        ++state.session;
        state.cancel = std::make_shared<std::atomic<bool>>(false);
        state.phase = QuizPhase::Select;
        state.generating = false;
        state.commentPending = false;
        state.error.clear();
    }

    WakeUI();
}

void ShutdownQuiz(QuizState& state)
{
    StopWorkers(state);
    StopQuizTest(state);
}
