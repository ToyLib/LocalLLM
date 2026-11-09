// OpenAI 互換 API へのストリーミング送信と SSE の解析、埋め込みの取得

#include "llm_client.h"

#include "config.h"

#include <SDL3/SDL.h>
#include <curl/curl.h>

#include <algorithm>
#include <stdexcept>
#include <functional>

namespace
{

// サーバーの URL は config/server.json に書く
// chat_url: チャットのサーバー、embed_url: RAG 用の埋め込みサーバー (llama-server --embeddings)
constexpr const char* SERVER_CONFIG = "server.json";

// SSE受信状態
struct StreamContext
{
    std::string buffer;
    std::string answer;
    std::string error;
    bool done = false;

    // エラーのときに返ってくる本文 (SSE ではない JSON) を読むため、先頭を取っておく
    std::string raw;

    const DeltaCallback* onDelta = nullptr;
    const std::atomic<bool>* cancel = nullptr;
    std::vector<Citation>* citations = nullptr;
};

// 画面からブラウザーで開いてよい URL か (モデルの出力なので http / https だけにする)
bool IsWebUrl(const std::string& url)
{
    return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
}

// SSE の delta の annotations から出典を取り出す
void ReadCitations(const json& delta, std::vector<Citation>* citations)
{
    if (!citations || !delta.contains("annotations") || !delta["annotations"].is_array())
    {
        return;
    }

    for (const auto& annotation : delta["annotations"])
    {
        if (!annotation.is_object() || !annotation.contains("url_citation") ||
            !annotation["url_citation"].is_object())
        {
            continue;
        }

        const json& item = annotation["url_citation"];
        const std::string url = ConfigStringOr(item, "url", "");

        if (IsWebUrl(url))
        {
            citations->push_back({ConfigStringOr(item, "title", ""), url});
        }
    }
}

// SSEの1行を処理
void ProcessLine(
    std::string_view line,
    StreamContext& context)
{
    if (line.empty() || line.front() == ':')
    {
        return;
    }

    if (!line.starts_with("data:"))
    {
        return;
    }

    line.remove_prefix(5);

    if (!line.empty() && line.front() == ' ')
    {
        line.remove_prefix(1);
    }

    if (line == "[DONE]")
    {
        context.done = true;
        return;
    }

    if (line.empty())
    {
        return;
    }

    json data = json::parse(line, nullptr, false);

    if (data.is_discarded())
    {
        throw std::runtime_error("Invalid SSE JSON");
    }

    if (data.contains("error"))
    {
        const json& error = data["error"];

        if (error.is_object() && error.contains("message") &&
            error["message"].is_string())
        {
            throw std::runtime_error(error["message"].get<std::string>());
        }

        throw std::runtime_error(error.dump());
    }

    if (!data.contains("choices") ||
        !data["choices"].is_array() ||
        data["choices"].empty())
    {
        return;
    }

    const auto& choice = data["choices"][0];

    if (!choice.contains("delta"))
    {
        return;
    }

    const auto& delta = choice["delta"];

    ReadCitations(delta, context.citations);

    if (delta.contains("content") &&
        delta["content"].is_string())
    {
        std::string text =
            delta["content"].get<std::string>();

        // 表示と履歴保存用の両方に渡す
        context.answer += text;

        if (*context.onDelta)
        {
            (*context.onDelta)(text);
        }
    }
}

// libcurlの受信コールバック
size_t WriteCallback(
    char* data,
    size_t size,
    size_t count,
    void* userdata)
{
    auto* context =
        static_cast<StreamContext*>(userdata);

    const size_t bytes = size * count;

    try
    {
        constexpr size_t MAX_RAW = 64 * 1024;

        if (context->raw.size() < MAX_RAW)
        {
            context->raw.append(data, (std::min)(bytes, MAX_RAW - context->raw.size()));
        }

        context->buffer.append(data, bytes);

        size_t pos;

        while ((pos = context->buffer.find('\n'))
               != std::string::npos)
        {
            std::string line =
                context->buffer.substr(0, pos);

            context->buffer.erase(0, pos + 1);

            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }

            ProcessLine(line, *context);
        }
    }
    catch (const std::exception& e)
    {
        context->error = e.what();
        return 0;
    }
    catch (...)
    {
        context->error = "Unknown streaming error";
        return 0;
    }

    return bytes;
}

