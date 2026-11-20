// チャット画面の描画 (ImGui)

#include "chat/chat_ui.h"

#include "common/ime.h"
#include "common/server_manager.h"
#include "common/widgets.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>

namespace
{

constexpr const char* CONFIRM_POPUP = "資料を送る確認";

// 会話の欄で押されたボタン (ロックを外してから処理する)
enum class MessageAction
{
    None,
    AskCloud,
    ConfirmAskCloud,
    AskWeb
};

// 最新の回答の下の「ChatGPT に聞き直す」「Web でも調べて比べる」ボタン
MessageAction DrawAskCloudButton(const ChatState& state, const Message& message)
{
    const AnswerStage next = NextStage(state, message);

    if (next == AnswerStage::Local)
    {
        return MessageAction::None;
    }

    const bool web = next == AnswerStage::Web;
    const std::string label =
        web ? std::string("Web でも調べて比べる") : state.cloudName + " に聞き直す";
    // 資料を外に送らない設定 (既定) のときは、資料を使った回答は聞き直せない
    const bool enabled = state.hasCloudKey && CanAskCloud(state) &&
                         (!message.usedRag || state.allowRag);

    const bool pressed = ui::SmallButton(label.c_str(), enabled);

    if (!state.hasCloudKey)
    {
        ImGui::SetItemTooltip("設定画面で API キーを登録すると使えます");
    }
    else if (message.usedRag && state.allowRag)
    {
        ImGui::SetItemTooltip("資料 (rag/) の内容も送ります (送る前に確認します)");
    }
    else if (message.usedRag)
    {
        // 押せない理由は、ツールチップではなく横に書く (デモで分かりやすいように)
        ImGui::SameLine();
        ImGui::TextDisabled("%s", "資料を使った回答は、資料を外に送らないため聞き直せません");
    }

    if (!pressed || !enabled)
    {
        return MessageAction::None;
    }

    if (web)
    {
        return MessageAction::AskWeb;
    }

    return message.usedRag ? MessageAction::ConfirmAskCloud : MessageAction::AskCloud;
}

MessageAction DrawMessages(ChatState& state, const UiColors& colors)
{
    std::lock_guard lock(state.mutex);

    MessageAction action = MessageAction::None;

    // 前フレームで一番下までスクロールしていたら追従する
    const bool atBottom =
        ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;

    for (size_t i = 0; i < state.messages.size(); ++i)
    {
        const auto& message = state.messages[i];
        const bool isUser = message.role == "user";

        ui::Id id(static_cast<int>(i));

        // AI の回答には、答えたエージェントの名前を出す
        const char* name = isUser ? "You"
            : message.agent.empty() ? "AI"
            : message.agent.c_str();

        if (message.superseded)
        {
            // 聞き直す前の回答は、薄く残す
            ImGui::TextDisabled(
                "%s (%s)", name,
                message.stage == AnswerStage::Local ? "ローカルの回答" : "Web 検索なしの回答");
            ui::WrappedDisabled(message.content);
        }
        else
        {
            ImGui::TextColored(
                isUser ? colors.user : colors.ai,
                "%s", name);

            if (!message.status.empty())
            {
                ui::WrappedDisabled(FormatStatus(message));
            }

            if (message.content.empty() && state.busy)
            {
                if (message.status.empty())
                {
                    ImGui::TextDisabled("考え中...");
                }
            }
            else
            {
                ImGui::TextWrapped("%s", message.content.c_str());
            }
        }

        // 書き直す前の ChatGPT の回答 (書き直しで中身が変わっていないか確かめられるように)
        if (!message.cloudOriginal.empty() && !message.superseded &&
            ImGui::TreeNode("original", "%s の元の回答", state.cloudName.c_str()))
        {
            ImGui::TextWrapped("%s", message.cloudOriginal.c_str());
            ImGui::TreePop();
        }

        // Web 検索の出典 (クリックでブラウザーを開く)
        if (!message.citations.empty() && !message.superseded)
        {
            ImGui::TextDisabled("出典:");

            for (size_t n = 0; n < message.citations.size(); ++n)
            {
                const Citation& citation = message.citations[n];
                const std::string label =
                    "[" + std::to_string(n + 1) + "] " + citation.title +
                    "##citation" + std::to_string(n);

                if (ImGui::TextLink(label.c_str()))
                {
                    SDL_OpenURL(citation.url.c_str());
                }

                ImGui::SetItemTooltip("%s", citation.url.c_str());
            }
        }

        // 聞き直す前の回答との違いのまとめ
        if (!message.compareVerdict.empty() && !message.superseded)
        {
            ui::TextWrap wrap;
            ImGui::TextDisabled(
                "%sとの違い (ローカルの LLM による目安):",
                message.stage == AnswerStage::Web ? "Web 検索なしの回答" : "聞き直す前の回答");
            ImGui::SameLine();

            if (message.compareVerdict == "食い違いあり")
            {
                ImGui::TextColored(colors.error, "%s", message.compareVerdict.c_str());
            }
            else
            {
                ImGui::TextUnformatted(message.compareVerdict.c_str());
            }

            for (const auto& point : message.comparePoints)
            {
                ImGui::Bullet();
                ImGui::TextDisabled("%s", point.c_str());
            }
        }

        if (!message.note.empty())
        {
            ui::WrappedDisabled(message.note);
        }

        if (ImGui::BeginPopupContextItem("copy"))
        {
            if (ImGui::MenuItem("コピー"))
            {
                ImGui::SetClipboardText(message.content.c_str());
            }
            ImGui::EndPopup();
        }

        // 最新の回答にだけ、ChatGPT に聞き直すボタンを付ける
        if (i + 1 == state.messages.size() && !isUser &&
            !message.agentKey.empty() && !message.superseded)
        {
            action = DrawAskCloudButton(state, message);
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
    }

    if (atBottom)
    {
        ImGui::SetScrollHereY(1.0f);
    }

    return action;
}

// 資料を使った回答を聞き直す前の確認
void DrawConfirmPopup(ChatState& state)
{
    if (!ImGui::BeginPopupModal(
            CONFIRM_POPUP, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        return;
    }

    ImGui::Text("資料 (rag/) の内容が %s に送られます。送りますか？",
        state.cloudName.c_str());
    ImGui::TextDisabled("ほかの、資料を使ったやりとりは送りません。");
    ImGui::Spacing();

    const float buttonWidth = ImGui::CalcTextSize("キャンセル").x * 1.6f;

    if (ImGui::Button("送る", ImVec2(buttonWidth, 0.0f)))
    {
        ImGui::CloseCurrentPopup();
        AskCloud(state);
    }

    ImGui::SameLine();

    if (ImGui::Button("キャンセル", ImVec2(buttonWidth, 0.0f)) ||
        ImGui::IsKeyPressed(ImGuiKey_Escape))
    {
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

// RAG のオン/オフとインデックスの状態
void DrawRagBar(ChatState& state, RagState& rag)
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
    if (ui::SmallButton("再読込", !building))
    {
        StartRagIndexing(rag);
    }

    ImGui::SameLine();
    ImGui::TextDisabled("%s", status.c_str());
}

} // namespace

ChatTab::ChatTab(RagState& rag)
    : mRag(rag)
{
    RefreshCloudStatus(mState);
}

void ChatTab::OnSettingsSaved()
{
    RefreshCloudStatus(mState);
}

void ChatTab::Shutdown()
{
    mState.cancel = true;
    JoinWorker(mState);
}

void ChatTab::Draw(const UiContext& context)
{
    ChatState& state = mState;

    bool busy;
    std::string error;
    {
        std::lock_guard lock(state.mutex);
        busy = state.busy;
        error = state.error;

        // 送れなかった質問を入力欄に戻す
        if (!state.restoredPrompt.empty())
        {
            mInput = std::move(state.restoredPrompt);
            state.restoredPrompt.clear();
            mFocusInput = true;
        }
    }

    const ImGuiStyle& style = ImGui::GetStyle();
    const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
    const float inputHeight =
        lineHeight * 4.0f + style.FramePadding.y * 2.0f;
    const float errorHeight =
        error.empty() ? 0.0f : lineHeight + style.ItemSpacing.y;

    DrawRagBar(state, mRag);

    // 会話表示 (高さは残りから入力欄などの分を引いたもの)
    MessageAction action;
    {
        ui::Child messages(
            "messages",
            ImVec2(0.0f, -(inputHeight + errorHeight + style.ItemSpacing.y)),
            ImGuiChildFlags_Borders);
        action = DrawMessages(state, context.colors);
    }

    if (action == MessageAction::AskCloud)
    {
        AskCloud(state);
    }
    else if (action == MessageAction::AskWeb)
    {
        AskCloud(state, true);
    }
    else if (action == MessageAction::ConfirmAskCloud)
    {
        ImGui::OpenPopup(CONFIRM_POPUP);
    }

    DrawConfirmPopup(state);

    if (!error.empty())
    {
        ImGui::TextColored(context.colors.error, "Error: %s", error.c_str());
    }

    // 入力欄 (Enterで送信 / Shift+Enterで改行)
    const float buttonWidth = ImGui::CalcTextSize("クリア").x * 2.5f;

    if (mFocusInput)
    {
        ImGui::SetKeyboardFocusHere();
        mFocusInput = false;
    }

    const bool entered = ImGui::InputTextMultiline(
        "##input", &mInput,
        ImVec2(-(buttonWidth + style.ItemSpacing.x), inputHeight),
        ImGuiInputTextFlags_EnterReturnsTrue |
        ImGuiInputTextFlags_CtrlEnterForNewLine |
        ImGuiInputTextFlags_WordWrap);

    ImGui::SameLine();

    bool send = false;
    {
        ui::Group buttons;

        const ImVec2 buttonSize(
            buttonWidth, (inputHeight - style.ItemSpacing.y) / 2.0f);

        if (busy)
        {
            if (ImGui::Button("中断", buttonSize))
            {
                state.cancel = true;
            }
        }
        else
        {
            // サーバーの準備ができるまでは送らない (入力は残す)
            const bool available = IsChatServerAvailable();
            send = (ui::Button("送信", available, buttonSize) || entered) && available;
        }

        if (ui::Button("クリア", !busy, buttonSize))
        {
            std::lock_guard lock(state.mutex);
            state.messages.clear();
            state.error.clear();
            mFocusInput = true;
        }
    }

    DrawComposition();

    if (entered)
    {
        mFocusInput = true;
    }

    if (send && mInput.find_first_not_of(" \t\r\n") != std::string::npos)
    {
        SendPrompt(state, mRag, mInput);
        mInput.clear();
        mFocusInput = true;
    }
}
