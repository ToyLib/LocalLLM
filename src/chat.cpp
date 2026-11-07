#include "chat.h"

#include "agent.h"
#include "llm_client.h"
#include "rag.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <optional>
#include <stdexcept>

namespace
{

// ワーカースレッドからUIスレッドを起こすためのイベント
Uint32 g_wakeEvent = 0;

// 参照した資料の一覧 (回答の下に表示する)
std::string FormatSources(const std::vector<RagHit>& hits)
{
    if (hits.empty())
    {
        return "参照: 関係のある資料は見つかりませんでした";
    }

    std::string note = "参照:";

    for (size_t i = 0; i < hits.size(); ++i)
    {
        char score[16];
        std::snprintf(score, sizeof(score), "%.2f", hits[i].score);

        note += "\n  [" + std::to_string(i + 1) + "] " + hits[i].source;

        if (!hits[i].heading.empty())
        {
            note += " (" + hits[i].heading + ")";
        }

        note += std::string(" ") + score;
    }

    return note;
}

// ルーターに資料が必要かを判断させ、その結果を note に書く
// 判断できなかったときは資料を参照する (ルーター導入前と同じ動き)
bool DecideRag(
    RagState& rag,
    const json& history,
    const std::string& prompt,
    const std::atomic<bool>& cancel,
    std::string& note)
{
    const auto sources = DescribeRagSources(rag);

    if (sources.empty())
    {
        note = "ルーター: 資料が無いので参照しません";
        return false;
    }

    // 送信する履歴から今回の質問を除いたもの
    json previous = json::array();

    for (size_t i = 0; i + 1 < history.size(); ++i)
    {
        previous.push_back(history[i]);
    }

    try
    {
        const auto decision = RouteQuestion(previous, prompt, sources, cancel);

        note = std::string("ルーター: ") +
            (decision.useRag ? "資料を参照します" : "資料は参照しません");

        if (!decision.reason.empty())
        {
            note += " (" + decision.reason + ")";
        }

        return decision.useRag;
    }
    catch (const std::exception& e)
    {
        if (cancel)
        {
            throw;
        }

        note = std::string("ルーター: 判断できなかったので資料を参照します: ") +
            e.what();
        return true;
    }
}

} // namespace

void RegisterWakeEvent()
{
    g_wakeEvent = SDL_RegisterEvents(1);
}

void WakeUI()
{
    SDL_Event event{};
    event.type = g_wakeEvent;
    SDL_PushEvent(&event);
}

void JoinWorker(ChatState& state)
{
    if (state.worker.joinable())
    {
        state.worker.join();
    }
}

void SendPrompt(ChatState& state, RagState& rag, const std::string& prompt)
{
    JoinWorker(state);

    // system プロンプトは回答役のエージェントが付ける
    json history = json::array();

    {
        std::lock_guard lock(state.mutex);

        state.messages.push_back({"user", prompt});

        // 履歴全体を送信 (ChatGPT に聞き直して置き換えた回答は除く)
        for (const auto& message : state.messages)
        {
            if (message.superseded)
            {
                continue;
            }

            history.push_back({
                {"role", message.role},
                {"content", message.content}
            });
        }

        // ストリーミングで埋めていくAIの回答
        state.messages.push_back({"assistant", ""});

        state.busy = true;
        state.error.clear();
    }

    state.cancel = false;

    const RagMode ragMode = state.ragMode;

    state.worker = std::thread([&state, &rag, history, prompt, ragMode]() mutable {
        try
        {
            std::string note;
            bool useRag = ragMode == RagMode::Always;

            if (ragMode == RagMode::Auto)
            {
                useRag = DecideRag(rag, history, prompt, state.cancel, note);
            }

            // 資料を使う質問はアシスタント、使わない質問は汎用の回答役が答える
            const Agent answerer = useRag ? CreateAssistant() : CreateGeneral();

            {
                std::lock_guard lock(state.mutex);
                state.messages.back().agent = answerer.name;
                state.messages.back().agentKey = answerer.key;
                state.messages.back().note = note;
            }
            WakeUI();

            if (useRag)
            {
                // 資料は送信する質問にだけ添え、画面の履歴には残さない
                // (ルーターの判断の下に、参照した資料を続ける)
                if (!note.empty())
                {
                    note += "\n";
                }

                try
                {
                    const auto hits = SearchRag(rag, prompt, state.cancel);

                    if (!hits.empty())
                    {
                        std::string ragPrompt = BuildRagPrompt(hits, prompt);
                        history.back()["content"] = ragPrompt;

                        // ChatGPT に聞き直すときに同じ資料を送れるように、
                        // また資料を使ったやりとりを ChatGPT 宛ての履歴から除けるように残す
                        std::lock_guard lock(state.mutex);
                        auto& messages = state.messages;
                        messages[messages.size() - 2].ragPrompt = std::move(ragPrompt);
                        messages.back().usedRag = true;
                    }

                    note += FormatSources(hits);
                }
                catch (const std::exception& e)
                {
                    if (state.cancel)
                    {
                        throw;
                    }

                    // 検索できなくても、資料なしで回答は続ける
                    note += std::string("資料を参照できませんでした: ") +
                        e.what();
                }

                {
                    std::lock_guard lock(state.mutex);
                    state.messages.back().note = std::move(note);
                }
                WakeUI();
            }

            RunAgent(
                answerer,
                history,
                [&state](std::string_view text) {
                    {
                        std::lock_guard lock(state.mutex);
                        state.messages.back().content += text;
                    }
                    WakeUI();
                },
                state.cancel);
        }
        catch (const std::exception& e)
        {
            std::lock_guard lock(state.mutex);

            const bool keepPartial =
                state.cancel && !state.messages.back().content.empty();

            if (!keepPartial)
            {
                // 失敗した質問は履歴から取り除き、入力欄に戻す
                state.messages.pop_back();
                state.messages.pop_back();
                state.restoredPrompt = prompt;
            }

            if (!state.cancel)
            {
                state.error = e.what();
            }
        }

        {
            std::lock_guard lock(state.mutex);
            state.busy = false;
        }
        WakeUI();
    });
}

