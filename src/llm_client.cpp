#include "llm_client.h"

#include <curl/curl.h>

#include <stdexcept>

namespace
{

constexpr const char* SERVER_URL =
    "http://127.0.0.1:8080/v1/chat/completions";

// RAG 用の埋め込みサーバー (llama-server --embeddings)
constexpr const char* EMBED_URL =
    "http://127.0.0.1:8081/v1/embeddings";

constexpr const char* SYSTEM_PROMPT =
    "あなたは親切なAIアシスタントです。"
    "日本語で回答してください。";

// SSE受信状態
struct StreamContext
{
    std::string buffer;
    std::string answer;
    std::string error;
    bool done = false;
    const DeltaCallback* onDelta = nullptr;
    const std::atomic<bool>* cancel = nullptr;
};

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
        throw std::runtime_error(data["error"].dump());
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

} // namespace

json CreateHistory()
{
    return json::array({
        {
            {"role", "system"},
            {"content", SYSTEM_PROMPT}
        }
    });
}

std::string AskLLM(
    const json& messages,
    const DeltaCallback& onDelta,
    const std::atomic<bool>& cancel,
    const LLMOptions& options)
{
    CURL* curl = curl_easy_init();

    if (!curl)
    {
        throw std::runtime_error("curl_easy_init failed");
    }

    json request = {
        {"model", "local"},
        {"messages", messages},
        {"max_tokens", options.maxTokens},
        {"stream", true}
    };

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
        request["chat_template_kwargs"] = {{"enable_thinking", false}};
    }

    std::string body = request.dump();

    StreamContext context;
    context.onDelta = &onDelta;
    context.cancel = &cancel;

    curl_slist* headers = nullptr;
    headers = curl_slist_append(
        headers, "Content-Type: application/json");
    headers = curl_slist_append(
        headers, "Accept: text/event-stream");

    curl_easy_setopt(
        curl, CURLOPT_URL, SERVER_URL);

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

    if (cancel.load())
    {
        throw std::runtime_error("Cancelled");
    }

    if (!context.error.empty())
    {
        throw std::runtime_error(context.error);
    }

    if (result != CURLE_OK)
    {
        throw std::runtime_error(curl_easy_strerror(result));
    }

    if (status != 200)
    {
        throw std::runtime_error(
            "HTTP " + std::to_string(status));
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
        curl, CURLOPT_URL, EMBED_URL);

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