// 進捗コールバック (0以外を返すと転送を中断)
int ProgressCallback(
    void* userdata,
    curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    auto* context =
        static_cast<StreamContext*>(userdata);

    return context->cancel->load() ? 1 : 0;
}

// 埋め込みの受信状態 (ストリーミングではないので全部ためる)
struct EmbedContext
{
    std::string body;
    const std::atomic<bool>* cancel = nullptr;
};

size_t EmbedWriteCallback(
    char* data,
    size_t size,
    size_t count,
    void* userdata)
{
    auto* context =
        static_cast<EmbedContext*>(userdata);

    context->body.append(data, size * count);
    return size * count;
}

int EmbedProgressCallback(
    void* userdata,
    curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    auto* context =
        static_cast<EmbedContext*>(userdata);

    return context->cancel->load() ? 1 : 0;
}

// エラーの本文 ({"error": {"message": ...}}) からメッセージを取り出す
std::string ErrorMessage(const std::string& body)
{
    const json data = json::parse(body, nullptr, false);

    if (!data.is_discarded() && data.is_object() && data.contains("error"))
    {
        const json& error = data["error"];

        if (error.is_object() && error.contains("message") &&
            error["message"].is_string())
        {
            return error["message"].get<std::string>();
        }

        if (error.is_string())
        {
            return error.get<std::string>();
        }
    }

    return {};
}

// HTTP のエラーを、分かりやすいメッセージにする
std::string HttpError(long status, const std::string& body, const std::string& server)
{
    std::string text = server + ": HTTP " + std::to_string(status);

    if (status == 401)
    {
        text += " (API キーが無効です)";
    }
    else if (status == 429)
    {
        text += " (回数の制限か、残高不足です)";
    }

    const std::string message = ErrorMessage(body);

    if (!message.empty())
    {
        text += ": " + message;
    }

    return text;
}

bool StartsWith(const std::string& text, const char* prefix)
{
    return text.rfind(prefix, 0) == 0;
}

} // namespace

CloudSettings LoadCloudSettings()
{
    CloudSettings settings;
    const json server = LoadConfigOrEmpty(SERVER_CONFIG);

    if (!server.contains("cloud"))
    {
        return settings;
    }

    const json& cloud = server["cloud"];

    if (!cloud.is_object())
    {
        SDL_Log("Config cloud is not an object (using defaults)");
        return settings;
    }

    settings.name = ConfigStringOr(cloud, "name", settings.name);
    settings.chatUrl = ConfigStringOr(cloud, "chat_url", settings.chatUrl);
    settings.model = ConfigStringOr(cloud, "model", settings.model);
    settings.apiKeyName = ConfigStringOr(cloud, "api_key_name", settings.apiKeyName);
    settings.auth = ConfigStringOr(cloud, "auth", settings.auth);
    settings.maxCompletionTokens = static_cast<int>(ConfigNumberOr(
        cloud, "max_completion_tokens",
        static_cast<float>(settings.maxCompletionTokens)));
    settings.reasoningEffort =
        ConfigStringOr(cloud, "reasoning_effort", settings.reasoningEffort);
    settings.searchModel = ConfigStringOr(cloud, "search_model", settings.searchModel);

    if (cloud.contains("web_search") && cloud["web_search"].is_boolean())
    {
        settings.webSearch = cloud["web_search"].get<bool>();
    }

    return settings;
}

bool HasCloudApiKey(const CloudSettings& settings)
{
    return !FindApiKey(settings.apiKeyName).value.empty();
}

std::string NumberCitations(const std::string& text, std::vector<Citation>& citations)
{
    std::vector<Citation> numbered;

    // url の番号 (1 から)。無ければ title か label を題名にして足す
    auto numberOf = [&](const std::string& url, const std::string& label)
    {
        for (size_t i = 0; i < numbered.size(); ++i)
        {
            if (numbered[i].url == url)
            {
                return i + 1;
            }
        }

        std::string title = label;

        for (const auto& citation : citations)
        {
            if (citation.url == url && !citation.title.empty())
            {
                title = citation.title;
                break;
            }
        }

        numbered.push_back({title.empty() ? url : title, url});
        return numbered.size();
    };

    // [題名](URL) を探す。([題名](URL)) なら外側のかっこも取る
    std::string result;
    size_t i = 0;

    while (i < text.size())
    {
        size_t close = std::string::npos;
        size_t end = std::string::npos;

        if (text[i] == '[')
        {
            close = text.find_first_of("]\n", i + 1);
        }

        if (close != std::string::npos && text.compare(close, 2, "](") == 0 &&
            (text.compare(close + 2, 7, "http://") == 0 ||
             text.compare(close + 2, 8, "https://") == 0))
        {
            end = text.find_first_of(") \t\r\n", close + 2);
        }

        if (end == std::string::npos || text[end] != ')')
        {
            result += text[i];
            ++i;
            continue;
        }

        const size_t number = numberOf(
            text.substr(close + 2, end - close - 2),
            text.substr(i + 1, close - i - 1));

        i = end + 1;

        if (!result.empty() && result.back() == '(' && i < text.size() && text[i] == ')')
        {
            result.pop_back();
            ++i;
        }

        result += "[" + std::to_string(number) + "]";
    }

    for (const auto& citation : citations)
    {
        numberOf(citation.url, citation.title);
    }

    citations = std::move(numbered);
    return result;
}

