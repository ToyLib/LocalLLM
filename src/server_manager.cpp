#include "server_manager.h"

#include "chat.h"
#include "config.h"

#include <SDL3/SDL.h>
#include <curl/curl.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace
{

constexpr const char* SERVER_CONFIG = "server.json";

#if defined(_WIN32)
constexpr const char* BINARY_NAME = "llama-server.exe";
constexpr char PATH_SEPARATOR = ';';
#else
constexpr const char* BINARY_NAME = "llama-server";
constexpr char PATH_SEPARATOR = ':';
#endif

// 確認のために llama-server を実行するときに待つ時間
constexpr auto CHECK_TIMEOUT = std::chrono::seconds(15);

// 普通に止めるのを待つ時間 (過ぎたら強制終了)
constexpr auto STOP_TIMEOUT = std::chrono::seconds(5);

// 画面に出すログの長さ
constexpr size_t MAX_DETAIL_BYTES = 120;
constexpr int LOG_TAIL_LINES = 8;

fs::path Utf8ToPath(const std::string& text)
{
    return fs::path(std::u8string(text.begin(), text.end()));
}

std::string PathToUtf8(const fs::path& path)
{
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

std::string EnvOr(const char* name)
{
    const char* value = SDL_getenv(name);
    return value ? value : "";
}

fs::path LogDirectory()
{
    return Utf8ToPath(UserDataDirectoryPath()) / "logs";
}

// 起動するサーバーの内容 (変わったら起動し直す)
struct ServerSpec
{
    bool run = false;
    int port = 0;
    std::string serverPath;
    std::vector<std::string> args;

    bool operator==(const ServerSpec&) const = default;
};

// 管理用のスレッドだけが触る
struct Slot
{
    const char* fileName;

    ServerSpec spec;
    SDL_Process* process = nullptr;

    // 画面に出す状態の、管理用のスレッド側の値
    ServerView view;
};

enum class Health
{
    None,     // 応答なし (サーバーが無い、または別のもの)
    Loading,  // llama-server がモデルを読み込み中
    Ready
};

struct FoundBinary
{
    std::string path;
    std::string source;
    std::string error;
};

struct Manager
{
    std::mutex mutex;
    std::condition_variable changed;
    std::thread worker;

    bool stop = false;
    bool reload = true;
    bool retry = false;

    // 読み直した設定を、すべてのサーバーに反映し終わったか
    bool settled = false;

    ServerManagerView view;
};

Manager manager;

// 管理用のスレッドだけが触る
Slot chatSlot{"chat-server"};
Slot embedSlot{"embed-server"};
std::optional<FoundBinary> foundBinary;
std::string verifiedPath;
std::string gpuWarning;

bool StopRequested()
{
    std::lock_guard lock(manager.mutex);
    return manager.stop;
}

// --------------------------------------------------
// 設定
// --------------------------------------------------

// http://127.0.0.1:8080/... の形ならポートを返す (ローカルでなければ空)
std::optional<int> LocalPort(const std::string& url)
{
    constexpr std::string_view SCHEME = "http://";

    if (!url.starts_with(SCHEME))
    {
        return std::nullopt;
    }

    std::string authority = url.substr(SCHEME.size());
    authority = authority.substr(0, authority.find('/'));

    const size_t colon = authority.rfind(':');
    const std::string host = authority.substr(0, colon);

    if (host != "127.0.0.1" && host != "localhost")
    {
        return std::nullopt;
    }

    if (colon == std::string::npos)
    {
        return 80;
    }

    try
    {
        const int port = std::stoi(authority.substr(colon + 1));

        if (port > 0 && port < 65536)
        {
            return port;
        }
    }
    catch (const std::exception&)
    {
    }

    return std::nullopt;
}

ServerSpec MakeSpec(
    const ManagedSettings& settings,
    const std::string& url,
    const std::string& model,
    std::vector<std::string> extra)
{
    ServerSpec spec;
    const std::optional<int> port = LocalPort(url);

    if (!settings.enabled || model.empty() || !port)
    {
        return spec;
    }

    spec.run = true;
    spec.port = *port;
    spec.serverPath = settings.serverPath;
    spec.args = {
        "-hf", model,
        "-ngl", std::to_string(settings.gpuLayers),
    };
    spec.args.insert(spec.args.end(), extra.begin(), extra.end());
    spec.args.insert(spec.args.end(), {
        "--host", "127.0.0.1",
        "--port", std::to_string(*port),
    });

    return spec;
}

// --------------------------------------------------
// llama-server を探す
// --------------------------------------------------

bool IsExecutable(const fs::path& path)
{
    std::error_code error;

    if (!fs::is_regular_file(path, error))
    {
        return false;
    }

#if defined(_WIN32)
    return true;
#else
    return access(path.c_str(), X_OK) == 0;
#endif
}

FoundBinary Absolute(const fs::path& path, const char* source)
{
    std::error_code error;
    const fs::path absolute = fs::absolute(path, error);
    return {PathToUtf8(error ? path : absolute), source, ""};
}

std::optional<FoundBinary> SearchPath()
{
    const std::string path = EnvOr("PATH");
    size_t start = 0;

    while (start <= path.size())
    {
        size_t end = path.find(PATH_SEPARATOR, start);

        if (end == std::string::npos)
        {
            end = path.size();
        }

        const fs::path directory = Utf8ToPath(path.substr(start, end - start));
        start = end + 1;

        // 空や相対の項目は、カレントディレクトリの実行ファイルを拾うので見ない
        if (directory.is_absolute() && IsExecutable(directory / BINARY_NAME))
        {
            return Absolute(directory / BINARY_NAME, "PATH");
        }
    }

#if defined(__APPLE__)
    // Finder から起動すると PATH に Homebrew が入らない
    for (const char* directory : {"/opt/homebrew/bin", "/usr/local/bin"})
    {
        if (IsExecutable(fs::path(directory) / BINARY_NAME))
        {
            return Absolute(fs::path(directory) / BINARY_NAME, "Homebrew");
        }
    }
#endif

    return std::nullopt;
}

fs::path DefaultLlamaDirectory()
{
    if (const std::string directory = EnvOr("LLAMA_DIR"); !directory.empty())
    {
        return Utf8ToPath(directory);
    }

#if defined(_WIN32)
    const std::string home = EnvOr("USERPROFILE");
#else
    const std::string home = EnvOr("HOME");
#endif

    return home.empty() ? fs::path() : Utf8ToPath(home) / "Repos" / "llama.cpp";
}

FoundBinary FindBinary(const std::string& serverPath)
{
    if (!serverPath.empty())
    {
        const fs::path path = Utf8ToPath(serverPath);

        if (IsExecutable(path))
        {
            return Absolute(path, "設定");
        }

        return {"", "", "設定の llama-server が見つかりません: " + serverPath};
    }

    if (const std::string env = EnvOr("LLAMA_SERVER"); !env.empty())
    {
        if (IsExecutable(Utf8ToPath(env)))
        {
            return Absolute(Utf8ToPath(env), "環境変数 LLAMA_SERVER");
        }

        return {"", "", "環境変数 LLAMA_SERVER の llama-server が見つかりません: " + env};
    }

    if (const char* base = SDL_GetBasePath())
    {
        const fs::path bundled = Utf8ToPath(base) / "llama" / BINARY_NAME;

        if (IsExecutable(bundled))
        {
            return Absolute(bundled, "同梱");
        }
    }

    if (const fs::path directory = DefaultLlamaDirectory(); !directory.empty())
    {
#if defined(_WIN32)
        const fs::path built = directory / "build" / "bin" / "Release" / BINARY_NAME;
#else
        const fs::path built = directory / "build" / "bin" / BINARY_NAME;
#endif

        if (IsExecutable(built))
        {
            return Absolute(built, "LLAMA_DIR のビルド");
        }
    }

    if (std::optional<FoundBinary> found = SearchPath())
    {
        return *found;
    }

    return {"", "", "llama-server が見つかりません。設定画面のサーバーのタブで場所を指定してください"};
}

// --------------------------------------------------
// プロセス
// --------------------------------------------------

// 標準出力と標準エラーを out に流して起動する
SDL_Process* CreateProcess(const std::vector<std::string>& args, SDL_IOStream* out)
{
    std::vector<const char*> argv;

    for (const auto& arg : args)
    {
        argv.push_back(arg.c_str());
    }

    argv.push_back(nullptr);

    const SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.data());
    SDL_SetNumberProperty(
        props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_REDIRECT);
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_POINTER, out);
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);

