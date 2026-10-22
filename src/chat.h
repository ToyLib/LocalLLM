#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct RagState;

// 質問に rag/ の資料を添えるか
enum class RagMode
{
    Auto,    // ルーターが質問ごとに判断する
    Always,
    Never
};

struct Message
{
    std::string role;
    std::string content;

    // 回答の下に表示する補足 (RAG で参照した資料など)
    std::string note;

    // 回答したエージェントの名前 (assistant のみ)
    std::string agent;
};

// UIスレッドとワーカースレッドで共有する会話状態
struct ChatState
{
    std::mutex mutex;

    // systemプロンプトを除いた会話 (user / assistant)
    std::vector<Message> messages;

    bool busy = false;
    std::string error;

    // 失敗時に入力欄へ戻す質問
    std::string restoredPrompt;

    std::atomic<bool> cancel = false;
    std::thread worker;

    // 質問に rag/ の資料を添えるか (UIスレッドだけが触る)
    RagMode ragMode = RagMode::Auto;
};

// ワーカースレッドからUIスレッドを起こすためのイベントを登録する
// (SDL_Init の後に1回呼ぶ)
void RegisterWakeEvent();

// UIスレッドを起こして再描画させる (どのスレッドからでも呼べる)
void WakeUI();

void JoinWorker(ChatState& state);

// 質問を送信してワーカースレッドで回答を受信する
void SendPrompt(ChatState& state, RagState& rag, const std::string& prompt);
