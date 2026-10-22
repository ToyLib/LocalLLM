#pragma once

#include <nlohmann/json.hpp>

#include <atomic>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

using json = nlohmann::json;

// 会話履歴の初期化 (systemプロンプトのみ)
json CreateHistory();

// 受信したテキスト片ごとに呼ばれる (ワーカースレッドから呼ばれる)
using DeltaCallback = std::function<void(std::string_view)>;

// 1回の問い合わせごとに変えられる設定
struct LLMOptions
{
    int maxTokens = 1024;

    // 負の値ならサーバーの既定値を使う
    float temperature = -1.0f;

    // JSON スキーマ (null 以外なら、回答をこの形の JSON に制限する)
    json schema = nullptr;

    // 考える過程を出すモデル (Qwen3 など) で、考えずに答えさせる
    bool disableThinking = false;
};

// LLMに会話履歴を送信し、ストリーミングで回答を受け取る
// cancel が true になると中断して std::runtime_error を投げる
std::string AskLLM(
    const json& messages,
    const DeltaCallback& onDelta,
    const std::atomic<bool>& cancel,
    const LLMOptions& options = {});

// 埋め込みサーバーで文章をベクトルに変換する (inputs と同じ順で返す)
// cancel が true になると中断して std::runtime_error を投げる
std::vector<std::vector<float>> Embed(
    const std::vector<std::string>& inputs,
    const std::atomic<bool>& cancel);