#if defined(_WIN32)
    // コンソールウィンドウを出さない
    // (Unix のバックグラウンド起動は親から切り離されて、終了を検知できなくなるので Windows だけ)
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
#endif

    SDL_Process* process = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    return process;
}

std::string ReadFileText(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), {});
}

struct CheckResult
{
    bool finished = false;
    int exitCode = -1;
    std::string output;
};

// 実行して終わるのを待つ (--version など)。時間内に終わらなければ止める
CheckResult RunCheck(const std::string& path, const char* option)
{
    fs::create_directories(LogDirectory());
    const fs::path outputPath = LogDirectory() / "server-check.log";

    SDL_IOStream* out = SDL_IOFromFile(PathToUtf8(outputPath).c_str(), "w");

    if (!out)
    {
        return {};
    }

    SDL_Process* process = CreateProcess({path, option}, out);
    SDL_CloseIO(out);

    if (!process)
    {
        return {};
    }

    CheckResult result;
    const auto deadline = Clock::now() + CHECK_TIMEOUT;

    while (!(result.finished = SDL_WaitProcess(process, false, &result.exitCode)))
    {
        if (Clock::now() >= deadline || StopRequested())
        {
            SDL_KillProcess(process, true);
            SDL_WaitProcess(process, true, nullptr);
            break;
        }

        SDL_Delay(50);
    }

    SDL_DestroyProcess(process);
    result.output = ReadFileText(outputPath);
    return result;
}

