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

    // 回答役のキー (assistant / general。ChatGPT に聞き直したときの書き直しに使う)
    std::string agentKey;

    // user: 資料を添えて送った質問 (資料を使わなかったら空)
    std::string ragPrompt;

    // assistant: 資料 (rag/) の内容を使って答えたか
    // (資料を使ったやりとりは、ChatGPT 宛ての履歴から除く)
    bool usedRag = false;

    // assistant: ChatGPT に聞き直して置き換えられた回答 (LLM に送る履歴から除き、薄く表示する)
    bool superseded = false;

    // assistant: ChatGPT に聞き直した回答の、書き直す前の元の文章 (折りたたんで表示する)
    std::string cloudOriginal;

    // assistant: 回答の途中の状態 (「ChatGPT に問い合わせ中…」など)
    std::string status;
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

    // クラウド (ChatGPT) の API キーがあるか (UIスレッドだけが触る)
    // 起動時と設定を保存したときに調べ直す
    bool hasCloudKey = false;
    std::string cloudName = "ChatGPT";
};

// ワーカースレッドからUIスレッドを起こすためのイベントを登録する
// (SDL_Init の後に1回呼ぶ)
void RegisterWakeEvent();

// UIスレッドを起こして再描画させる (どのスレッドからでも呼べる)
void WakeUI();

void JoinWorker(ChatState& state);

// 質問を送信してワーカースレッドで回答を受信する
void SendPrompt(ChatState& state, RagState& rag, const std::string& prompt);

// クラウドの API キーの有無と名前を調べ直す (UIスレッドから呼ぶ)
void RefreshCloudStatus(ChatState& state);

// 最新の回答を ChatGPT に聞き直せるか (受信中でなく、最新の回答があるとき)
// 呼ぶ側で state.mutex をロックしておく
bool CanAskCloud(const ChatState& state);

// 最新の回答が資料 (rag/) を使っているか (聞き直す前の確認に使う)
// 呼ぶ側で state.mutex をロックしておく
bool LastAnswerUsedRag(const ChatState& state);

// 最新の質問を ChatGPT に聞き直し、最新の回答の回答役の口調で書き直して表示する
// 資料を使ったほかのやりとりは送らない。最新の質問が資料を使っていたら、その資料は送る
// (資料を送ることの確認は、呼ぶ前に UI で済ませておく)
void AskCloud(ChatState& state);
