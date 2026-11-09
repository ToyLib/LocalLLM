#pragma once

#include "llm_client.h"

#include <atomic>
#include <chrono>
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

// 回答を作った段階 (ローカル → ChatGPT → ChatGPT + Web 検索)
enum class AnswerStage
{
    Local,
    Cloud,
    Web
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

    // assistant: 資料 (rag/) の内容を使って答えたか (ChatGPT 宛ての履歴から除く目印)
    bool usedRag = false;

    // assistant: ChatGPT に聞き直して置き換えられた回答 (LLM に送る履歴から除き、薄く表示する)
    bool superseded = false;

    // assistant: ChatGPT に聞き直した回答の、書き直す前の元の文章 (折りたたんで表示する)
    std::string cloudOriginal;

    // assistant: 回答を作った段階
    AnswerStage stage = AnswerStage::Local;

    // assistant: Web 検索の出典 (本文の [1] などの番号の順)
    std::vector<Citation> citations;

    // assistant: 聞き直す前の回答との違いのまとめ (まとめていなければ compareVerdict は空)
    std::string compareVerdict;
    std::vector<std::string> comparePoints;

    // assistant: 回答の途中の状態 (「ChatGPT に問い合わせ中…」など。表示は FormatStatus)
    std::string status;
    std::chrono::steady_clock::time_point statusSince;

    // 今の段階で受け取った文字数と、目安の全体の文字数 (分からなければ 0)
    size_t receivedChars = 0;
    size_t totalChars = 0;
};

// 回答の途中の状態に、受け取った文字数と経過時間を添えた文 (status が空なら空)
// 呼ぶ側で state.mutex をロックしておく
std::string FormatStatus(const Message& message);

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

    // クラウド (ChatGPT) の API キーがあるか (UIスレッドだけが触る。起動時と設定の保存時に調べ直す)
    bool hasCloudKey = false;
    std::string cloudName = "ChatGPT";

    // 聞き直した後に Web 検索でも聞き直せるか (server.json の cloud.web_search。UIスレッドだけが触る)
    bool webSearch = false;
};

// ワーカースレッドからUIスレッドを起こすイベントを登録する (SDL_Init の後に1回呼ぶ)
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

// この回答の次に聞き直せる段階 (Cloud / Web。もう聞き直せなければ Local)
// 資料を使った回答は、資料の内容が検索語として外に出ないように Web 検索しない
// 呼ぶ側で state.mutex をロックしておく
AnswerStage NextStage(const ChatState& state, const Message& message);

// 最新の質問を ChatGPT に聞き直し、最新の回答の回答役の口調で書き直して表示する
// webSearch なら、Web 検索用のモデルで聞き直し、検索なしの ChatGPT の回答と比べる
// 資料を使ったほかのやりとりは送らない。最新の質問が資料を使っていたら、その資料は送る
// (資料を送ることの確認は、呼ぶ前に UI で済ませておく)
void AskCloud(ChatState& state, bool webSearch = false);