// --list-devices の「Available devices:」の後に、BLAS 以外のデバイスがあるか
bool HasGpuDevice(const std::string& output)
{
    constexpr std::string_view HEADER = "Available devices:";
    size_t position = output.find(HEADER);

    if (position == std::string::npos)
    {
        return false;
    }

    position += HEADER.size();

    while (position < output.size())
    {
        size_t end = output.find('\n', position);

        if (end == std::string::npos)
        {
            end = output.size();
        }

        const std::string line = output.substr(position, end - position);
        const size_t first = line.find_first_not_of(" \t\r");

        if (first != std::string::npos && line.compare(first, 4, "BLAS") != 0)
        {
            return true;
        }

        position = end + 1;
    }

    return false;
}

// ログの最後の行 (\r で上書きする進捗も 1 行として扱う)。制御文字とエスケープは除く
std::vector<std::string> LastLines(const fs::path& path, int count)
{
    constexpr std::streamoff MAX_BYTES = 16 * 1024;

    std::ifstream file(path, std::ios::binary | std::ios::ate);

    if (!file)
    {
        return {};
    }

    const std::streamoff size = file.tellg();
    const std::streamoff offset = std::max<std::streamoff>(0, size - MAX_BYTES);
    file.seekg(offset);
    const std::string text(std::istreambuf_iterator<char>(file), {});

    std::vector<std::string> lines;
    std::string line;

    auto flush = [&]
    {
        if (line.find_first_not_of(" \t") != std::string::npos)
        {
            lines.push_back(line);
        }

        line.clear();
    };

    for (size_t i = 0; i < text.size(); ++i)
    {
        const char c = text[i];

        if (c == '\n' || c == '\r')
        {
            flush();
        }
        else if (c == '\x1b' && i + 1 < text.size() && text[i + 1] == '[')
        {
            // エスケープシーケンスを読み飛ばす
            i += 2;

            while (i < text.size() && (text[i] < '@' || text[i] > '~'))
            {
                ++i;
            }
        }
        else if (static_cast<unsigned char>(c) >= 0x20 && c != '\x7f')
        {
            line += c;
        }
    }

    flush();

    if (lines.size() > static_cast<size_t>(count))
    {
        lines.erase(lines.begin(), lines.end() - count);
    }

    return lines;
}

std::string Shorten(std::string text)
{
    if (text.size() <= MAX_DETAIL_BYTES)
    {
        return text;
    }

    // UTF-8 の文字の途中で切らない
    size_t end = MAX_DETAIL_BYTES;

    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)
    {
        --end;
    }

    text.resize(end);
    return text + "…";
}

fs::path LogPath(const Slot& slot)
{
    return LogDirectory() / (std::string(slot.fileName) + ".log");
}