std::string AskLLM(
    const json& messages,
    const DeltaCallback& onDelta,
    const std::atomic<bool>& cancel,
    const LLMOptions& options)
{
    // リクエストを作る (送り先はクラウドか、ローカルの llama-server)
    json request = {
        {"messages", messages},
        {"stream", true}
    };

    std::string url;
    std::string serverName;
    std::string authHeader;

    if (options.cloud)
    {
        const CloudSettings cloud = LoadCloudSettings();
        url = cloud.chatUrl;
        serverName = cloud.name;

        const ApiKey key = FindApiKey(cloud.apiKeyName);

        if (key.value.empty())
        {
            throw std::runtime_error(
                cloud.name + " の API キー (" + cloud.apiKeyName + ") が設定されていません");
        }

        // キーを平文の通信やローカルのサーバーに送らないように
        if (!StartsWith(url, "https://"))
        {
            throw std::runtime_error(
                "API キーは https の URL にだけ送ります: " + url);
        }

        authHeader = cloud.auth == "api-key"
            ? "api-key: " + key.value
            : "Authorization: Bearer " + key.value;

        request["model"] = options.webSearch ? cloud.searchModel : cloud.model;

        if (cloud.maxCompletionTokens > 0)
        {
            request["max_completion_tokens"] = cloud.maxCompletionTokens;
        }

        if (options.webSearch)
        {
            request["web_search_options"] = json::object();
        }
        else if (!cloud.reasoningEffort.empty())
        {
            request["reasoning_effort"] = cloud.reasoningEffort;
        }

        if (!options.schema.is_null())
        {
            request["response_format"] = {
                {"type", "json_schema"},
                {"json_schema", {
                    {"name", "result"},
                    {"strict", true},
                    {"schema", options.schema}
                }}
            };
        }
    }
    else
    {
        url = ConfigString(SERVER_CONFIG, "chat_url");
        serverName = "LLM server";

        // llama-server はモデル名を見ない
        request["model"] = "local";
        request["max_tokens"] = options.maxTokens;

        if (options.temperature >= 0.0f)
        {
            request["temperature"] = options.temperature;
        }

        if (!options.schema.is_null())
        {
            // llama-server は文法で出力を縛るので、小さいモデルでも形が崩れない
            request["response_format"] = {
                {"type", "json_schema"},
                {"json_schema", {{"name", "result"}, {"schema", options.schema}}}
            };
        }

        if (options.disableThinking)
        {
            // enable_thinking: Qwen3 など。gpt-oss は推論を切れないので reasoning_effort を最小にする
            // (テンプレートが使わない項目は無視される)
            request["chat_template_kwargs"] = {
                {"enable_thinking", false},
                {"reasoning_effort", "low"}
            };
        }
    }

    // 送信する (受信したテキストは WriteCallback から onDelta に渡る)
    CURL* curl = curl_easy_init();

    if (!curl)
    {
        throw std::runtime_error("curl_easy_init failed");
    }

    std::string body = request.dump();

    StreamContext context;
    context.onDelta = &onDelta;
    context.cancel = &cancel;
    context.citations = options.citations;

    curl_slist* headers = nullptr;
    headers = curl_slist_append(
        headers, "Content-Type: application/json");
    headers = curl_slist_append(
        headers, "Accept: text/event-stream");

    if (!authHeader.empty())
    {
        headers = curl_slist_append(headers, authHeader.c_str());
    }

    curl_easy_setopt(
        curl, CURLOPT_URL, url.c_str());

    curl_easy_setopt(
        curl, CURLOPT_HTTPHEADER, headers);

    curl_easy_setopt(
        curl, CURLOPT_POSTFIELDS, body.c_str());

    curl_easy_setopt(
        curl, CURLOPT_WRITEFUNCTION, WriteCallback);

    curl_easy_setopt(
        curl, CURLOPT_WRITEDATA, &context);

    // 中断ボタン用
    curl_easy_setopt(
        curl, CURLOPT_XFERINFOFUNCTION, ProgressCallback);

    curl_easy_setopt(
        curl, CURLOPT_XFERINFODATA, &context);

    curl_easy_setopt(
        curl, CURLOPT_NOPROGRESS, 0L);

    // ストリーミングなのでタイムアウトなし
    curl_easy_setopt(
        curl, CURLOPT_TIMEOUT, 0L);

    CURLcode result = curl_easy_perform(curl);

    long status = 0;
    curl_easy_getinfo(
        curl, CURLINFO_RESPONSE_CODE, &status);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    // ヘッダーにキーが入っていたので、メモリから消しておく
    std::fill(authHeader.begin(), authHeader.end(), '\0');

    if (cancel.load())
    {
        throw std::runtime_error("Cancelled");
    }

    // エラーの本文は SSE ではないので、行の処理より先に HTTP の状態を見る
    if (result == CURLE_OK && status != 200)
    {
        throw std::runtime_error(HttpError(status, context.raw, serverName));
    }

    if (!context.error.empty())
    {
        throw std::runtime_error(context.error);
    }

    if (result != CURLE_OK)
    {
        throw std::runtime_error(
            serverName + ": " + curl_easy_strerror(result));
    }

    if (!context.done)
    {
        throw std::runtime_error(
            "Stream ended before [DONE]");
    }

    return context.answer;
}

