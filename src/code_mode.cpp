// コーディング練習: コードの生成・解説と、サブプロセスでの実行

#include "code_mode.h"

#include "agent.h"
#include "chat.h"
#include "config.h"
#include "llm_client.h"

#include <SDL3/SDL.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace
{

constexpr const char* CODE_CONFIG = "code.json";

// code.json に無いときの既定値
constexpr float DEFAULT_TIMEOUT_SECONDS = 10.0f;
constexpr int DEFAULT_MAX_OUTPUT_BYTES = 20000;
constexpr int DEFAULT_MAX_TOKENS = 2048;
constexpr int DEFAULT_EXPLAIN_MAX_TOKENS = 1536;

constexpr const char* DEFAULT_GENERATE_PROMPT =
    "あなたはプログラミングを教えるアシスタントです。"
    "ユーザーの依頼に合う Python 3 のプログラムを、標準ライブラリだけで、短く読みやすく書いてください。"
    "回答は ```python で始まるコードブロック 1 つだけにしてください。";

constexpr const char* DEFAULT_EXPLAIN_PROMPT =
    "あなたはプログラミングを教えるアシスタントです。"
    "渡された Python のコードを、初心者に向けて日本語で解説してください。"
    "全体の流れを 2〜3 文で書いてから、大事な部分だけを「N 行目: ...」の形で書いてください。";

// 実行したコードに渡す環境変数 (API キーなどを見せないため、これだけにする)
constexpr const char* PASSED_ENVIRONMENT[] = {
    "PATH", "HOME", "USERPROFILE", "LANG", "LC_ALL", "TMPDIR", "TEMP", "TMP",
    "SYSTEMROOT", "COMSPEC", "PATHEXT"};

struct CodeSettings
{
    float timeoutSeconds = DEFAULT_TIMEOUT_SECONDS;
    int maxOutputBytes = DEFAULT_MAX_OUTPUT_BYTES;
    int maxTokens = DEFAULT_MAX_TOKENS;
    int explainMaxTokens = DEFAULT_EXPLAIN_MAX_TOKENS;
    std::string explainPersona;
    std::string pythonPath;
    std::string generatePrompt = DEFAULT_GENERATE_PROMPT;
    std::string explainPrompt = DEFAULT_EXPLAIN_PROMPT;
};

// 正の数だけ受け取る (無い・正しくないときは既定値)
float PositiveNumber(const json& config, const char* key, float fallback)
{
    const float value = ConfigNumberOr(config, key, fallback);
    return value > 0.0f ? value : fallback;
}

// 文字列か、文字列の配列 (改行でつなげて 1 つにする)
std::string PromptText(const json& config, const char* key, const char* fallback)
{
    const auto found = config.find(key);

    if (found == config.end())
    {
        return fallback;
    }

    if (found->is_string() && !found->get<std::string>().empty())
    {
        return found->get<std::string>();
    }

    if (found->is_array() && !found->empty())
    {
        std::string text;

        for (const auto& line : *found)
        {
            if (!line.is_string())
            {
                return fallback;
            }

            if (!text.empty())
            {
                text += "\n";
            }

            text += line.get<std::string>();
        }

        return text;
    }

    return fallback;
}

CodeSettings LoadCodeSettings()
{
    const json config = LoadConfigOrEmpty(CODE_CONFIG);

    CodeSettings settings;
    settings.timeoutSeconds = PositiveNumber(config, "timeout_seconds", DEFAULT_TIMEOUT_SECONDS);
    settings.maxOutputBytes = static_cast<int>(
        PositiveNumber(config, "max_output_bytes", DEFAULT_MAX_OUTPUT_BYTES));
    settings.maxTokens = static_cast<int>(PositiveNumber(config, "max_tokens", DEFAULT_MAX_TOKENS));
    settings.explainMaxTokens = static_cast<int>(
        PositiveNumber(config, "explain_max_tokens", DEFAULT_EXPLAIN_MAX_TOKENS));
    settings.explainPersona = ConfigStringOr(config, "explain_persona", "");
    settings.pythonPath = ConfigStringOr(config, "python_path", "");
    settings.generatePrompt = PromptText(config, "generate_prompt", DEFAULT_GENERATE_PROMPT);
    settings.explainPrompt = PromptText(config, "explain_prompt", DEFAULT_EXPLAIN_PROMPT);
    return settings;
}

// explain_persona (agents.json のキー) の口調の指示 (無ければ空)
std::string PersonaPrompt(const std::string& key)
{
    if (key.empty())
    {
        return "";
    }

    const json agents = LoadConfigOrEmpty("agents.json");
    const auto entry = agents.find(key);

    if (entry == agents.end() || !entry->is_object())
    {
        return "";
    }

    const auto prompt = entry->find("system_prompt");

    if (prompt == entry->end())
    {
        return "";
    }

    std::string text;

    if (prompt->is_string())
    {
        text = prompt->get<std::string>();
    }
    else if (prompt->is_array())
    {
        for (const auto& line : *prompt)
        {
            if (line.is_string())
            {
                text += line.get<std::string>();
            }
        }
    }

    if (text.empty())
    {
        return "";
    }

    return "\n\n解説の口調は、次の役の指示に従ってください。"
           "ただし、解説の形 (全体の流れとポイント) は変えないでください。\n" + text;
}

bool IsCancelled(const std::exception& e)
{
    return std::string(e.what()) == "Cancelled";
}

// 作業フォルダ (ユーザーごとのフォルダーの workspace/)
fs::path WorkspacePath()
{
    const std::string base = UserDataDirectoryPath();
    const fs::path directory =
        fs::path(std::u8string(base.begin(), base.end())) / "workspace";
    fs::create_directories(directory);
    return directory;
}

std::string PathText(const fs::path& path)
{
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

// ワーカーを起こす前に、前のワーカーの終了を待つ (すでに終わっているので一瞬)
void JoinPrevious(CodeState& state)
{
    if (state.worker.joinable())
    {
        state.worker.join();
    }
}

void Explain(CodeState& state, const std::string& code)
{
    const CodeSettings settings = LoadCodeSettings();

    {
        std::lock_guard lock(state.mutex);
        state.explaining = true;
        state.explanation.clear();
    }
    WakeUI();

    std::string error;

    try
    {
        Agent agent;
        agent.name = "解説";
        agent.systemPrompt = settings.explainPrompt + PersonaPrompt(settings.explainPersona);
        agent.options.maxTokens = settings.explainMaxTokens;
        agent.options.temperature = 0.3f;
        agent.options.disableThinking = true;

        std::string received;
        const json messages = json::array({
            {{"role", "user"},
             {"content",
              "次のコードを解説してください。各行の頭の「番号:」は行番号で、"
              "コードの一部ではありません。\n\n" + NumberedCode(code)}}});

        RunAgent(
            agent, messages,
            [&state, &received](std::string_view text) {
                received += text;
                std::lock_guard lock(state.mutex);
                state.explanation = received;
                WakeUI();
            },
            state.cancel);

        std::lock_guard lock(state.mutex);

        // 前後の空白を落とす
        const size_t first = state.explanation.find_first_not_of(" \t\r\n");
        const size_t last = state.explanation.find_last_not_of(" \t\r\n");
        state.explanation = first == std::string::npos
            ? std::string() : state.explanation.substr(first, last - first + 1);
    }
    catch (const std::exception& e)
    {
        if (!IsCancelled(e))
        {
            error = e.what();
        }
    }

    {
        std::lock_guard lock(state.mutex);
        state.explaining = false;
        state.error = error;
    }
    WakeUI();
}

void Generate(CodeState& state, const std::string& prompt, const std::string& current)
{
    const CodeSettings settings = LoadCodeSettings();
    std::string error;

    try
    {
        // 直前のコードがあれば、それを直す依頼として送る
        const std::string content = current.find_first_not_of(" \t\r\n") != std::string::npos
            ? "いまのコード:\n```python\n" + current + "\n```\n\n依頼: " + prompt
            : prompt;

        Agent agent;
        agent.name = "コード生成";
        agent.systemPrompt = settings.generatePrompt;
        agent.options.maxTokens = settings.maxTokens;
        agent.options.temperature = 0.2f;
        agent.options.disableThinking = true;

        std::string received;
        bool started = false;
        const json messages = json::array({{{"role", "user"}, {"content", content}}});

        const std::string answer = RunAgent(
            agent, messages,
            [&state, &received, &started](std::string_view text) {
                received += text;
                const std::string code = ExtractCode(received);

                std::lock_guard lock(state.mutex);

                if (!started)
                {
                    // 受信が始まったら、前の解説と実行結果を消す
                    started = true;
                    state.explanation.clear();
                    state.output.clear();
                    state.runStatus.clear();
                }

                state.code = code;
                ++state.codeVersion;
                WakeUI();
            },
            state.cancel);

        const std::string code = ExtractCode(answer);

        {
            std::lock_guard lock(state.mutex);
            state.code = code;
            ++state.codeVersion;
            state.originalCode = code;
            state.generating = false;
        }
        WakeUI();

        if (code.find_first_not_of(" \t\r\n") == std::string::npos)
        {
            throw std::runtime_error("コードが返ってきませんでした");
        }

        Explain(state, code);
        return;
    }
    catch (const std::exception& e)
    {
        if (!IsCancelled(e))
        {
            error = e.what();
        }
    }

    {
        std::lock_guard lock(state.mutex);
        state.generating = false;
        state.error = error;
    }
    WakeUI();
}

// 実行する (ワーカースレッドで)
void Run(CodeState& state, const std::string& code)
{
    const CodeSettings settings = LoadCodeSettings();
    std::string status;
    std::string error;

    SDL_Process* process = nullptr;
    SDL_Environment* environment = nullptr;

    try
    {
        const fs::path directory = WorkspacePath();
        const fs::path script = directory / "main.py";

        {
            std::ofstream file(script, std::ios::binary);

            if (!file)
            {
                throw std::runtime_error("コードを書き出せませんでした: " + PathText(script));
            }

            file << code << "\n";
        }

        // 環境変数は最低限だけ渡す
        environment = SDL_CreateEnvironment(false);

        for (const char* name : PASSED_ENVIRONMENT)
        {
            if (const char* value = SDL_getenv(name))
            {
                SDL_SetEnvironmentVariable(environment, name, value, true);
            }
        }

        SDL_SetEnvironmentVariable(environment, "PYTHONIOENCODING", "utf-8", true);
        SDL_SetEnvironmentVariable(environment, "PYTHONUNBUFFERED", "1", true);

        std::string python = settings.pythonPath;

        if (python.empty())
        {
#if defined(_WIN32)
            python = "python";
#else
            python = "python3";
#endif
        }

        // -I: 環境変数の PYTHON* とユーザーの site-packages を読まない
        const std::string scriptName = "main.py";
        const std::string directoryText = PathText(directory);
        const char* argv[] = {python.c_str(), "-I", scriptName.c_str(), nullptr};

        const SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv);
        SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ENVIRONMENT_POINTER, environment);
        SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
        SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
        SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
        SDL_SetStringProperty(props, SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING,
                              directoryText.c_str());
#if defined(_WIN32)
        // コンソールウィンドウを出さない
        SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
#endif
        process = SDL_CreateProcessWithProperties(props);
        SDL_DestroyProperties(props);

        if (!process)
        {
            throw std::runtime_error(
                "Python (" + python + ") を起動できませんでした。"
                "Python 3 を入れるか、code.json の python_path で場所を指定してください");
        }

        {
            std::lock_guard lock(state.mutex);
            state.process = process;
        }

        // 制限時間が過ぎたら止める (読み取りは終わりまで待つので、別スレッドで見張る)
        std::atomic<bool> finished = false;
        std::atomic<bool> timedOut = false;
        const auto started = std::chrono::steady_clock::now();
        const auto deadline = started + std::chrono::milliseconds(
            static_cast<int>(settings.timeoutSeconds * 1000.0f));

        std::thread watcher([&]() {
            while (!finished)
            {
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    timedOut = true;
                    std::lock_guard lock(state.mutex);

                    if (state.process)
                    {
                        SDL_KillProcess(state.process, true);
                    }

                    return;
                }

                SDL_Delay(20);
            }
        });

        SDL_IOStream* out = SDL_GetProcessOutput(process);
        std::string received;
        bool truncated = false;
        char buffer[4096];

        while (out)
        {
            const size_t size = SDL_ReadIO(out, buffer, sizeof(buffer));

            if (size == 0)
            {
                // まだ届いていないだけなら待つ (終わり (EOF) かエラーなら抜ける)
                if (SDL_GetIOStatus(out) == SDL_IO_STATUS_NOT_READY)
                {
                    SDL_Delay(10);
                    continue;
                }

                break;
            }

            size_t usable = size;

            if (received.size() + size > static_cast<size_t>(settings.maxOutputBytes))
            {
                usable = static_cast<size_t>(settings.maxOutputBytes) - received.size();
                truncated = true;
            }

            received.append(buffer, usable);

            {
                std::lock_guard lock(state.mutex);
                state.output = received;
            }
            WakeUI();

            if (truncated)
            {
                std::lock_guard lock(state.mutex);
                SDL_KillProcess(state.process, true);
                break;
            }
        }

        int exitCode = -1;
        SDL_WaitProcess(process, true, &exitCode);
        finished = true;
        watcher.join();

        {
            std::lock_guard lock(state.mutex);
            state.process = nullptr;
            state.output = received;
        }

        const double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started).count();
        char text[96];

        if (timedOut)
        {
            std::snprintf(text, sizeof(text), "%g 秒たったので止めました", settings.timeoutSeconds);
            status = text;
        }
        else if (truncated)
        {
            status = "出力が多すぎるので止めました";
        }
        else if (exitCode == 0)
        {
            std::snprintf(text, sizeof(text), "終了しました (%.1f 秒)", elapsed);
            status = text;
        }
        else
        {
            std::snprintf(text, sizeof(text), "エラーで終了しました (終了コード %d)", exitCode);
            status = text;
        }
    }
    catch (const std::exception& e)
    {
        error = e.what();
    }

    if (process)
    {
        SDL_DestroyProcess(process);
    }

    if (environment)
    {
        SDL_DestroyEnvironment(environment);
    }

    {
        std::lock_guard lock(state.mutex);
        state.process = nullptr;
        state.running = false;
        state.runStatus = status;
        state.error = error;
    }
    WakeUI();
}

} // namespace