// --------------------------------------------------
// アプリが異常終了したときの後始末
// --------------------------------------------------

#if defined(_WIN32)

// アプリが終わると一緒に終わるように、ジョブに入れる
void AttachToAppLifetime(SDL_Process* process)
{
    static HANDLE job = []
    {
        HANDLE handle = CreateJobObjectW(nullptr, nullptr);

        if (handle)
        {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
            info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(
                handle, JobObjectExtendedLimitInformation, &info, sizeof(info));
        }

        return handle;
    }();

    const auto pid = static_cast<DWORD>(SDL_GetNumberProperty(
        SDL_GetProcessProperties(process), SDL_PROP_PROCESS_PID_NUMBER, 0));

    if (!job || pid == 0)
    {
        return;
    }

    HANDLE handle = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, pid);

    if (handle)
    {
        AssignProcessToJobObject(job, handle);
        CloseHandle(handle);
    }
}

void RememberProcess(const Slot&, SDL_Process* process)
{
    AttachToAppLifetime(process);
}

void ForgetProcess(const Slot&)
{
}

void CleanUpOrphan(const Slot&)
{
}

#else

// macOS には親の終了を子に知らせる仕組みが無いので、Linux も含めて PID ファイルで片付ける
// 中身は「サーバーの PID アプリの PID」(Python 版と同じ形式)
fs::path PidFile(const Slot& slot)
{
    return Utf8ToPath(UserDataDirectoryPath()) / "run" / (std::string(slot.fileName) + ".pid");
}

void RememberProcess(const Slot& slot, SDL_Process* process)
{
    const auto pid = SDL_GetNumberProperty(
        SDL_GetProcessProperties(process), SDL_PROP_PROCESS_PID_NUMBER, 0);

    std::error_code error;
    fs::create_directories(PidFile(slot).parent_path(), error);

    std::ofstream file(PidFile(slot), std::ios::trunc);
    file << pid << ' ' << getpid() << '\n';
}

void ForgetProcess(const Slot& slot)
{
    std::error_code error;
    fs::remove(PidFile(slot), error);
}

bool IsAlive(pid_t pid)
{
    return kill(pid, 0) == 0 || errno == EPERM;
}

// その PID のプロセスが llama-server か (PID は再利用されるので確かめる)
bool IsLlamaServer(pid_t pid)
{
    const std::string command = "ps -p " + std::to_string(pid) + " -o comm=";
    FILE* pipe = popen(command.c_str(), "r");

    if (!pipe)
    {
        return false;
    }

    char buffer[1024] = {};
    const bool read = fgets(buffer, sizeof(buffer), pipe) != nullptr;
    pclose(pipe);

    return read && std::string_view(buffer).find("llama-server") != std::string_view::npos;
}

// 前回のアプリが異常終了して残ったサーバーを止める
// (アプリがまだ生きていれば、C++ 版と Python 版の同時起動なので触らない)
void CleanUpOrphan(const Slot& slot)
{
    std::ifstream file(PidFile(slot));
    pid_t server = 0;
    pid_t owner = 0;

    if (!(file >> server >> owner) || server <= 0 || owner <= 0)
    {
        ForgetProcess(slot);
        return;
    }

    if (IsAlive(owner))
    {
        return;
    }

    if (IsAlive(server) && IsLlamaServer(server))
    {
        SDL_Log("Stopping orphaned %s (PID %d)", slot.fileName, static_cast<int>(server));
        kill(server, SIGTERM);

        const auto deadline = Clock::now() + STOP_TIMEOUT;

        while (IsAlive(server) && Clock::now() < deadline)
        {
            SDL_Delay(50);
        }

        if (IsAlive(server))
        {
            kill(server, SIGKILL);
        }
    }

    ForgetProcess(slot);
}

#endif

// --------------------------------------------------
// 状態の公開
// --------------------------------------------------

bool SameView(const ServerView& a, const ServerView& b)
{
    return a.status == b.status && a.detail == b.detail && a.logTail == b.logTail;
}

ServerView& PublicView(const Slot& slot)
{
    return &slot == &chatSlot ? manager.view.chat : manager.view.embed;
}

