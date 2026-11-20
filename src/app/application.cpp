#include "app/application.h"

#include "chat/chat_ui.h"
#include "common/config.h"
#include "common/server_manager.h"

#include <curl/curl.h>

Application::~Application()
{
    Shutdown();
}

int Application::Run()
{
    if (!Init())
    {
        return 1;
    }

    while (ProcessEvents())
    {
        if (mRenderer.IsMinimized())
        {
            SDL_Delay(10);
            continue;
        }

        DrawFrame();

        if (mFramesToRender > 0)
        {
            --mFramesToRender;
        }
    }

    Shutdown();

    return 0;
}

bool Application::Init()
{
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
    {
        SDL_Log("curl initialization failed");
        return false;
    }

    mCurlInitialized = true;

    // 画面の設定は config/ui.json に書く
    mSettings.current = ReadUiSettings(LoadConfigOrEmpty("ui.json"));

    if (!mRenderer.Init(
            "Local LLM Chat", 900, 700,
            mSettings.current.fontFile, mSettings.current.fontSize))
    {
        return false;
    }

    RegisterWakeEvent();
    ApplyUiSettings(mSettings.current);
    RefreshCloudStatus(mChat);

    // server.json の managed がオンなら llama-server を起動する
    StartServerManager();

    // rag/ の資料のインデックスをバックグラウンドで作る
    StartRagIndexing(mRag);

    return true;
}

bool Application::ProcessEvents()
{
    SDL_Event event;

    bool hasEvent = mFramesToRender > 0
        ? SDL_PollEvent(&event)
        : SDL_WaitEventTimeout(&event, 500);

    bool running = true;

    while (hasEvent)
    {
        if (mRenderer.ProcessEvent(event))
        {
            running = false;
        }

        mFramesToRender = 3;
        hasEvent = SDL_PollEvent(&event);
    }

    return running;
}

void Application::DrawFrame()
{
    mRenderer.BeginFrame();

    DrawUI(
        mChat, mRag, mQuiz, mMaze, mSettings,
        mInput, mFocusInput, mRenderer.TopInset());

    mRenderer.EndFrame();
}

void Application::Shutdown()
{
    if (mShutDown)
    {
        return;
    }

    mShutDown = true;

    // 受信とインデックス作成を中断して、スレッドの終了を待つ
    mChat.cancel = true;
    JoinWorker(mChat);

    mRag.cancel = true;
    JoinRagWorker(mRag);

    ShutdownQuiz(mQuiz);
    ShutdownMaze(mMaze);

    // サーバーの停止に数秒かかることがあるので、先にウィンドウを閉じる
    mRenderer.Hide();
    StopServerManager();

    if (mCurlInitialized)
    {
        curl_global_cleanup();
        mCurlInitialized = false;
    }
}
