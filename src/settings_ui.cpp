#include "settings_ui.h"

#include "config.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>

using ordered_json = nlohmann::ordered_json;

namespace
{

constexpr const char* POPUP_NAME = "設定";

// 文字の大きさを選べる範囲 (ピクセル)
constexpr int MIN_FONT_SIZE = 10;
constexpr int MAX_FONT_SIZE = 48;

// ui.json の colors のキーと、画面に出す説明
struct ColorEntry
{
    const char* key;
    const char* label;
    ImVec4 UiColors::* member;
};

constexpr ColorEntry COLOR_ENTRIES[] = {
    {"background", "背景", &UiColors::background},
    {"text", "文字", &UiColors::text},
    {"sub_text", "補足の文字", &UiColors::subText},
    {"user", "送信者名 (You)", &UiColors::user},
    {"ai", "回答者名", &UiColors::ai},
    {"error", "エラー", &UiColors::error},
};

// "#RRGGBB" を色にする。形が違えば false
bool ParseColor(const std::string& text, ImVec4& color)
{
    unsigned int r, g, b;
    char rest;

    if (text.size() != 7 ||
        std::sscanf(text.c_str(), "#%2x%2x%2x%c", &r, &g, &b, &rest) != 3)
    {
        return false;
    }

    color = UiColors::Rgb(static_cast<int>(r), static_cast<int>(g), static_cast<int>(b));
    return true;
}

std::string FormatColor(const ImVec4& color)
{
    auto channel = [](float value)
    {
        return static_cast<int>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    };

    char text[8];
    std::snprintf(text, sizeof(text), "#%02X%02X%02X",
        channel(color.x), channel(color.y), channel(color.z));
    return text;
}

// 設定画面で編集するエージェントと、画面に出す説明
struct AgentEntry
{
    const char* key;
    const char* label;
};

constexpr AgentEntry AGENT_ENTRIES[] = {
    {"router", "ルーター"},
    {"assistant", "資料ありの回答役"},
    {"general", "資料なしの回答役"},
};

// system_prompt (文字列か文字列の配列) を、編集用に1要素1行の文字列にする
std::string PromptToText(const ordered_json& prompt)
{
    if (prompt.is_string())
    {
        return prompt.get<std::string>();
    }

    std::string text;

    if (prompt.is_array())
    {
        for (const auto& line : prompt)
        {
            if (!text.empty())
            {
                text += "\n";
            }

            if (line.is_string())
            {
                text += line.get<std::string>();
            }
        }
    }

    return text;
}

// 編集した文字列を、1行1要素の配列にする (末尾の空行は捨てる)
ordered_json TextToPromptLines(const std::string& text)
{
    ordered_json lines = ordered_json::array();
    size_t start = 0;

    while (start <= text.size())
    {
        size_t end = text.find('\n', start);

        if (end == std::string::npos)
        {
            end = text.size();
        }

        std::string line = text.substr(start, end - start);

        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }

        lines.push_back(line);
        start = end + 1;
    }

    while (!lines.empty() && lines.back().get<std::string>().empty())
    {
        lines.erase(lines.size() - 1);
    }

    return lines;
}

std::string StringOr(
    const ordered_json& config, const char* key, const std::string& fallback)
{
    return config.contains(key) && config[key].is_string()
        ? config[key].get<std::string>()
        : fallback;
}

// 設定ファイルを読み直して、編集用の値を作る
void LoadSettings(SettingsState& settings)
{
    settings.loadError.clear();
    settings.saveError.clear();
    settings.ui = settings.current;
    settings.agents.clear();

    std::string errors;

    auto load = [&](const char* fileName, ordered_json& target)
    {
        try
        {
            target = LoadConfigForEdit(fileName);
        }
        catch (const std::exception& e)
        {
            target = ordered_json::object();
            errors += std::string(e.what()) + "\n";
        }
    };

    load("ui.json", settings.uiJson);
    load("server.json", settings.serverJson);
    load("agents.json", settings.agentsJson);

    settings.chatUrl = StringOr(settings.serverJson, "chat_url", "");
    settings.embedUrl = StringOr(settings.serverJson, "embed_url", "");

    for (const auto& entry : AGENT_ENTRIES)
    {
        AgentSettings agent;
        agent.key = entry.key;
        agent.name = entry.key;

        const ordered_json& agents = settings.agentsJson;

        if (agents.contains(entry.key) && agents[entry.key].is_object())
        {
            const ordered_json& config = agents[entry.key];
            agent.name = StringOr(config, "name", entry.key);

            if (config.contains("system_prompt"))
            {
                agent.prompt = PromptToText(config["system_prompt"]);
                agent.promptIsString = config["system_prompt"].is_string();
            }
        }

        settings.agents.push_back(std::move(agent));
    }

    if (!errors.empty())
    {
        errors.pop_back();
        settings.loadError = errors +
            "\n(ファイルを直すまで保存できません)";
    }
}

