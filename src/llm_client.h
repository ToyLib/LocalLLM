#pragma once

#include <nlohmann/json.hpp>

#include <atomic>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

using json = nlohmann::json;

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
    // (gpt-oss は推論を切れないので、推論の量を最小にする)
    bool disableThinking = false;

    // true ならローカルの LLM ではなく、クラウド (ChatGPT など、server.json の cloud) に送る
    // クラウドには maxTokens / temperature / disableThinking を送らない
    // (OpenAI の推論モデルは temperature を受け付けず、llama-server 専用の項目も通らないため)
    bool cloud = false;
};

// クラウドの LLM の設定 (config/server.json の cloud。書いていない項目は既定値)
struct CloudSettings
{
    // 画面に出す名前
    std::string name = "ChatGPT";

    std::string chatUrl = "https://api.openai.com/v1/chat/completions";
    std::string model = "gpt-5-mini";

    // API キーを探す名前 (環境変数 → 秘密ファイル。値は設定ファイルに書かない)
    std::string apiKeyName = "OPENAI_API_KEY";

    // 認証ヘッダーの形式: "bearer" (Authorization: Bearer) か "api-key" (Azure OpenAI)
    std::string auth = "bearer";

    // 0 なら送らない (サーバーの既定値)
    int maxCompletionTokens = 0;

    // 推論の深さ (minimal / low / medium / high)。空なら送らない
    std::string reasoningEffort;
};

// server.json を読んで、クラウドの設定を返す (読めなければ既定値)
CloudSettings LoadCloudSettings();

// クラウドの API キーが見つかるか (環境変数か秘密ファイル)
bool HasCloudApiKey(const CloudSettings& settings);

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