namespace
{

// 最新の質問の位置 (無ければ nullopt)
std::optional<size_t> LastUserIndex(const std::vector<Message>& messages)
{
    for (size_t i = messages.size(); i > 0; --i)
    {
        if (messages[i - 1].role == "user")
        {
            return i - 1;
        }
    }

    return std::nullopt;
}

// UTF-8 の文字数 (続きのバイト 10xxxxxx は数えない)
size_t CountChars(std::string_view text)
{
    size_t count = 0;

    for (const char c : text)
    {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80)
        {
            ++count;
        }
    }

    return count;
}

// 1234 → "1,234"
std::string FormatCount(size_t value)
{
    std::string digits = std::to_string(value);

    for (int i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3)
    {
        digits.insert(static_cast<size_t>(i), ",");
    }

    return digits;
}

// 途中の状態を変える (呼ぶ側で state.mutex をロックしておく)
void SetStatus(Message& message, std::string status, size_t totalChars = 0)
{
    message.status = std::move(status);
    message.statusSince = std::chrono::steady_clock::now();
    message.receivedChars = 0;
    message.totalChars = totalChars;
}

// この秒数を過ぎたら、中断できることを添える
constexpr long long SLOW_SECONDS = 20;

// ChatGPT に送る履歴を作る (呼ぶ側で state.mutex をロックしておく)
// 最新の質問より前のやりとりのうち、資料を使っていないものだけを送る
json BuildCloudHistory(const std::vector<Message>& messages, size_t lastUser)
{
    json history = json::array();

    for (size_t i = 0; i < lastUser; ++i)
    {
        if (messages[i].role != "user")
        {
            continue;
        }

        // この質問への今の回答 (聞き直して置き換えたものは除く)
        const Message* answer = nullptr;

        for (size_t j = i + 1; j < lastUser && messages[j].role != "user"; ++j)
        {
            if (!messages[j].superseded)
            {
                answer = &messages[j];
            }
        }

        if (!answer || answer->usedRag || answer->content.empty())
        {
            continue;
        }

        history.push_back({{"role", "user"}, {"content", messages[i].content}});
        history.push_back({{"role", "assistant"}, {"content", answer->content}});
    }

    return history;
}

bool IsBlank(const std::string& text)
{
    return text.find_first_not_of(" \t\r\n") == std::string::npos;
}

} // namespace

std::string FormatStatus(const Message& message)
{
    if (message.status.empty())
    {
        return {};
    }

    const long long seconds = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - message.statusSince).count();

    std::string text = message.status;

    if (message.receivedChars > 0 || message.totalChars > 0)
    {
        text += " " + FormatCount(message.receivedChars) + " 文字";

        // 書き直すと長さが変わるので、元の長さは目安として添える
        if (message.totalChars > 0)
        {
            text += " (元の回答は " + FormatCount(message.totalChars) + " 文字)";
        }
    }

    text += " (" + std::to_string(seconds) + " 秒)";

    if (seconds >= SLOW_SECONDS && message.receivedChars == 0)
    {
        text += "\n時間がかかっています。中断ボタンで止めると、元の回答に戻ります。";
    }

    return text;
}

void RefreshCloudStatus(ChatState& state)
{
    const CloudSettings cloud = LoadCloudSettings();
    state.hasCloudKey = HasCloudApiKey(cloud);
    state.cloudName = cloud.name;
}

bool CanAskCloud(const ChatState& state)
{
    if (state.busy || state.messages.empty())
    {
        return false;
    }

    const Message& last = state.messages.back();

    return last.role == "assistant" &&
        !last.agentKey.empty() &&
        !last.content.empty() &&
        LastUserIndex(state.messages).has_value();
}

bool LastAnswerUsedRag(const ChatState& state)
{
    return !state.messages.empty() && state.messages.back().usedRag;
}