// 編集した値を設定ファイルに書く。失敗したら saveError に入れて false
bool SaveSettings(SettingsState& settings)
{
    // 小数は2桁に丸める (0.8500000238 のように書かれないように)
    settings.uiJson["window_opacity"] =
        std::round(settings.ui.windowOpacity * 100.0) / 100.0;
    settings.uiJson["font_file"] = settings.ui.fontFile;
    settings.uiJson["font_size"] = static_cast<int>(settings.ui.fontSize);

    ordered_json& colors = settings.uiJson["colors"];

    if (!colors.is_object())
    {
        colors = ordered_json::object();
    }

    for (const auto& entry : COLOR_ENTRIES)
    {
        colors[entry.key] = FormatColor(settings.ui.colors.*entry.member);
    }

    settings.serverJson["chat_url"] = settings.chatUrl;
    settings.serverJson["embed_url"] = settings.embedUrl;

    for (const auto& agent : settings.agents)
    {
        ordered_json& config = settings.agentsJson[agent.key];

        if (!config.is_object())
        {
            config = ordered_json::object();
        }

        config["name"] = agent.name;
        config["system_prompt"] = agent.promptIsString
            ? ordered_json(agent.prompt)
            : TextToPromptLines(agent.prompt);
    }

    try
    {
        SaveConfig("ui.json", settings.uiJson);
        SaveConfig("server.json", settings.serverJson);
        SaveConfig("agents.json", settings.agentsJson);
    }
    catch (const std::exception& e)
    {
        settings.saveError = e.what();
        return false;
    }

    return true;
}

void DrawUiTab(SettingsState& settings)
{
    UiSettings& ui = settings.ui;
    bool changed = false;

    ImGui::SeparatorText("背景");
    changed |= ImGui::SliderFloat(
        "不透明度", &ui.windowOpacity, 0.0f, 1.0f, "%.2f",
        ImGuiSliderFlags_AlwaysClamp);

    ImGui::SeparatorText("文字");

    int fontSize = static_cast<int>(ui.fontSize);

    if (ImGui::SliderInt(
            "大きさ (ピクセル)", &fontSize, MIN_FONT_SIZE, MAX_FONT_SIZE,
            "%d", ImGuiSliderFlags_AlwaysClamp))
    {
        ui.fontSize = static_cast<float>(fontSize);
        changed = true;
    }

    ImGui::InputText("フォントファイル", &ui.fontFile);
    ImGui::TextDisabled(
        "フォントファイルは再起動後に反映されます (相対パスは実行ファイルのフォルダーから)");

    ImGui::SeparatorText("色");

    for (const auto& entry : COLOR_ENTRIES)
    {
        ImVec4& color = ui.colors.*entry.member;
        changed |= ImGui::ColorEdit3(
            entry.label, &color.x,
            ImGuiColorEditFlags_DisplayHex | ImGuiColorEditFlags_InputRGB);
    }

    if (ImGui::Button("色を既定に戻す"))
    {
        ui.colors = UiColors();
        changed = true;
    }

    // 不透明度と大きさは、見ながら調整できるようにすぐ反映する
    if (changed)
    {
        ApplyUiSettings(ui);
    }
}

void DrawPromptTab(SettingsState& settings, float footerHeight)
{
    ImGui::TextDisabled(
        "1行が system_prompt の配列の1要素です。送るときは行をそのままつなげます (改行は入りません)。");
    ImGui::TextDisabled("保存すると次の質問から反映されます。");

    if (!ImGui::BeginTabBar("agents"))
    {
        return;
    }

    for (size_t i = 0; i < settings.agents.size(); ++i)
    {
        AgentSettings& agent = settings.agents[i];
        const std::string label =
            std::string(AGENT_ENTRIES[i].label) + "###" + agent.key;

        if (!ImGui::BeginTabItem(label.c_str()))
        {
            continue;
        }

        ImGui::InputText("名前", &agent.name);

        if (agent.promptIsString)
        {
            ImGui::TextDisabled(
                "(この system_prompt は1つの文字列で書かれています。改行もそのまま送ります)");
        }

        ImGui::InputTextMultiline(
            "##prompt", &agent.prompt,
            ImVec2(-FLT_MIN, -footerHeight),
            ImGuiInputTextFlags_WordWrap);

        ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
}

void DrawServerTab(SettingsState& settings)
{
    ImGui::InputText("チャット", &settings.chatUrl);
    ImGui::InputText("埋め込み (RAG)", &settings.embedUrl);
    ImGui::TextDisabled("保存すると次の質問から反映されます。");
}

} // namespace

