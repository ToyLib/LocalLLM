#include "chat.h"

#include "llm_client.h"
#include "rag.h"

#include <SDL3/SDL.h>

#include <cstdio>

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

    json history = CreateHistory();

    {
        std::lock_guard lock(state.mutex);

        state.messages.push_back({"user", prompt});

        // 履歴全体を送信
        for (const auto& message : state.messages)
        {
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

    const bool useRag = state.useRag;

    state.worker = std::thread([&state, &rag, history, prompt, useRag]() mutable {
        try
        {
            if (useRag)
            {
                // 資料は送信する質問にだけ添え、画面の履歴には残さない
                std::string note;

                try
                {
                    const auto hits = SearchRag(rag, prompt, state.cancel);

                    if (!hits.empty())
                    {
                        history.back()["content"] =
                            BuildRagPrompt(hits, prompt);
                    }

                    note = FormatSources(hits);
                }
                catch (const std::exception& e)
                {
                    if (state.cancel)
                    {
                        throw;
                    }

                    // 検索できなくても、資料なしで回答は続ける
                    note = std::string("資料を参照できませんでした: ") + e.what();
                }

                {
                    std::lock_guard lock(state.mutex);
                    state.messages.back().note = std::move(note);
                }
                WakeUI();
            }

            AskLLM(
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
