#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct Message {
    std::string role;
    std::string content;
};

// UIスレッドとワーカースレッドで共有する会話状態
struct ChatState {
    std::mutex mutex;

    // systemプロンプトを除いた会話 (user / assistant)
    std::vector<Message> messages;

    bool busy = false;
    std::string error;

    // 失敗時に入力欄へ戻す質問
    std::string restoredPrompt;

    std::atomic<bool> cancel = false;
    std::thread worker;
};

// ワーカースレッドからUIスレッドを起こすためのイベントを登録する
// (SDL_Init の後に1回呼ぶ)
void RegisterWakeEvent();

void JoinWorker(ChatState& state);

// 質問を送信してワーカースレッドで回答を受信する
void SendPrompt(ChatState& state, const std::string& prompt);
