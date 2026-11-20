#pragma once

#include <imgui.h>

#include <string_view>

// ImGui の薄いラッパー
//
// - Begin/End や Push/Pop の組は、スコープを抜けると閉じるクラスにする
//   (閉じ忘れや、途中で return したときの閉じ漏れを防ぐ)
//       {
//           ui::Disabled disabled(busy);
//           ImGui::InputText(...);
//       }
// - よく使う描き方は関数にする (折り返すテキスト、押せないことがあるボタン)
//
// ImGui は毎フレーム描き直すので、ウィジェットを状態を持つクラスにはしない
namespace ui
{

// BeginDisabled / EndDisabled
class Disabled
{
public:
    [[nodiscard]] explicit Disabled(bool disabled = true) { ImGui::BeginDisabled(disabled); }
    ~Disabled() { ImGui::EndDisabled(); }

    Disabled(const Disabled&) = delete;
    Disabled& operator=(const Disabled&) = delete;
};

// PushID / PopID
class Id
{
public:
    [[nodiscard]] explicit Id(int id) { ImGui::PushID(id); }
    [[nodiscard]] explicit Id(const char* id) { ImGui::PushID(id); }
    ~Id() { ImGui::PopID(); }

    Id(const Id&) = delete;
    Id& operator=(const Id&) = delete;
};

// PushStyleColor / PopStyleColor
// enabled が false なら何もしない (選んでいるものだけ色を変える、など)
class StyleColor
{
public:
    [[nodiscard]] StyleColor(ImGuiCol index, const ImVec4& color, bool enabled = true)
        : mEnabled(enabled)
    {
        if (mEnabled)
        {
            ImGui::PushStyleColor(index, color);
        }
    }

    ~StyleColor()
    {
        if (mEnabled)
        {
            ImGui::PopStyleColor();
        }
    }

    StyleColor(const StyleColor&) = delete;
    StyleColor& operator=(const StyleColor&) = delete;

private:
    bool mEnabled;
};

// PushTextWrapPos / PopTextWrapPos (0 なら窓の右端で折り返す)
class TextWrap
{
public:
    [[nodiscard]] explicit TextWrap(float wrapPos = 0.0f) { ImGui::PushTextWrapPos(wrapPos); }
    ~TextWrap() { ImGui::PopTextWrapPos(); }

    TextWrap(const TextWrap&) = delete;
    TextWrap& operator=(const TextWrap&) = delete;
};

// BeginChild / EndChild (EndChild は BeginChild の戻り値に関係なく呼ぶ)
class Child
{
public:
    [[nodiscard]] Child(
        const char* id,
        const ImVec2& size = ImVec2(0.0f, 0.0f),
        ImGuiChildFlags childFlags = ImGuiChildFlags_None,
        ImGuiWindowFlags windowFlags = ImGuiWindowFlags_None)
    {
        ImGui::BeginChild(id, size, childFlags, windowFlags);
    }

    ~Child() { ImGui::EndChild(); }

    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
};

// BeginGroup / EndGroup
class Group
{
public:
    [[nodiscard]] Group() { ImGui::BeginGroup(); }
    ~Group() { ImGui::EndGroup(); }

    Group(const Group&) = delete;
    Group& operator=(const Group&) = delete;
};

// 窓の右端で折り返すテキスト (書式は使わない)
void WrappedText(std::string_view text);
void WrappedDisabled(std::string_view text);
void WrappedColored(const ImVec4& color, std::string_view text);

// enabled が false なら押せない (薄く表示する) ボタン。押されたら true
bool Button(const char* label, bool enabled, const ImVec2& size = ImVec2(0.0f, 0.0f));
bool SmallButton(const char* label, bool enabled);

// ラベルがちょうど収まるボタンの幅
float ButtonWidth(const char* label);

} // namespace ui
