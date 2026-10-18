#include "llm_client.h"

#include <SDL3/SDL.h>
#include <curl/curl.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>
#include <imgui_stdlib.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

// 同梱フォント (実行ファイルからの相対パス)
constexpr const char* BUNDLED_FONT_PATH =
    "Asset/MPLUSRounded1c-Bold.ttf";

// OSの日本語フォントの候補 (同梱フォントに無い文字の補完用)
constexpr const char* SYSTEM_FONT_PATHS[] = {
#if defined(_WIN32)
    "C:/Windows/Fonts/YuGothM.ttc",
    "C:/Windows/Fonts/meiryo.ttc",
    "C:/Windows/Fonts/msgothic.ttc",
#elif defined(__APPLE__)
    "/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc",
    "/System/Library/Fonts/Hiragino Sans GB.ttc",
#else
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
#endif
};

constexpr float FONT_SIZE = 18.0f;

// チャット画面の背景の不透明度 (文字は不透明のまま)
constexpr float WINDOW_OPACITY = 0.85f;

struct Message {
    std::string role;
    std::string content;
};

// UIスレッドとワーカースレッドで共有する会話状態
struct ChatState {
    std::mutex mutex;

    // systemプロンプトを除いた会話 (user / assistant)
    std::vector<Message> messages;

    bool busy = false;
    std::string error;

    // 失敗時に入力欄へ戻す質問
    std::string restoredPrompt;

    std::atomic<bool> cancel = false;
    std::thread worker;
};

// ワーカースレッドからUIスレッドを起こすためのイベント
Uint32 g_wakeEvent = 0;

void WakeUI()
{
    SDL_Event event{};
    event.type = g_wakeEvent;
    SDL_PushEvent(&event);
}

// フォントを読み込む (パスはUTF-8)
bool TryLoadFont(ImGuiIO& io, const char* path, bool merge = false)
{
    ImFontConfig config;
    config.MergeMode = merge;

    return SDL_GetPathInfo(path, nullptr) &&
        io.Fonts->AddFontFromFileTTF(path, FONT_SIZE, &config);
}

// 日本語フォントを読み込む
// LOCALLLM_FONT → 同梱フォント → OSのフォント の順に探し、
// 足りない文字はOSのフォントで補う
void LoadFont(ImGuiIO& io)
{
    bool loaded = false;

    if (const char* env = SDL_getenv("LOCALLLM_FONT")) {
        loaded = TryLoadFont(io, env);

        if (!loaded)
            SDL_Log("Font not found: %s", env);
    }

    if (!loaded) {
        const char* basePath = SDL_GetBasePath();
        const std::string bundled =
            std::string(basePath ? basePath : "") + BUNDLED_FONT_PATH;

        loaded = TryLoadFont(io, bundled.c_str());

        if (!loaded)
            SDL_Log("Font not found: %s", bundled.c_str());
    }

    // 先に読めたフォントがあれば、OSのフォントは補完として合成する
    for (const char* path : SYSTEM_FONT_PATHS) {
        if (TryLoadFont(io, path, loaded))
            return;
    }

    if (!loaded) {
        SDL_Log("Japanese font not found (using default font)");
        io.Fonts->AddFontDefault();
    }
}

void JoinWorker(ChatState& state)
{
    if (state.worker.joinable())
        state.worker.join();
}

// 質問を送信してワーカースレッドで回答を受信する
void SendPrompt(ChatState& state, const std::string& prompt)
{
    JoinWorker(state);

    json history = CreateHistory();

    {
        std::lock_guard lock(state.mutex);

        state.messages.push_back({"user", prompt});

        // 履歴全体を送信
        for (const auto& message : state.messages) {
            history.push_back({
                {"role", message.role},
                {"content", message.content}
            });
        }

        // ストリーミングで埋めていくAIの回答
        state.messages.push_back({"assistant", ""});

        state.busy = true;
        state.error.clear();
    }

    state.cancel = false;

    state.worker = std::thread([&state, history, prompt] {
        try {
            AskLLM(
                history,
                [&state](std::string_view text) {
                    {
                        std::lock_guard lock(state.mutex);
                        state.messages.back().content += text;
                    }
                    WakeUI();
                },
                state.cancel);
        }
        catch (const std::exception& e) {
            std::lock_guard lock(state.mutex);

            const bool keepPartial =
                state.cancel && !state.messages.back().content.empty();

            if (!keepPartial) {
                // 失敗した質問は履歴から取り除き、入力欄に戻す
                state.messages.pop_back();
                state.messages.pop_back();
                state.restoredPrompt = prompt;
            }

            if (!state.cancel)
                state.error = e.what();
        }

        {
            std::lock_guard lock(state.mutex);
            state.busy = false;
        }
        WakeUI();
    });
}

