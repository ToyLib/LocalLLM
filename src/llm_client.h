#pragma once

#include <nlohmann/json.hpp>

#include <atomic>
#include <functional>
#include <string>
#include <string_view>

using json = nlohmann::json;

// 会話履歴の初期化 (systemプロンプトのみ)
json CreateHistory();

// 受信したテキスト片ごとに呼ばれる (ワーカースレッドから呼ばれる)
using DeltaCallback = std::function<void(std::string_view)>;

// LLMに会話履歴を送信し、ストリーミングで回答を受け取る
// cancel が true になると中断して std::runtime_error を投げる
std::string AskLLM(
    const json& messages,
    const DeltaCallback& onDelta,
    const std::atomic<bool>& cancel);