std::string ExtractCode(const std::string& text)
{
    // 開きの ``` と、その行の言語名を読み飛ばす
    const size_t open = text.find("```");

    if (open == std::string::npos)
    {
        // ブロックが無ければ、回答の全体をコードとみなす
        const size_t first = text.find_first_not_of('\n');
        const size_t last = text.find_last_not_of('\n');
        return first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
    }

    const size_t lineEnd = text.find('\n', open);

    // 受信し始めで、まだ改行まで届いていない
    if (lineEnd == std::string::npos)
    {
        return "";
    }

    const size_t bodyStart = lineEnd + 1;
    const size_t close = text.find("```", bodyStart);
    std::string body = text.substr(
        bodyStart, close == std::string::npos ? std::string::npos : close - bodyStart);

    // CRLF と、末尾の改行を落とす
    std::string normalized;

    for (const char c : body)
    {
        if (c != '\r')
        {
            normalized += c;
        }
    }

    while (!normalized.empty() && normalized.back() == '\n')
    {
        normalized.pop_back();
    }

    return normalized;
}

std::string NumberedCode(const std::string& code)
{
    std::istringstream lines(code);
    std::string line;
    std::string result;
    int number = 1;

    while (std::getline(lines, line))
    {
        if (number > 1)
        {
            result += "\n";
        }

        result += std::to_string(number++) + ": " + line;
    }

    return result;
}

