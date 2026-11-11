// コーディング練習タブの描画 (ImGui)

#include "code_ui.h"

#include "ime.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <algorithm>

namespace
{

constexpr const char* NOTICE =
    "生成されたコードはあなたのパソコンでそのまま動きます。"
    "内容を読んでから実行してください (自己責任で使ってください)。";

int CountLines(const std::string& text)
{
    return 1 + static_cast<int>(std::count(text.begin(), text.end(), '\n'));
}

// 行番号の列 (コード欄のスクロールに合わせる)
void DrawGutter(const std::string& code, float height, float scrollY, const UiColors& colors)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    const int lines = CountLines(code);
    const float width = ImGui::CalcTextSize(std::to_string(std::max(lines, 99)).c_str()).x +
                        style.FramePadding.x * 2.0f;

    // コード欄と同じ余白にして、行が揃うようにする
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(style.FramePadding.x, style.FramePadding.y));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::BeginChild(
        "gutter", ImVec2(width, height), ImGuiChildFlags_AlwaysUseWindowPadding,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav);
    ImGui::SetScrollY(scrollY);

    for (int n = 1; n <= lines; ++n)
    {
        const std::string number = std::to_string(n);
        ImGui::SetCursorPosX(
            width - style.FramePadding.x - ImGui::CalcTextSize(number.c_str()).x);
        ImGui::TextColored(colors.subText, "%s", number.c_str());
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

} // namespace

void DrawCodeTab(CodeState& state, const UiColors& colors)
{
    bool generating;
    bool explaining;
    bool running;
    bool canRevert;
    int version;
    std::string explanation;
    std::string output;
    std::string runStatus;
    std::string error;

    {
        std::lock_guard lock(state.mutex);
        generating = state.generating;
        explaining = state.explaining;
        running = state.running;
        version = state.codeVersion;
        explanation = state.explanation;
        output = state.output;
        runStatus = state.runStatus;
        error = state.error;

        // LLM の生成や「元に戻す」で変わったときだけ、コード欄を書き換える
        // (ユーザーが編集中の内容は上書きしない)
        if (version != state.shownVersion)
        {
            state.shownVersion = version;
            state.editor = state.code;
        }

        canRevert = !state.originalCode.empty() && state.editor != state.originalCode;
    }

    const bool busy = generating || explaining;
    const bool idle = !busy && !running;
    const ImGuiStyle& style = ImGui::GetStyle();
    const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
    const float spacing = style.ItemSpacing.y;

    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", NOTICE);
    ImGui::PopTextWrapPos();

    // 作りたいもの (Enter で作る / Shift+Enter で改行)
    const float buttonWidth = ImGui::CalcTextSize("作る").x * 3.0f;
    const float promptHeight = lineHeight * 3.0f + style.FramePadding.y * 2.0f;

    if (state.focusPrompt)
    {
        ImGui::SetKeyboardFocusHere();
        state.focusPrompt = false;
    }

    const bool entered = ImGui::InputTextMultiline(
        "##prompt", &state.prompt,
        ImVec2(-(buttonWidth + style.ItemSpacing.x), promptHeight),
        ImGuiInputTextFlags_EnterReturnsTrue |
        ImGuiInputTextFlags_CtrlEnterForNewLine |
        ImGuiInputTextFlags_WordWrap);

    ImGui::SameLine();

    ImGui::BeginDisabled(!idle);
    const bool pressed = ImGui::Button("作る", ImVec2(buttonWidth, promptHeight));
    ImGui::EndDisabled();

    DrawComposition();

    if (entered)
    {
        state.focusPrompt = true;
    }

    if ((pressed || (entered && idle)) &&
        state.prompt.find_first_not_of(" \t\r\n") != std::string::npos)
    {
        StartGenerate(state, state.prompt, state.editor);
    }

    // 下に並べるもの: 状態、実行結果、ボタンの高さ
    const float statusHeight = lineHeight;
    const float buttonsHeight = ImGui::GetFrameHeight() + spacing;
    const float outputHeight = lineHeight * 8.0f + style.WindowPadding.y * 2.0f;
    const float labelHeight = lineHeight;

    const float middleHeight = std::max(
        ImGui::GetContentRegionAvail().y -
            (statusHeight + outputHeight + labelHeight * 3.0f + buttonsHeight + spacing * 3.0f),
        lineHeight * 4.0f);

    // コードと解説を左右に並べる
    const float totalWidth = ImGui::GetContentRegionAvail().x;
    const float codeWidth = (totalWidth - style.ItemSpacing.x) * 0.58f;
    const float explainWidth = totalWidth - style.ItemSpacing.x - codeWidth;

    ImGui::BeginGroup();
    ImGui::TextDisabled("コード (編集できます)");

    const float codeHeight = middleHeight;
    const int lines = CountLines(state.editor);
    const float gutterWidth =
        ImGui::CalcTextSize(std::to_string(std::max(lines, 99)).c_str()).x +
        style.FramePadding.x * 2.0f;

    // 行番号の列は、コード欄の縦スクロールに合わせる (前のフレームの値)
    const ImGuiID editorId = ImGui::GetID("##code");
    float scrollY = 0.0f;

    if (const ImGuiInputTextState* input = ImGui::GetInputTextState(editorId))
    {
        scrollY = input->Scroll.y;
    }

    DrawGutter(state.editor, codeHeight, scrollY, colors);
    ImGui::SameLine(0.0f, 0.0f);

    ImGui::InputTextMultiline(
        "##code", &state.editor,
        ImVec2(codeWidth - gutterWidth - style.ItemSpacing.x, codeHeight),
        ImGuiInputTextFlags_AllowTabInput);
    ImGui::EndGroup();

    DrawComposition();

    ImGui::SameLine();

    // 解説
    ImGui::BeginGroup();
    ImGui::TextDisabled("解説");
    ImGui::BeginChild("explanation", ImVec2(explainWidth, middleHeight), ImGuiChildFlags_Borders);

    if (explanation.empty())
    {
        if (generating || explaining)
        {
            ImGui::TextDisabled(generating ? "コードを作っています…" : "解説を作っています…");
        }
    }
    else
    {
        ImGui::TextWrapped("%s", explanation.c_str());
    }

    ImGui::EndChild();
    ImGui::EndGroup();

    // 操作ボタン
    ImGui::BeginDisabled(!idle);
    if (ImGui::Button("実行"))
    {
        StartRun(state, state.editor);
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(idle);
    if (ImGui::Button("停止"))
    {
        if (busy)
        {
            CancelGenerate(state);
        }
        else
        {
            StopRun(state);
        }
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!idle || !canRevert);
    if (ImGui::Button("元に戻す"))
    {
        RevertCode(state);
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!idle);
    if (ImGui::Button("解説を作り直す"))
    {
        StartExplain(state, state.editor);
    }
    ImGui::EndDisabled();

    // 実行結果 (下まで届いていたら追従する)
    ImGui::TextDisabled("実行結果");
    ImGui::BeginChild("output", ImVec2(0.0f, outputHeight), ImGuiChildFlags_Borders);
    const bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;
    ImGui::TextWrapped("%s", output.c_str());

    if (running && atBottom)
    {
        ImGui::SetScrollHereY(1.0f);
    }

    ImGui::EndChild();

    // 状態とエラー
    if (!error.empty())
    {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(colors.error, "%s", error.c_str());
        ImGui::PopTextWrapPos();
    }
    else
    {
        ImGui::TextDisabled("%s", runStatus.c_str());
    }
}