void Publish(Slot& slot)
{
    {
        std::lock_guard lock(manager.mutex);
        ServerView& view = PublicView(slot);

        if (SameView(view, slot.view))
        {
            return;
        }

        view = slot.view;
    }

    manager.changed.notify_all();
    WakeUI();
}

void SetView(Slot& slot, ServerStatus status, std::string detail, std::string logTail = "")
{
    slot.view.status = status;
    slot.view.detail = std::move(detail);
    slot.view.logTail = std::move(logTail);
    Publish(slot);
}

// --------------------------------------------------
// サーバーの起動と停止
// --------------------------------------------------

Health ProbeHealth(int port)
{
    CURL* curl = curl_easy_init();

    if (!curl)
    {
        return Health::None;
    }

    const std::string url = "http://127.0.0.1:" + std::to_string(port) + "/health";

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 500L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 1500L);
    curl_easy_setopt(
        curl, CURLOPT_WRITEFUNCTION,
        +[](char*, size_t size, size_t count, void*) { return size * count; });

    const CURLcode result = curl_easy_perform(curl);

    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);

    if (result != CURLE_OK)
    {
        return Health::None;
    }

    return status == 200 ? Health::Ready
        : status == 503 ? Health::Loading
        : Health::None;
}

// 自分が起動したサーバーを止める (まとめて止めて、待つ時間を 1 回分にする)
void StopProcesses(const std::vector<Slot*>& slots)
{
    std::vector<Slot*> running;

    for (Slot* slot : slots)
    {
        if (slot->process)
        {
            SDL_KillProcess(slot->process, false);
            running.push_back(slot);
        }
    }

    const auto deadline = Clock::now() + STOP_TIMEOUT;
    bool forced = false;

    while (!running.empty())
    {
        std::erase_if(running, [](Slot* slot)
        {
            if (!SDL_WaitProcess(slot->process, false, nullptr))
            {
                return false;
            }

            SDL_DestroyProcess(slot->process);
            slot->process = nullptr;
            ForgetProcess(*slot);
            return true;
        });

        if (!forced && Clock::now() >= deadline)
        {
            for (Slot* slot : running)
            {
                SDL_KillProcess(slot->process, true);
            }

            forced = true;
        }

        if (!running.empty())
        {
            SDL_Delay(20);
        }
    }
}

// 使う llama-server を探して、動くことを確かめる (結果は設定が変わるまで使い回す)
const FoundBinary& ResolveBinary(const std::string& serverPath)
{
    if (foundBinary && foundBinary->error.empty())
    {
        return *foundBinary;
    }

    FoundBinary found = FindBinary(serverPath);

    if (found.error.empty())
    {
        const CheckResult version = RunCheck(found.path, "--version");

        if (!version.finished || version.exitCode != 0)
        {
            found.error = "llama-server を実行できません: " + found.path;
        }
    }

    if (found.error.empty() && found.path != verifiedPath)
    {
        verifiedPath = found.path;
        gpuWarning = HasGpuDevice(RunCheck(found.path, "--list-devices").output)
            ? ""
            : "GPU が見つかりません。CPU で動くので遅くなります";
    }

    foundBinary = std::move(found);

    {
        std::lock_guard lock(manager.mutex);
        manager.view.binaryPath = foundBinary->path;
        manager.view.binarySource = foundBinary->source;
        manager.view.warning = foundBinary->error.empty() ? gpuWarning : "";
    }

    WakeUI();
    return *foundBinary;
}

void Launch(Slot& slot)
{
    const FoundBinary& binary = ResolveBinary(slot.spec.serverPath);

    if (!binary.error.empty())
    {
        SetView(slot, ServerStatus::Failed, binary.error);
        return;
    }

    fs::create_directories(LogDirectory());
    const std::string logPath = PathToUtf8(LogPath(slot));

    SDL_IOStream* log = SDL_IOFromFile(logPath.c_str(), "w");

    if (!log)
    {
        SetView(slot, ServerStatus::Failed, "ログを開けません: " + logPath);
        return;
    }

    std::vector<std::string> args = {binary.path};
    args.insert(args.end(), slot.spec.args.begin(), slot.spec.args.end());

    slot.process = CreateProcess(args, log);
    SDL_CloseIO(log);

    if (!slot.process)
    {
        SetView(
            slot, ServerStatus::Failed,
            std::string("llama-server を起動できません: ") + SDL_GetError());
        return;
    }

    RememberProcess(slot, slot.process);
    SetView(slot, ServerStatus::Starting, "起動中");
}

