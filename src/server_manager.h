#pragma once

#include <atomic>
#include <string>

// llama-server (チャット用と埋め込み用) を子プロセスとして起動・監視・停止する
// 設定は config/server.json の managed。専用のスレッドで動く

enum class ServerStatus
{
    Off,       // 起動しない (自動起動がオフ、URL がローカルでない、モデルが空)
    Starting,  // ダウンロードまたは読み込み中
    Ready,     // 自分で起動したサーバーが使える
    External,  // すでに動いているサーバーを使っている (止めない)
    Failed     // 起動できなかった、または落ちた
};

struct ServerView
{
    ServerStatus status = ServerStatus::Off;

    // 今の様子 (ログの最後の行や、失敗の理由)
    std::string detail;

    // 失敗したときのログの末尾
    std::string logTail;
};

// 画面に出す状態
struct ServerManagerView
{
    bool enabled = false;
    ServerView chat;
    ServerView embed;

    // 使っている llama-server と、見つけた経路
    std::string binaryPath;
    std::string binarySource;

    // GPU が見つからないなどの注意
    std::string warning;

    // ログのフォルダー
    std::string logDirectory;
};

// server.json の managed (書いていない項目は既定値)
struct ManagedSettings
{
    // 前の版の設定ファイルをそのまま読めるように、managed が無ければ自動起動しない
    bool enabled = false;

    std::string serverPath;
    std::string chatModel = "unsloth/Qwen3-4B-Instruct-2507-GGUF:Q4_K_M";

    // 空なら埋め込みサーバーを起動しない
    std::string embedModel = "ggml-org/bge-m3-Q8_0-GGUF";

    int context = 4096;
    int embedContext = 8192;
    int gpuLayers = 99;
};

ManagedSettings LoadManagedSettings();

const char* ServerStatusText(ServerStatus status);

// 管理用のスレッドを起動する (UI スレッドから、SDL_Init と RegisterWakeEvent の後に 1 回呼ぶ)
void StartServerManager();

// 自分が起動したサーバーを止めて、スレッドを終える (数秒かかることがある)
void StopServerManager();

// server.json を読み直す。変わったサーバーだけを起動し直す
void ReloadServerManager();

// 起動に失敗したサーバーを、もう一度起動する
void RetryServers();

ServerManagerView GetServerManagerView();

// チャットのサーバーに質問を送ってよいか (読み込み中や失敗のときは false)
bool IsChatServerAvailable();

// 埋め込みサーバーが使えるようになるまで待つ
// 起動に失敗していたら、cancel が true になったら、std::runtime_error を投げる
void WaitForEmbedServer(const std::atomic<bool>& cancel);
