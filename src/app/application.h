#pragma once

#include "app/renderer.h"
#include "chat/chat.h"
#include "maze/maze_mode.h"
#include "quiz/quiz_mode.h"
#include "common/rag.h"
#include "common/settings_ui.h"

#include <string>

// アプリ本体: 状態とバックグラウンドの仕事 (サーバー管理・RAG・ワーカー) を持ち、
// メインループを回す。ウィンドウと ImGui の細かいことは Renderer に任せる
class Application
{
public:
    Application() = default;
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    // 初期化してメインループを回す。終わったら終了コード (0 = 正常)
    int Run();

private:
    bool Init();

    // イベントを処理する。ウィンドウが閉じられたら false
    bool ProcessEvents();

    void DrawFrame();

    // 受信とインデックス作成を中断してスレッドを待ち、サーバーを止める
    void Shutdown();

    Renderer mRenderer;

    ChatState mChat;
    RagState mRag;
    QuizState mQuiz;
    MazeState mMaze;
    SettingsState mSettings;

    // チャットの入力欄
    std::string mInput;
    bool mFocusInput = true;

    // 入力後しばらくは毎フレーム描画し、それ以外はイベント待ちで省電力
    int mFramesToRender = 3;

    bool mCurlInitialized = false;
    bool mShutDown = false;
};
