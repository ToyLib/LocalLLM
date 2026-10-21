#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// 文書を分割した1かたまり
struct RagChunk
{
    std::string source;   // rag/ からの相対パス
    std::string heading;  // Markdown の見出し (無ければ空)
    std::string text;
    std::vector<float> vector;  // 正規化済みの埋め込み
};

// 検索で見つかったチャンク
struct RagHit
{
    std::string source;
    std::string heading;
    std::string text;
    float score = 0.0f;  // コサイン類似度
};

// UIスレッド・インデックス作成スレッド・会話スレッドで共有する状態
struct RagState
{
    std::mutex mutex;

    std::vector<RagChunk> chunks;

    bool building = false;
    std::string status;

    std::atomic<bool> cancel = false;
    std::thread worker;
};

// rag/ を走査してインデックスをバックグラウンドで作る
// (変更の無いファイルは保存済みのインデックスを再利用する)
void StartRagIndexing(RagState& state);

void JoinRagWorker(RagState& state);

// 質問に近いチャンクを返す (埋め込みサーバーに問い合わせるので
// ワーカースレッドから呼ぶ)
std::vector<RagHit> SearchRag(
    RagState& state,
    const std::string& query,
    const std::atomic<bool>& cancel);

// 検索結果を参考資料として質問に添えたプロンプトを作る
std::string BuildRagPrompt(
    const std::vector<RagHit>& hits,
    const std::string& question);