void Step(Slot& slot)
{
    if (!slot.spec.run)
    {
        StopProcesses({&slot});
        SetView(slot, ServerStatus::Off, "");
        return;
    }

    if (slot.process)
    {
        int exitCode = 0;

        if (SDL_WaitProcess(slot.process, false, &exitCode))
        {
            SDL_DestroyProcess(slot.process);
            slot.process = nullptr;
            ForgetProcess(slot);

            std::string tail;

            for (const auto& line : LastLines(LogPath(slot), LOG_TAIL_LINES))
            {
                tail += line + "\n";
            }

            SetView(
                slot, ServerStatus::Failed,
                "llama-server が終了しました (コード " + std::to_string(exitCode) + ")",
                std::move(tail));
            return;
        }

        if (slot.view.status == ServerStatus::Ready)
        {
            return;
        }

        if (ProbeHealth(slot.spec.port) == Health::Ready)
        {
            SetView(slot, ServerStatus::Ready, "");
            return;
        }

        // モデルのダウンロード中は /health が応答しないことがあるので、
        // 時間では打ち切らず、プロセスが生きている間は待つ
        const auto lines = LastLines(LogPath(slot), 1);
        SetView(
            slot, ServerStatus::Starting,
            lines.empty() ? "起動中" : Shorten(lines.back()));
        return;
    }

    if (slot.view.status == ServerStatus::Failed)
    {
        return;
    }

    // すでに動いているサーバーがあれば、それを使う
    switch (ProbeHealth(slot.spec.port))
    {
    case Health::Ready:
        SetView(slot, ServerStatus::External, "すでに動いているサーバーを使っています");
        return;

    case Health::Loading:
        SetView(slot, ServerStatus::Starting, "すでに動いているサーバーが読み込み中です");
        return;

    case Health::None:
        break;
    }

    Launch(slot);
}

void ApplySettings()
{
    const json server = LoadConfigOrEmpty(SERVER_CONFIG);
    const ManagedSettings settings = LoadManagedSettings();

    const ServerSpec chat = MakeSpec(
        settings, ConfigStringOr(server, "chat_url", ""), settings.chatModel,
        {"-c", std::to_string(settings.context), "--alias", "local"});

    const std::string embedContext = std::to_string(settings.embedContext);
    const ServerSpec embed = MakeSpec(
        settings, ConfigStringOr(server, "embed_url", ""), settings.embedModel,
        {
            "--embeddings",
            "-c", embedContext, "-b", embedContext, "-ub", embedContext,
            "--alias", "embed",
        });

    if (chat.serverPath != chatSlot.spec.serverPath ||
        embed.serverPath != embedSlot.spec.serverPath)
    {
        foundBinary.reset();
    }

    std::vector<Slot*> changed;

    for (auto [slot, spec] : {std::pair{&chatSlot, &chat}, std::pair{&embedSlot, &embed}})
    {
        if (slot->spec != *spec)
        {
            changed.push_back(slot);
            slot->spec = *spec;
        }
    }

    StopProcesses(changed);

    for (Slot* slot : changed)
    {
        slot->view.status = ServerStatus::Off;
        slot->view.detail.clear();
        slot->view.logTail.clear();
    }

    std::lock_guard lock(manager.mutex);
    manager.view.enabled = settings.enabled;
}

