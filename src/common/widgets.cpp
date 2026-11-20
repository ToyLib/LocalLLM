#include "common/widgets.h"

namespace ui
{

void WrappedText(std::string_view text)
{
    TextWrap wrap;
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
}

void WrappedDisabled(std::string_view text)
{
    StyleColor color(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    WrappedText(text);
}

void WrappedColored(const ImVec4& color, std::string_view text)
{
    StyleColor style(ImGuiCol_Text, color);
    WrappedText(text);
}

bool Button(const char* label, bool enabled, const ImVec2& size)
{
    Disabled disabled(!enabled);
    return ImGui::Button(label, size);
}

bool SmallButton(const char* label, bool enabled)
{
    Disabled disabled(!enabled);
    return ImGui::SmallButton(label);
}

float ButtonWidth(const char* label)
{
    return ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
}

} // namespace ui
