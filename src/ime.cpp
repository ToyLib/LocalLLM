#include "ime.h"

#include <imgui.h>

#include <string>

namespace {

// IMEで変換中 (未確定) の文字列
struct Composition {
    std::string text;
    int start = -1;     // 選択中の文節の先頭 (文字数)
    int length = -1;    // 選択中の文節の長さ (文字数)
};

Composition g_composition;

// ImGuiが通知する入力カーソルの位置 (変換中の文字列の描画位置)
ImGuiPlatformImeData g_imeData;
void (*g_setImeData)(ImGuiContext*, ImGuiViewport*, ImGuiPlatformImeData*) =
    nullptr;

void SetImeData(
    ImGuiContext* context, ImGuiViewport* viewport, ImGuiPlatformImeData* data)
{
    g_imeData = *data;

    if (g_setImeData)
        g_setImeData(context, viewport, data);
}

// UTF-8文字列の先頭から count 文字進めた位置
const char* AdvanceChars(const char* text, const char* end, int count)
{
    while (text < end && count > 0) {
        ++text;
        while (text < end && (static_cast<unsigned char>(*text) & 0xC0) == 0x80)
            ++text;
        --count;
    }
    return text;
}

} // namespace

void SetImeHint()
{
    // OSの変換ウィンドウと二重に出さない
    SDL_SetHint(SDL_HINT_IME_IMPLEMENTED_UI, "composition");
}

void InstallImeHook()
{
    ImGuiPlatformIO& platformIO = ImGui::GetPlatformIO();
    g_setImeData = platformIO.Platform_SetImeDataFn;
    platformIO.Platform_SetImeDataFn = SetImeData;
}

void HandleCompositionEvent(const SDL_Event& event)
{
    if (event.type == SDL_EVENT_TEXT_EDITING) {
        g_composition.text = event.edit.text ? event.edit.text : "";
        g_composition.start = event.edit.start;
        g_composition.length = event.edit.length;
    }
    else if (event.type == SDL_EVENT_TEXT_INPUT ||
             event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        g_composition = {};
    }
}

void DrawComposition()
{
    if (g_composition.text.empty() || !g_imeData.WantVisible)
        return;

    ImDrawList* drawList = ImGui::GetForegroundDrawList();

    const char* begin = g_composition.text.c_str();
    const char* end = begin + g_composition.text.size();

    const float x = g_imeData.InputPos.x + 1.0f;
    const float y = g_imeData.InputPos.y;
    const float width = ImGui::CalcTextSize(begin, end).x;
    const float height = g_imeData.InputLineHeight;

    ImVec4 background = ImGui::GetStyleColorVec4(ImGuiCol_FrameBg);
    background.w = 1.0f;
    const ImU32 textColor = ImGui::GetColorU32(ImGuiCol_Text);

    drawList->AddRectFilled(
        ImVec2(x, y), ImVec2(x + width, y + height),
        ImGui::GetColorU32(background));
    drawList->AddText(ImVec2(x, y), textColor, begin, end);

    // 下線 (選択中の文節は太線)
    const float underlineY = y + height - 1.0f;
    drawList->AddLine(
        ImVec2(x, underlineY), ImVec2(x + width, underlineY), textColor);

    if (g_composition.start < 0)
        return;

    const char* selBegin = AdvanceChars(begin, end, g_composition.start);
    const float selX = x + ImGui::CalcTextSize(begin, selBegin).x;

    if (g_composition.length > 0) {
        const char* selEnd =
            AdvanceChars(selBegin, end, g_composition.length);
        const float selWidth = ImGui::CalcTextSize(selBegin, selEnd).x;

        drawList->AddLine(
            ImVec2(selX, underlineY), ImVec2(selX + selWidth, underlineY),
            textColor, 3.0f);
    }
    else {
        // 変換中のカーソル
        drawList->AddLine(
            ImVec2(selX, y), ImVec2(selX, y + height), textColor);
    }
}