void Run()
{
    {
        std::lock_guard lock(manager.mutex);

        try
        {
            manager.view.logDirectory = PathToUtf8(LogDirectory());
        }
        catch (const std::exception& e)
        {
            SDL_Log("%s", e.what());
        }
    }

    bool first = true;

    while (true)
    {
        bool reload = false;
        bool retry = false;

        {
            std::unique_lock lock(manager.mutex);

            manager.changed.wait_for(lock, std::chrono::seconds(1), []
            {
                return manager.stop || manager.reload || manager.retry;
            });

            if (manager.stop)
            {
                break;
            }

            reload = manager.reload;
            retry = manager.retry;
            manager.reload = false;
            manager.retry = false;
        }

        try
        {
            if (first)
            {
                CleanUpOrphan(chatSlot);
                CleanUpOrphan(embedSlot);
                first = false;
            }

            if (reload)
            {
                ApplySettings();
            }

            if (retry)
            {
                foundBinary.reset();

                for (Slot* slot : {&chatSlot, &embedSlot})
                {
                    if (slot->view.status == ServerStatus::Failed)
                    {
                        slot->view.status = ServerStatus::Off;
                    }
                }
            }
        }
        catch (const std::exception& e)
        {
            SDL_Log("Server manager: %s", e.what());
        }

        for (Slot* slot : {&chatSlot, &embedSlot})
        {
            try
            {
                Step(*slot);
            }
            catch (const std::exception& e)
            {
                SetView(*slot, ServerStatus::Failed, e.what());
            }
        }

        {
            std::lock_guard lock(manager.mutex);

            // 待っている間に読み直しを頼まれていたら、次の周で反映してから知らせる
            if (!manager.reload)
            {
                manager.settled = true;
            }
        }

        manager.changed.notify_all();
        WakeUI();
    }

    StopProcesses({&chatSlot, &embedSlot});
}

} // namespace

ManagedSettings LoadManagedSettings()
{
    ManagedSettings settings;
    const json server = LoadConfigOrEmpty(SERVER_CONFIG);

    if (!server.contains("managed") || !server["managed"].is_object())
    {
        return settings;
    }

    const json& managed = server["managed"];

    settings.enabled = managed.value("enabled", false);
    settings.serverPath = ConfigStringOr(managed, "server_path", settings.serverPath);
    settings.chatModel = ConfigStringOr(managed, "chat_model", settings.chatModel);
    settings.embedModel = ConfigStringOr(managed, "embed_model", settings.embedModel);
    settings.context = static_cast<int>(ConfigNumberOr(
        managed, "context", static_cast<float>(settings.context)));
    settings.embedContext = static_cast<int>(ConfigNumberOr(
        managed, "embed_context", static_cast<float>(settings.embedContext)));
    settings.gpuLayers = static_cast<int>(ConfigNumberOr(
        managed, "gpu_layers", static_cast<float>(settings.gpuLayers)));

    return settings;
}

const char* ServerStatusText(ServerStatus status)
{
    switch (status)
    {
    case ServerStatus::Off:
        return "起動しない";
    case ServerStatus::Starting:
        return "ダウンロードまたは読み込み中";
    case ServerStatus::Ready:
        return "使えます";
    case ServerStatus::External:
        return "使えます (外部のサーバー)";
    case ServerStatus::Failed:
        return "失敗";
    }

    return "";
}

void StartServerManager()
{
    manager.worker = std::thread(Run);
}

void StopServerManager()
{
    {
        std::lock_guard lock(manager.mutex);
        manager.stop = true;
    }

    manager.changed.notify_all();

    if (manager.worker.joinable())
    {
        manager.worker.join();
    }
}

void ReloadServerManager()
{
    {
        std::lock_guard lock(manager.mutex);
        manager.reload = true;
        manager.settled = false;
    }

    manager.changed.notify_all();
}

void RetryServers()
{
    {
        std::lock_guard lock(manager.mutex);
        manager.retry = true;
    }

    manager.changed.notify_all();
}

ServerManagerView GetServerManagerView()
{
    std::lock_guard lock(manager.mutex);
    return manager.view;
}

bool IsChatServerAvailable()
{
    std::lock_guard lock(manager.mutex);

    return manager.settled &&
        manager.view.chat.status != ServerStatus::Starting &&
        manager.view.chat.status != ServerStatus::Failed;
}

void WaitForEmbedServer(const std::atomic<bool>& cancel)
{
    std::unique_lock lock(manager.mutex);

    while (true)
    {
        if (cancel.load())
        {
            throw std::runtime_error("Cancelled");
        }

        if (manager.settled)
        {
            const ServerView& embed = manager.view.embed;

            if (embed.status == ServerStatus::Failed)
            {
                throw std::runtime_error("埋め込みサーバーを起動できません: " + embed.detail);
            }

            if (embed.status != ServerStatus::Starting)
            {
                return;
            }
        }

        manager.changed.wait_for(lock, std::chrono::milliseconds(200));
    }
}