void AskCloud(ChatState& state)
{
    JoinWorker(state);

    json history;
    std::string question;
    std::string agentKey;

    {
        std::lock_guard lock(state.mutex);

        if (!CanAskCloud(state))
        {
            return;
        }

        const size_t lastUser = *LastUserIndex(state.messages);
        Message& previous = state.messages.back();
        const Message& user = state.messages[lastUser];

        history = BuildCloudHistory(state.messages, lastUser);
        question = user.content;
        agentKey = previous.agentKey;

        // 資料を使った回答なら、同じ資料を添えた質問を送る (UI で確認済み)
        const bool sendRag = previous.usedRag && !user.ragPrompt.empty();
        history.push_back({
            {"role", "user"},
            {"content", sendRag ? user.ragPrompt : user.content}
        });

        Message answer;
        answer.role = "assistant";
        answer.agent = previous.agent + " (" + state.cloudName + ")";
        answer.agentKey = agentKey;
        answer.usedRag = previous.usedRag;
        answer.note = state.cloudName + " に聞き直しました" +
            (sendRag ? std::string(" (資料も送りました)") : std::string());
        SetStatus(answer, state.cloudName + " に問い合わせ中…");

        previous.superseded = true;
        state.messages.push_back(std::move(answer));

        state.busy = true;
        state.error.clear();
    }

    state.cancel = false;

    state.worker = std::thread([&state, history, question, agentKey]() {
        std::string cloudAnswer;

        try
        {
            const Agent persona = CreateAnswerer(agentKey);
            const Agent cloud = CreateCloud();

            {
                std::lock_guard lock(state.mutex);
                state.messages.back().agent = persona.name + " (" + cloud.name + ")";
            }
            WakeUI();

            // 画面には流さず、受け取った文字数だけを進み具合として出す
            const std::string receiving = cloud.name + " から受信中…";

            cloudAnswer = RunAgent(
                cloud,
                history,
                [&state, &receiving](std::string_view text) {
                    {
                        std::lock_guard lock(state.mutex);
                        Message& answer = state.messages.back();

                        if (answer.receivedChars == 0)
                        {
                            const auto since = answer.statusSince;
                            SetStatus(answer, receiving);

                            // 経過時間は問い合わせた時から数える
                            answer.statusSince = since;
                        }

                        answer.receivedChars += CountChars(text);
                    }
                    WakeUI();
                },
                state.cancel);

            if (IsBlank(cloudAnswer))
            {
                throw std::runtime_error(cloud.name + " の回答が空でした");
            }

            if (!persona.rewrite)
            {
                std::lock_guard lock(state.mutex);
                Message& answer = state.messages.back();
                answer.agent = persona.name + " (" + cloud.name + ")";
                answer.content = cloudAnswer;
                answer.status.clear();
            }
            else
            {
                {
                    std::lock_guard lock(state.mutex);
                    Message& answer = state.messages.back();
                    answer.agent =
                        persona.name + " (" + cloud.name + " → ローカルで書き直し)";
                    answer.cloudOriginal = cloudAnswer;
                    SetStatus(
                        answer, persona.name + "が言い直し中…", CountChars(cloudAnswer));
                }
                WakeUI();

                std::string failure;

                try
                {
                    RewriteAnswer(
                        persona,
                        question,
                        cloudAnswer,
                        [&state](std::string_view text) {
                            {
                                std::lock_guard lock(state.mutex);
                                Message& answer = state.messages.back();
                                answer.content += text;
                                answer.receivedChars += CountChars(text);
                            }
                            WakeUI();
                        },
                        state.cancel);
                }
                catch (const std::exception& e)
                {
                    failure = state.cancel ? "中断しました" : e.what();
                }

                std::lock_guard lock(state.mutex);
                Message& answer = state.messages.back();
                answer.status.clear();

                // 書き直せなかったら、ChatGPT の回答をそのまま出す
                // (中断しても、途中まで書き直したものがあればそれを残す)
                if (IsBlank(answer.content))
                {
                    answer.content = cloudAnswer;
                    answer.agent = persona.name + " (" + cloud.name + ")";
                    answer.cloudOriginal.clear();
                    answer.note += "\n書き直せなかったので、" + cloud.name +
                        " の回答をそのまま表示しています" +
                        (failure.empty() ? std::string() : ": " + failure);
                }
                else if (!failure.empty() && !state.cancel)
                {
                    answer.note += "\n書き直しが途中で止まりました: " + failure;
                }
            }
        }
        catch (const std::exception& e)
        {
            // ChatGPT に聞けなかった: 聞き直す前の回答に戻す
            std::lock_guard lock(state.mutex);
            state.messages.pop_back();

            if (!state.messages.empty())
            {
                state.messages.back().superseded = false;
            }

            if (!state.cancel)
            {
                state.error = e.what();
            }
        }

        {
            std::lock_guard lock(state.mutex);
            state.busy = false;
        }
        WakeUI();
    });
}
