#include "chat_ui.h"

#include "ime.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>

namespace
{

void DrawMessages(ChatState& state, const UiColors& colors)
{
    std::lock_guard lock(state.mutex);

    // 前フレームで一番下までスクロールしていたら追従する
    const bool atBottom =
        ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;

    for (size_t i = 0; i < state.messages.size(); ++i)
    {
        const auto& message = state.messages[i];
        const bool isUser = message.role == "user";

        ImGui::PushID(static_cast<int>(i));

        // AI の回答には、答えたエージェントの名前を出す
        const char* name = isUser ? "You"
            : message.agent.empty() ? "AI"
            : message.agent.c_str();

        ImGui::TextColored(
            isUser ? colors.user : colors.ai,
            "%s", name);

        if (message.content.empty() && state.busy)
        {
            ImGui::TextDisabled("考え中...");
        }
        else
        {
            ImGui::TextWrapped("%s", message.content.c_str());
        }

        if (!message.note.empty())
        {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s", message.note.c_str());
            ImGui::PopTextWrapPos();
        }

        if (ImGui::BeginPopupContextItem("copy"))
        {
            if (ImGui::MenuItem("コピー"))
            {
                ImGui::SetClipboardText(message.content.c_str());
            }
            ImGui::EndPopup();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::PopID();
    }

    if (atBottom)
    {
        ImGui::SetScrollHereY(1.0f);
    }
}

// RAG のオン/オフとインデックスの状態
void DrawRagBar(ChatState& state, RagState& rag, SettingsState& settings)
{
    bool building;
    std::string status;
    {
        std::lock_guard lock(rag.mutex);
        building = rag.building;
        status = rag.status;
    }

    // 資料を添えるか (自動ならルーターが質問ごとに判断する)
    ImGui::TextUnformatted("資料 (RAG):");

    ImGui::SameLine();
    if (ImGui::RadioButton("自動", state.ragMode == RagMode::Auto))
    {
        state.ragMode = RagMode::Auto;
    }

    ImGui::SameLine();
    if (ImGui::RadioButton("常に", state.ragMode == RagMode::Always))
    {
        state.ragMode = RagMode::Always;
    }

    ImGui::SameLine();
    if (ImGui::RadioButton("使わない", state.ragMode == RagMode::Never))
    {
        state.ragMode = RagMode::Never;
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(building);
    if (ImGui::SmallButton("再読込"))
    {
        StartRagIndexing(rag);
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::TextDisabled("%s", status.c_str());

    // 設定ボタンは右端
    const float buttonWidth = ImGui::CalcTextSize("設定").x +
        ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SameLine(std::max(
        ImGui::GetCursorPosX(),
        ImGui::GetContentRegionMax().x - buttonWidth));

    if (ImGui::SmallButton("設定"))
    {
        settings.requestOpen = true;
    }
}

} // namespace

void DrawUI(
    ChatState& state,
    RagState& rag,
    SettingsState& settings,
    std::string& input,
    bool& focusInput,
    float topInset)
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

        if (!state.restoredPrompt.empty())
        {
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

    // 背景はタイトルバーの下まで塗り、中身はその下から並べる
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + topInset);

    DrawRagBar(state, rag, settings);

    // 会話表示 (高さは残りから入力欄などの分を引いたもの)
    ImGui::BeginChild(
        "messages",
        ImVec2(0.0f, -(inputHeight + errorHeight + style.ItemSpacing.y)),
        ImGuiChildFlags_Borders);
    DrawMessages(state, ActiveUiSettings(settings).colors);
    ImGui::EndChild();

    if (!error.empty())
    {
        ImGui::TextColored(
            ActiveUiSettings(settings).colors.error, "Error: %s", error.c_str());
    }

    // 入力欄 (Enterで送信 / Shift+Enterで改行)
    const float buttonWidth = ImGui::CalcTextSize("クリア").x * 2.5f;

    if (focusInput)
    {
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

    if (busy)
    {
        if (ImGui::Button("中断", buttonSize))
        {
            state.cancel = true;
        }
    }
    else
    {
        send = ImGui::Button("送信", buttonSize) || entered;
    }

    ImGui::BeginDisabled(busy);
    if (ImGui::Button("クリア", buttonSize))
    {
        std::lock_guard lock(state.mutex);
        state.messages.clear();
        state.error.clear();
        focusInput = true;
    }
    ImGui::EndDisabled();

    ImGui::EndGroup();

    DrawComposition();

    if (entered)
    {
        focusInput = true;
    }

    if (send && input.find_first_not_of(" \t\r\n") != std::string::npos)
    {
        SendPrompt(state, rag, input);
        input.clear();
        focusInput = true;
    }

    DrawSettings(settings);

    ImGui::End();
}