void DrawMessages(ChatState& state)
{
    std::lock_guard lock(state.mutex);

    // 前フレームで一番下までスクロールしていたら追従する
    const bool atBottom =
        ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;

    for (size_t i = 0; i < state.messages.size(); ++i) {
        const auto& message = state.messages[i];
        const bool isUser = message.role == "user";

        ImGui::PushID(static_cast<int>(i));

        ImGui::TextColored(
            isUser ? ImVec4(0.45f, 0.75f, 1.0f, 1.0f)
                   : ImVec4(0.55f, 0.9f, 0.55f, 1.0f),
            "%s", isUser ? "You" : "AI");

        if (message.content.empty() && state.busy)
            ImGui::TextDisabled("考え中...");
        else
            ImGui::TextWrapped("%s", message.content.c_str());

        if (ImGui::BeginPopupContextItem("copy")) {
            if (ImGui::MenuItem("コピー"))
                ImGui::SetClipboardText(message.content.c_str());
            ImGui::EndPopup();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::PopID();
    }

    if (atBottom)
        ImGui::SetScrollHereY(1.0f);
}

void DrawUI(ChatState& state, std::string& input, bool& focusInput)
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);

    ImGui::Begin(
        "Chat", nullptr,
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings);

    bool busy;
    std::string error;
    {
        std::lock_guard lock(state.mutex);
        busy = state.busy;
        error = state.error;

        if (!state.restoredPrompt.empty()) {
            input = std::move(state.restoredPrompt);
            state.restoredPrompt.clear();
            focusInput = true;
        }
    }

    const ImGuiStyle& style = ImGui::GetStyle();
    const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
    const float inputHeight =
        lineHeight * 4.0f + style.FramePadding.y * 2.0f;
    const float errorHeight =
        error.empty() ? 0.0f : lineHeight + style.ItemSpacing.y;

    // 会話表示
    ImGui::BeginChild(
        "messages",
        ImVec2(0.0f, -(inputHeight + errorHeight + style.ItemSpacing.y)),
        ImGuiChildFlags_Borders);
    DrawMessages(state);
    ImGui::EndChild();

    if (!error.empty())
        ImGui::TextColored(
            ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Error: %s", error.c_str());

    // 入力欄 (Enterで送信 / Shift+Enterで改行)
    const float buttonWidth = ImGui::CalcTextSize("クリア").x * 2.5f;

    if (focusInput) {
        ImGui::SetKeyboardFocusHere();
        focusInput = false;
    }

    const bool entered = ImGui::InputTextMultiline(
        "##input", &input,
        ImVec2(-(buttonWidth + style.ItemSpacing.x), inputHeight),
        ImGuiInputTextFlags_EnterReturnsTrue |
        ImGuiInputTextFlags_CtrlEnterForNewLine |
        ImGuiInputTextFlags_WordWrap);

    ImGui::SameLine();
    ImGui::BeginGroup();

    const ImVec2 buttonSize(
        buttonWidth, (inputHeight - style.ItemSpacing.y) / 2.0f);

    bool send = false;

    if (busy) {
        if (ImGui::Button("中断", buttonSize))
            state.cancel = true;
    }
    else {
        send = ImGui::Button("送信", buttonSize) || entered;
    }

    ImGui::BeginDisabled(busy);
    if (ImGui::Button("クリア", buttonSize)) {
        std::lock_guard lock(state.mutex);
        state.messages.clear();
        state.error.clear();
        focusInput = true;
    }
    ImGui::EndDisabled();

    ImGui::EndGroup();

    if (entered)
        focusInput = true;

    if (send && input.find_first_not_of(" \t\r\n") != std::string::npos) {
        SendPrompt(state, input);
        input.clear();
        focusInput = true;
    }

    ImGui::End();
}

} // namespace

int main(int, char**)
{
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        SDL_Log("curl initialization failed");
        return 1;
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    g_wakeEvent = SDL_RegisterEvents(1);

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

    if (!window) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        return 1;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);

    if (!renderer) {
        SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
        return 1;
    }

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
    style.Colors[ImGuiCol_WindowBg].w = WINDOW_OPACITY;

    LoadFont(io);

    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    ChatState state;
    std::string input;
    bool focusInput = true;

    // 入力後しばらくは毎フレーム描画し、それ以外はイベント待ちで省電力
    int framesToRender = 3;
    bool done = false;

    while (!done) {
        SDL_Event event;

        bool hasEvent = framesToRender > 0
            ? SDL_PollEvent(&event)
            : SDL_WaitEventTimeout(&event, 500);

        while (hasEvent) {
            ImGui_ImplSDL3_ProcessEvent(&event);

            if (event.type == SDL_EVENT_QUIT)
                done = true;

            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                event.window.windowID == SDL_GetWindowID(window))
                done = true;

            framesToRender = 3;
            hasEvent = SDL_PollEvent(&event);
        }

        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
            SDL_Delay(10);
            continue;
        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        DrawUI(state, input, focusInput);

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
            --framesToRender;
    }

    // 受信中なら中断してから終了
    state.cancel = true;
    JoinWorker(state);

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    curl_global_cleanup();

    return 0;
}
