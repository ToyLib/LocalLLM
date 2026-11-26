#include "app/application.h"

#include "chat/chat.h"
#include "chat/chat_ui.h"
#include "maze/maze_ui.h"
#include "quiz/quiz_ui.h"
#include "sandbox/sandbox_ui.h"
#include "common/config.h"
#include "common/server_manager.h"
#include "common/widgets.h"

#include <curl/curl.h>
#include <imgui.h>

namespace
{

// llama-server が使えないときだけ、状態を 1 行ずつ出す
void DrawServerLine(const char* name, const ServerView& view, const UiColors& colors, bool& retry)
{
    if (view.status == ServerStatus::Starting)
    {
        ui::TextWrap wrap;
        ImGui::TextDisabled(
            "%s: %s (%s)", name, ServerStatusText(view.status), view.detail.c_str());
    }
    else if (view.status == ServerStatus::Failed)
    {
        {
            ui::TextWrap wrap;
            ImGui::TextColored(colors.error, "%s: %s", name, view.detail.c_str());
        }

        if (!view.logTail.empty())
        {
            ImGui::SetItemTooltip("%s", view.logTail.c_str());
        }

        ui::Id id(name);
        retry |= ImGui::SmallButton("もう一度起動");
    }
}

void DrawServerBar(const UiColors& colors)
{
    const ServerManagerView view = GetServerManagerView();

    if (!view.enabled)
    {
        return;
    }

    bool retry = false;
    DrawServerLine("チャットのサーバー", view.chat, colors, retry);
    DrawServerLine("埋め込みのサーバー", view.embed, colors, retry);

    if (retry)
    {
        RetryServers();
    }
}

} // namespace

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

    // タブの並び順 (先頭のタブを最初に開く)
    mTabs.push_back(std::make_unique<ChatTab>(mRag));
    mTabs.push_back(std::make_unique<QuizTab>(mRag));
    mTabs.push_back(std::make_unique<MazeTab>());
    mTabs.push_back(std::make_unique<SandboxTab>());

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
        : SDL_WaitEventTimeout(&event, mTabAnimating ? 16 : 500);

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
    DrawMainWindow();
    mRenderer.EndFrame();
}

void Application::DrawMainWindow()
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);

    ImGui::Begin(
        "Chat", nullptr,
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings);

    const UiContext context{ActiveUiSettings(mSettings).colors};

    // 背景はタイトルバーの下まで塗り、中身はその下から並べる
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + mRenderer.TopInset());

    // サーバーの状態はどのタブでも見えるように、タブの外 (上) に置く
    DrawServerBar(context.colors);

    // 設定ボタンはどのタブからでも開けるように、タブの行の右端に重ねる
    {
        const float buttonWidth = ui::ButtonWidth("設定");
        const ImVec2 tabsPos = ImGui::GetCursorPos();

        ImGui::SetCursorPos(ImVec2(
            ImGui::GetContentRegionMax().x - buttonWidth, tabsPos.y));

        if (ImGui::SmallButton("設定"))
        {
            mSettings.requestOpen = true;
        }

        ImGui::SetCursorPos(tabsPos);
    }

    mTabAnimating = false;

    // 各モードはタブで切り替える
    // (閉じているタブの状態も、ワーカースレッドで動き続ける)
    if (ImGui::BeginTabBar("tabs"))
    {
        for (size_t i = 0; i < mTabs.size(); ++i)
        {
            const ImGuiTabItemFlags flags = mFirstFrame && i == 0
                ? ImGuiTabItemFlags_SetSelected
                : ImGuiTabItemFlags_None;

            if (ImGui::BeginTabItem(mTabs[i]->Label(), nullptr, flags))
            {
                mTabs[i]->Draw(context);
                mTabAnimating = mTabs[i]->IsAnimating();
                ImGui::EndTabItem();
            }
        }

        mFirstFrame = false;
        ImGui::EndTabBar();
    }

    DrawSettings(mSettings);

    // 設定を保存したら、タブに設定を読み直させ、サーバーの設定も読み直す
    if (mSettings.saved)
    {
        mSettings.saved = false;

        for (const auto& tab : mTabs)
        {
            tab->OnSettingsSaved();
        }

        ReloadServerManager();
    }

    ImGui::End();
}

void Application::Shutdown()
{
    if (mShutDown)
    {
        return;
    }

    mShutDown = true;

    // 受信とインデックス作成を中断して、スレッドの終了を待つ
    for (const auto& tab : mTabs)
    {
        tab->Shutdown();
    }

    mRag.cancel = true;
    JoinRagWorker(mRag);

    // サーバーの停止に数秒かかることがあるので、先にウィンドウを閉じる
    mRenderer.Hide();
    StopServerManager();

    if (mCurlInitialized)
    {
        curl_global_cleanup();
        mCurlInitialized = false;
    }
}
