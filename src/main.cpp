// アプリの入口: ウィンドウと ImGui の初期化、メインループ、終了処理

#include "chat/chat.h"
#include "chat/chat_ui.h"
#include "quiz/quiz_mode.h"
#include "maze/maze_mode.h"
#include "common/config.h"
#include "common/font.h"
#include "common/ime.h"
#include "common/rag.h"
#include "common/server_manager.h"
#include "common/settings_ui.h"

#include <SDL3/SDL.h>
#include <curl/curl.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#if defined(__APPLE__)
#include "common/macos_window.h"
#endif

#include <string>

int main(int, char**)
{
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
    {
        SDL_Log("curl initialization failed");
        return 1;
    }

    SetImeHint();

    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    RegisterWakeEvent();

    const float scale =
        SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());

    SDL_Window* window = SDL_CreateWindow(
        "Local LLM Chat",
        static_cast<int>(900 * scale),
        static_cast<int>(700 * scale),
        SDL_WINDOW_RESIZABLE |
        SDL_WINDOW_HIDDEN |
        SDL_WINDOW_TRANSPARENT |
        SDL_WINDOW_HIGH_PIXEL_DENSITY);

    if (!window)
    {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        return 1;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);

    if (!renderer)
    {
        SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
        return 1;
    }

#if defined(__APPLE__)
    ApplyMacTitleBarStyle(window);
#endif

    SDL_SetRenderVSync(renderer, 1);
    SDL_SetWindowPosition(
        window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;

    // 画面の設定は config/ui.json に書く
    SettingsState settings;
    settings.current = ReadUiSettings(LoadConfigOrEmpty("ui.json"));

    LoadFont(io, settings.current.fontFile, settings.current.fontSize);
    ApplyUiSettings(settings.current);

    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    InstallImeHook();

    ChatState state;
    RefreshCloudStatus(state);
    RagState rag;
    QuizState quiz;
    MazeState maze;
    std::string input;
    bool focusInput = true;

    // server.json の managed がオンなら llama-server を起動する
    StartServerManager();

    // rag/ の資料のインデックスをバックグラウンドで作る
    StartRagIndexing(rag);

    // 入力後しばらくは毎フレーム描画し、それ以外はイベント待ちで省電力
    int framesToRender = 3;
    bool done = false;

    while (!done)
    {
        SDL_Event event;

        bool hasEvent = framesToRender > 0
            ? SDL_PollEvent(&event)
            : SDL_WaitEventTimeout(&event, 500);

        while (hasEvent)
        {
            ImGui_ImplSDL3_ProcessEvent(&event);
            HandleCompositionEvent(event);

            if (event.type == SDL_EVENT_QUIT)
            {
                done = true;
            }

            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                event.window.windowID == SDL_GetWindowID(window))
            {
                done = true;
            }

            framesToRender = 3;
            hasEvent = SDL_PollEvent(&event);
        }

        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED)
        {
            SDL_Delay(10);
            continue;
        }

        // 1フレーム描画する
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

#if defined(__APPLE__)
        const float topInset = GetMacTitleBarHeight(window);
#else
        const float topInset = 0.0f;
#endif

        DrawUI(state, rag, quiz, maze, settings, input, focusInput, topInset);

        ImGui::Render();
        SDL_SetRenderScale(
            renderer,
            io.DisplayFramebufferScale.x,
            io.DisplayFramebufferScale.y);
        // 透明でクリアし、ImGuiの背景色だけを半透明で重ねる
        SDL_SetRenderDrawColorFloat(renderer, 0.0f, 0.0f, 0.0f, 0.0f);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(
            ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);

        if (framesToRender > 0)
        {
            --framesToRender;
        }
    }

    // 終了処理: 受信とインデックス作成を中断して、スレッドの終了を待つ
    state.cancel = true;
    JoinWorker(state);

    rag.cancel = true;
    JoinRagWorker(rag);

    ShutdownQuiz(quiz);
    ShutdownMaze(maze);

    // サーバーの停止に数秒かかることがあるので、先にウィンドウを閉じる
    SDL_HideWindow(window);
    StopServerManager();

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    curl_global_cleanup();

    return 0;
}