std::vector<std::vector<float>> Embed(
    const std::vector<std::string>& inputs,
    const std::atomic<bool>& cancel)
{
    const std::string url = ConfigString(SERVER_CONFIG, "embed_url");

    CURL* curl = curl_easy_init();

    if (!curl)
    {
        throw std::runtime_error("curl_easy_init failed");
    }

    json request = {
        {"model", "embed"},
        {"input", inputs}
    };

    std::string body = request.dump();

    EmbedContext context;
    context.cancel = &cancel;

    curl_slist* headers = nullptr;
    headers = curl_slist_append(
        headers, "Content-Type: application/json");

    curl_easy_setopt(
        curl, CURLOPT_URL, url.c_str());

    curl_easy_setopt(
        curl, CURLOPT_HTTPHEADER, headers);

    curl_easy_setopt(
        curl, CURLOPT_POSTFIELDS, body.c_str());

    curl_easy_setopt(
        curl, CURLOPT_WRITEFUNCTION, EmbedWriteCallback);

    curl_easy_setopt(
        curl, CURLOPT_WRITEDATA, &context);

    curl_easy_setopt(
        curl, CURLOPT_XFERINFOFUNCTION, EmbedProgressCallback);

    curl_easy_setopt(
        curl, CURLOPT_XFERINFODATA, &context);

    curl_easy_setopt(
        curl, CURLOPT_NOPROGRESS, 0L);

    // サーバーが起動していないときに待たされないように
    curl_easy_setopt(
        curl, CURLOPT_CONNECTTIMEOUT, 3L);

    CURLcode result = curl_easy_perform(curl);

    long status = 0;
    curl_easy_getinfo(
        curl, CURLINFO_RESPONSE_CODE, &status);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (cancel.load())
    {
        throw std::runtime_error("Cancelled");
    }

    if (result != CURLE_OK)
    {
        throw std::runtime_error(
            std::string("Embedding server: ") +
            curl_easy_strerror(result));
    }

    if (status != 200)
    {
        throw std::runtime_error(
            "Embedding server: HTTP " + std::to_string(status));
    }

    json response = json::parse(context.body, nullptr, false);

    if (response.is_discarded() ||
        !response.contains("data") ||
        !response["data"].is_array() ||
        response["data"].size() != inputs.size())
    {
        throw std::runtime_error("Invalid embedding response");
    }

    // index の順に並べ直す
    std::vector<std::vector<float>> vectors(inputs.size());

    for (const auto& item : response["data"])
    {
        const size_t index = item.value("index", size_t{0});

        if (index >= vectors.size() ||
            !item.contains("embedding"))
        {
            throw std::runtime_error("Invalid embedding response");
        }

        vectors[index] =
            item["embedding"].get<std::vector<float>>();
    }

    return vectors;
}
