#include "chat.h"

#include "llm_client.h"

#include <SDL3/SDL.h>

namespace
{

// ワーカースレッドからUIスレッドを起こすためのイベント
Uint32 g_wakeEvent = 0;

void WakeUI()
{
    SDL_Event event{};
    event.type = g_wakeEvent;
    SDL_PushEvent(&event);
}

} // namespace

void RegisterWakeEvent()
{
    g_wakeEvent = SDL_RegisterEvents(1);
}

void JoinWorker(ChatState& state)
{
    if (state.worker.joinable())
    {
        state.worker.join();
    }
}

void SendPrompt(ChatState& state, const std::string& prompt)
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

    state.worker = std::thread([&state, history, prompt] {
        try
        {
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