void StartGenerate(CodeState& state, const std::string& prompt, const std::string& currentCode)
{
    {
        std::lock_guard lock(state.mutex);

        if (state.Busy() || state.running)
        {
            return;
        }

        state.generating = true;
        state.error.clear();
        state.request = prompt;
        state.code = currentCode;
        state.cancel = false;
    }

    JoinPrevious(state);
    state.worker = std::thread(
        [&state, prompt, currentCode]() { Generate(state, prompt, currentCode); });
    WakeUI();
}

void StartExplain(CodeState& state, const std::string& currentCode)
{
    {
        std::lock_guard lock(state.mutex);

        if (state.Busy() || state.running ||
            currentCode.find_first_not_of(" \t\r\n") == std::string::npos)
        {
            return;
        }

        state.explaining = true;
        state.error.clear();
        state.code = currentCode;
        state.cancel = false;
    }

    JoinPrevious(state);
    state.worker = std::thread([&state, currentCode]() { Explain(state, currentCode); });
    WakeUI();
}

void StartRun(CodeState& state, const std::string& code)
{
    {
        std::lock_guard lock(state.mutex);

        if (state.Busy() || state.running ||
            code.find_first_not_of(" \t\r\n") == std::string::npos)
        {
            return;
        }

        state.code = code;
        state.running = true;
        state.output.clear();
        state.runStatus = "実行中…";
        state.error.clear();
    }

    JoinPrevious(state);
    state.worker = std::thread([&state, code]() { Run(state, code); });
    WakeUI();
}

void CancelGenerate(CodeState& state)
{
    state.cancel = true;
}

void StopRun(CodeState& state)
{
    std::lock_guard lock(state.mutex);

    if (state.process)
    {
        SDL_KillProcess(state.process, true);
    }
}

void RevertCode(CodeState& state)
{
    {
        std::lock_guard lock(state.mutex);

        if (state.Busy() || state.running)
        {
            return;
        }

        state.code = state.originalCode;
        ++state.codeVersion;
    }

    WakeUI();
}

void ShutdownCode(CodeState& state)
{
    state.cancel = true;
    StopRun(state);

    if (state.worker.joinable())
    {
        state.worker.join();
    }
}