UiSettings ReadUiSettings(const nlohmann::json& ui)
{
    UiSettings settings;

    if (ui.contains("colors"))
    {
        const nlohmann::json& colors = ui["colors"];

        if (!colors.is_object())
        {
            SDL_Log("Config colors is not an object (using defaults)");
        }
        else
        {
            for (const auto& entry : COLOR_ENTRIES)
            {
                if (!colors.contains(entry.key))
                {
                    continue;
                }

                const nlohmann::json& value = colors[entry.key];

                if (!value.is_string() ||
                    !ParseColor(value.get<std::string>(), settings.colors.*entry.member))
                {
                    SDL_Log("Config colors.%s must be \"#RRGGBB\" (using default)",
                        entry.key);
                }
            }
        }
    }

    settings.windowOpacity = std::clamp(
        ConfigNumberOr(ui, "window_opacity", settings.windowOpacity),
        0.0f, 1.0f);

    settings.fontFile = ConfigStringOr(ui, "font_file", settings.fontFile);

    const float fontSize = ConfigNumberOr(ui, "font_size", settings.fontSize);

    if (fontSize > 0.0f)
    {
        settings.fontSize = fontSize;
    }
    else
    {
        SDL_Log("Config font_size must be positive (using default)");
    }

    return settings;
}

void ApplyUiSettings(const UiSettings& ui)
{
    ImGuiStyle& style = ImGui::GetStyle();

    ImVec4 background = ui.colors.background;
    background.w = ui.windowOpacity;
    style.Colors[ImGuiCol_WindowBg] = background;
    style.Colors[ImGuiCol_Text] = ui.colors.text;
    style.Colors[ImGuiCol_TextDisabled] = ui.colors.subText;

    // フレームの途中で FontSizeBase を変えても戻されるので、
    // 次のフレームの始めに反映させる
    style._NextFrameFontSizeBase = ui.fontSize;
}

const UiSettings& ActiveUiSettings(const SettingsState& settings)
{
    return settings.editing ? settings.ui : settings.current;
}

void DrawSettings(SettingsState& settings)
{
    if (settings.requestOpen)
    {
        settings.requestOpen = false;
        settings.editing = true;
        LoadSettings(settings);
        ImGui::OpenPopup(POPUP_NAME);
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(
        ImVec2(viewport->WorkSize.x * 0.85f, viewport->WorkSize.y * 0.85f),
        ImGuiCond_Appearing);

    // 背景の不透明度を見ながら調整できるように、後ろは暗くしすぎない
    ImGui::PushStyleColor(
        ImGuiCol_ModalWindowDimBg, ImVec4(0.0f, 0.0f, 0.0f, 0.15f));
    const bool open = ImGui::BeginPopupModal(POPUP_NAME);
    ImGui::PopStyleColor();

    if (!open)
    {
        return;
    }

    const ImGuiStyle& style = ImGui::GetStyle();
    const float lineHeight = ImGui::GetTextLineHeightWithSpacing();

    // 下端のボタン (と保存のエラー) の高さ
    const float footerHeight = ImGui::GetFrameHeightWithSpacing() +
        (settings.saveError.empty() ? 0.0f : lineHeight);

    if (!settings.loadError.empty())
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
        ImGui::TextWrapped("%s", settings.loadError.c_str());
        ImGui::PopStyleColor();
    }

    if (ImGui::BeginTabBar("settings"))
    {
        if (ImGui::BeginTabItem("画面"))
        {
            DrawUiTab(settings);
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("システムプロンプト"))
        {
            DrawPromptTab(settings, footerHeight);
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("サーバー"))
        {
            DrawServerTab(settings);
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    // ボタンは下端にそろえる
    const float buttonsY = ImGui::GetWindowHeight() - style.WindowPadding.y -
        ImGui::GetFrameHeight() -
        (settings.saveError.empty() ? 0.0f : lineHeight);
    ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), buttonsY));

    if (!settings.saveError.empty())
    {
        ImGui::TextColored(
            ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", settings.saveError.c_str());
    }

    const float buttonWidth = ImGui::CalcTextSize("キャンセル").x * 1.6f;

    ImGui::BeginDisabled(!settings.loadError.empty());
    if (ImGui::Button("保存", ImVec2(buttonWidth, 0.0f)) &&
        SaveSettings(settings))
    {
        settings.current = settings.ui;
        settings.editing = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();

    ImGui::SameLine();

    if (ImGui::Button("キャンセル", ImVec2(buttonWidth, 0.0f)))
    {
        // 見ながら変えた不透明度と大きさを元に戻す
        ApplyUiSettings(settings.current);
        settings.editing = false;
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}
