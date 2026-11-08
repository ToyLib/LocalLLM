#include "settings_ui.h"

#include "agent.h"
#include "config.h"
#include "chat.h"
#include "llm_client.h"
#include "server_manager.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <mutex>

using ordered_json = nlohmann::ordered_json;

namespace
{

constexpr const char* POPUP_NAME = "設定";
constexpr const char* RESET_POPUP_NAME = "既定に戻す";

// 文字の大きさを選べる範囲 (ピクセル)
constexpr int MIN_FONT_SIZE = 10;
constexpr int MAX_FONT_SIZE = 48;

// llama-server で起動するモデルの候補
struct ModelPreset
{
    const char* name;
    const char* model;
};

constexpr ModelPreset CHAT_MODEL_PRESETS[] = {
    {"Qwen3 4B 2507", "unsloth/Qwen3-4B-Instruct-2507-GGUF:Q4_K_M"},
    {"Gemma 3 4B", "ggml-org/gemma-3-4b-it-GGUF"},
    {"Phi-4 mini", "bartowski/microsoft_Phi-4-mini-instruct-GGUF:Q4_K_M"},
    {"gpt-oss 20B", "ggml-org/gpt-oss-20b-GGUF:MXFP4"},
};

// ファイル選択ダイアログの結果 (ダイアログのコールバックは別のスレッドから呼ばれることがある)
std::mutex pickedServerPathMutex;
std::string pickedServerPath;

void SDLCALL OnServerPathPicked(void*, const char* const* files, int)
{
    if (files && files[0])
    {
        {
            std::lock_guard lock(pickedServerPathMutex);
            pickedServerPath = files[0];
        }

        WakeUI();
    }
}

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
    {"cloud", "ChatGPT"},
    {"rewriter", "口調の書き直し"},
    {"comparer", "違いのまとめ"},
};

bool IsAnswerer(const std::string& key)
{
    return key == "assistant" || key == "general";
}

// API キーの末尾だけを見せる
std::string MaskKey(const std::string& key)
{
    if (key.size() <= 8)
    {
        return "…";
    }

    return "…" + key.substr(key.size() - 4);
}

// API キーの状態を調べ直す (キーの名前が変わったときだけ)
void UpdateKeyStatus(SettingsState& settings)
{
    if (settings.keyStatusName == settings.cloudKeyName &&
        !settings.keyStatus.empty())
    {
        return;
    }

    settings.keyStatusName = settings.cloudKeyName;
    const ApiKey key = FindApiKey(settings.cloudKeyName);

    switch (key.source)
    {
    case ApiKeySource::Environment:
        settings.keyStatus = "設定済み (" + MaskKey(key.value) + ")。環境変数 " +
            settings.cloudKeyName + " から読んでいます";
        break;
    case ApiKeySource::SecretsFile:
        settings.keyStatus = "設定済み (" + MaskKey(key.value) + ")。秘密ファイルから読んでいます";
        break;
    default:
        settings.keyStatus = "未設定";
        break;
    }
}

// 編集用の API キーを消す (メモリに残さないように)
void ClearApiKeyInput(SettingsState& settings)
{
    std::fill(settings.apiKeyInput.begin(), settings.apiKeyInput.end(), '\0');
    settings.apiKeyInput.clear();
}

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

// config/personas.json のテンプレートを読む
// 読めなくても設定は保存できるように、loadError ではなく templateError に入れる
void LoadTemplates(SettingsState& settings)
{
    settings.templates.clear();
    settings.templateError.clear();

    ordered_json config;

    try
    {
        config = LoadConfigForEdit("personas.json");
    }
    catch (const std::exception& e)
    {
        settings.templateError = e.what();
        return;
    }

    if (!config.contains("templates") || !config["templates"].is_array())
    {
        settings.templateError = "personas.json に templates (配列) がありません";
        return;
    }

    for (const auto& entry : config["templates"])
    {
        if (!entry.is_object() || !entry.contains("system_prompt"))
        {
            continue;
        }

        PromptTemplate item;
        item.name = StringOr(entry, "name", "(名前なし)");
        item.prompt = PromptToText(entry["system_prompt"]);
        item.promptIsString = entry["system_prompt"].is_string();
        settings.templates.push_back(std::move(item));
    }
}

// テンプレートの選択欄と、選んだものを読み込むボタン
void DrawTemplatePicker(SettingsState& settings, AgentSettings& agent)
{
    if (!settings.templateError.empty())
    {
        ImGui::TextDisabled("テンプレートを読めません: %s", settings.templateError.c_str());
        return;
    }

    if (settings.templates.empty())
    {
        return;
    }

    agent.templateIndex = std::clamp(
        agent.templateIndex, 0, static_cast<int>(settings.templates.size()) - 1);

    const float buttonWidth = ImGui::CalcTextSize("このテンプレートにする").x +
        ImGui::GetStyle().FramePadding.x * 2.0f;

    ImGui::SetNextItemWidth(std::max(
        ImGui::GetContentRegionAvail().x * 0.4f,
        ImGui::GetFontSize() * 10.0f));

    if (ImGui::BeginCombo(
            "##template", settings.templates[agent.templateIndex].name.c_str()))
    {
        for (int i = 0; i < static_cast<int>(settings.templates.size()); ++i)
        {
            const bool selected = i == agent.templateIndex;

            if (ImGui::Selectable(settings.templates[i].name.c_str(), selected))
            {
                agent.templateIndex = i;
            }

            if (selected)
            {
                ImGui::SetItemDefaultFocus();
            }
        }

        ImGui::EndCombo();
    }

    ImGui::SameLine();

    // 名前とプロンプトを置き換える (保存するまではファイルは変わらない)
    if (ImGui::Button("このテンプレートにする", ImVec2(buttonWidth, 0.0f)))
    {
        const PromptTemplate& item = settings.templates[agent.templateIndex];
        agent.name = item.name;
        agent.prompt = item.prompt;
        agent.promptIsString = item.promptIsString;
    }

    ImGui::SameLine();
    ImGui::TextDisabled("テンプレート");
}

// 設定ファイルを読み直して、編集用の値を作る
void LoadSettings(SettingsState& settings)
{
    settings.loadError.clear();
    settings.saveError.clear();
    settings.configDirectory = ConfigDirectoryPath();
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

    const ManagedSettings managed = LoadManagedSettings();
    settings.managedEnabled = managed.enabled;
    settings.managedServerPath = managed.serverPath;
    settings.managedChatModel = managed.chatModel;
    settings.managedEmbedModel = managed.embedModel;
    settings.managedContext = managed.context;
    settings.managedEmbedContext = managed.embedContext;
    settings.managedGpuLayers = managed.gpuLayers;

    // cloud が無い (前の版の設定ファイル) ときは既定値
    const CloudSettings defaults;
    const ordered_json cloud =
        settings.serverJson.contains("cloud") && settings.serverJson["cloud"].is_object()
            ? settings.serverJson["cloud"]
            : ordered_json::object();
    settings.cloudName = StringOr(cloud, "name", defaults.name);
    settings.cloudUrl = StringOr(cloud, "chat_url", defaults.chatUrl);
    settings.cloudModel = StringOr(cloud, "model", defaults.model);
    settings.cloudKeyName = StringOr(cloud, "api_key_name", defaults.apiKeyName);

    ClearApiKeyInput(settings);
    settings.deleteApiKey = false;
    settings.keyStatus.clear();

    for (const auto& entry : AGENT_ENTRIES)
    {
        AgentSettings agent;
        agent.key = entry.key;
        agent.name = entry.key;
        agent.rewrite = agent.key != "assistant";

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

            if (config.contains("rewrite") && config["rewrite"].is_boolean())
            {
                agent.rewrite = config["rewrite"].get<bool>();
            }
        }
        else if (!DefaultAgentPrompt(entry.key).empty())
        {
            // 前の版の設定ファイルには無いので、既定のプロンプトから始める
            agent.name = DefaultAgentName(entry.key);
            agent.prompt = DefaultAgentPrompt(entry.key);
        }

        settings.agents.push_back(std::move(agent));
    }

    LoadTemplates(settings);

    // 選択欄は、今の名前と同じテンプレートを選んだ状態で開く
    for (auto& agent : settings.agents)
    {
        for (size_t i = 0; i < settings.templates.size(); ++i)
        {
            if (settings.templates[i].name == agent.name)
            {
                agent.templateIndex = static_cast<int>(i);
            }
        }
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

    ordered_json& managed = settings.serverJson["managed"];

    if (!managed.is_object())
    {
        managed = ordered_json::object();
    }

    managed["enabled"] = settings.managedEnabled;
    managed["server_path"] = settings.managedServerPath;
    managed["chat_model"] = settings.managedChatModel;
    managed["embed_model"] = settings.managedEmbedModel;
    managed["context"] = (std::max)(settings.managedContext, 1);
    managed["embed_context"] = (std::max)(settings.managedEmbedContext, 1);
    managed["gpu_layers"] = (std::max)(settings.managedGpuLayers, 0);

    // auth など、画面で扱わない項目はそのまま残す
    ordered_json& cloud = settings.serverJson["cloud"];

    if (!cloud.is_object())
    {
        cloud = ordered_json::object();
    }

    cloud["name"] = settings.cloudName;
    cloud["chat_url"] = settings.cloudUrl;
    cloud["model"] = settings.cloudModel;
    cloud["api_key_name"] = settings.cloudKeyName;

    if (!cloud.contains("auth"))
    {
        cloud["auth"] = "bearer";
    }

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

        if (IsAnswerer(agent.key))
        {
            config["rewrite"] = agent.rewrite;
        }
    }

    try
    {
        // API キーは設定ファイルではなく秘密ファイルに書く
        if (settings.deleteApiKey)
        {
            SaveApiKey(settings.cloudKeyName, "");
        }
        else if (settings.apiKeyInput.find_first_not_of(" \t\r\n") != std::string::npos)
        {
            std::string key = settings.apiKeyInput;
            key.erase(0, key.find_first_not_of(" \t\r\n"));
            key.erase(key.find_last_not_of(" \t\r\n") + 1);
            SaveApiKey(settings.cloudKeyName, key);
            std::fill(key.begin(), key.end(), '\0');
        }

        ClearApiKeyInput(settings);
        settings.deleteApiKey = false;
        settings.keyStatus.clear();

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

        // 回答役は、人格のテンプレートから選んで直せる
        if (IsAnswerer(agent.key))
        {
            DrawTemplatePicker(settings, agent);
            ImGui::InputText("名前", &agent.name);
            ImGui::Checkbox(
                "ChatGPT に聞き直した回答を、この口調で書き直す", &agent.rewrite);
        }
        else if (agent.key == "cloud")
        {
            ImGui::TextDisabled(
                "「ChatGPT に聞き直す」で送る system プロンプトです (名前はサーバーのタブで決めます)。");
        }
        else if (agent.key == "rewriter")
        {
            ImGui::TextDisabled(
                "ChatGPT の回答を書き直すとき、回答役の system プロンプトの後ろに付ける指示です。");
        }
        else if (agent.key == "comparer")
        {
            ImGui::TextDisabled(
                "ChatGPT に聞き直した後、聞き直す前の回答との違いをローカルの LLM でまとめるときの指示です。");
        }
        else
        {
            ImGui::InputText("名前", &agent.name);
        }

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

void DrawChatModelPicker(SettingsState& settings)
{
    const char* preview = "(その他)";

    for (const auto& preset : CHAT_MODEL_PRESETS)
    {
        if (settings.managedChatModel == preset.model)
        {
            preview = preset.name;
        }
    }

    if (ImGui::BeginCombo("モデルの候補", preview))
    {
        for (const auto& preset : CHAT_MODEL_PRESETS)
        {
            if (ImGui::Selectable(preset.name, settings.managedChatModel == preset.model))
            {
                settings.managedChatModel = preset.model;
            }
        }

        ImGui::EndCombo();
    }
}

void DrawServerStatusLine(const char* name, const ServerView& view)
{
    ImGui::PushTextWrapPos(0.0f);

    if (view.detail.empty())
    {
        ImGui::Text("%s: %s", name, ServerStatusText(view.status));
    }
    else
    {
        ImGui::Text("%s: %s (%s)", name, ServerStatusText(view.status), view.detail.c_str());
    }

    ImGui::PopTextWrapPos();
}

void DrawManagedServer(SettingsState& settings)
{
    ImGui::SeparatorText("llama-server の自動起動");
    ImGui::Checkbox("アプリから llama-server を起動する", &settings.managedEnabled);

    {
        std::lock_guard lock(pickedServerPathMutex);

        if (!pickedServerPath.empty())
        {
            settings.managedServerPath = std::move(pickedServerPath);
            pickedServerPath.clear();
        }
    }

    ImGui::BeginDisabled(!settings.managedEnabled);

    DrawChatModelPicker(settings);
    ImGui::InputText("チャットのモデル", &settings.managedChatModel);
    ImGui::InputText("埋め込みのモデル (空なら起動しない)", &settings.managedEmbedModel);
    ImGui::InputInt("チャットのコンテキスト", &settings.managedContext, 0);
    ImGui::InputInt("埋め込みのコンテキスト", &settings.managedEmbedContext, 0);
    ImGui::InputInt("GPU に載せる層の数", &settings.managedGpuLayers, 0);

    ImGui::InputText("llama-server の場所 (空なら自動で探す)", &settings.managedServerPath);
    ImGui::SameLine();

    if (ImGui::SmallButton("参照..."))
    {
        SDL_ShowOpenFileDialog(
            OnServerPathPicked, nullptr, nullptr, nullptr, 0, nullptr, false);
    }

    ImGui::EndDisabled();

    const ServerManagerView view = GetServerManagerView();

    DrawServerStatusLine("チャットのサーバー", view.chat);
    DrawServerStatusLine("埋め込みのサーバー", view.embed);

    if (!view.binaryPath.empty())
    {
        ImGui::TextDisabled(
            "使用中: %s (%s)", view.binaryPath.c_str(), view.binarySource.c_str());
    }

    if (!view.warning.empty())
    {
        ImGui::TextColored(ActiveUiSettings(settings).colors.error, "%s", view.warning.c_str());
    }

    if (!view.logDirectory.empty())
    {
        ImGui::TextDisabled("ログ: %s", view.logDirectory.c_str());
    }
}

void DrawServerTab(SettingsState& settings)
{
    ImGui::SeparatorText("ローカル");
    ImGui::InputText("チャット", &settings.chatUrl);
    ImGui::InputText("埋め込み (RAG)", &settings.embedUrl);
    ImGui::TextDisabled("保存すると次の質問から反映されます。");

    DrawManagedServer(settings);

    ImGui::SeparatorText("ChatGPT (「聞き直す」ボタンで使う)");
    ImGui::InputText("表示名", &settings.cloudName);
    ImGui::InputText("URL", &settings.cloudUrl);
    ImGui::InputText("モデル", &settings.cloudModel);
    ImGui::InputText("API キーの名前", &settings.cloudKeyName);
    ImGui::TextDisabled(
        "API キーは、この名前の環境変数か、下の秘密ファイルから読みます (設定ファイルには書きません)。");

    UpdateKeyStatus(settings);
    ImGui::Text("API キー: %s", settings.keyStatus.c_str());

    ImGui::BeginDisabled(settings.deleteApiKey);
    ImGui::InputText(
        "新しい API キー", &settings.apiKeyInput, ImGuiInputTextFlags_Password);
    ImGui::EndDisabled();

    ImGui::SameLine();

    if (settings.deleteApiKey)
    {
        if (ImGui::SmallButton("削除をやめる"))
        {
            settings.deleteApiKey = false;
        }
    }
    else if (ImGui::SmallButton("秘密ファイルから削除"))
    {
        settings.deleteApiKey = true;
        ClearApiKeyInput(settings);
    }

    if (settings.deleteApiKey)
    {
        ImGui::TextDisabled("保存すると、秘密ファイルから API キーを削除します。");
    }
    else
    {
        ImGui::TextDisabled("入力して保存すると、秘密ファイルに登録します (空なら今のまま)。");
    }

    ImGui::TextDisabled("秘密ファイル: %s", SecretsFilePath().c_str());
    ImGui::TextDisabled("API キーは https の URL にだけ送ります。");
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

namespace
{

// 「すべて既定に戻す」の確認。戻したら、画面の設定を反映して編集中の値を読み直す
void DrawResetConfirm(SettingsState& settings)
{
    ImGui::SetNextWindowPos(
        ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (!ImGui::BeginPopupModal(
            RESET_POPUP_NAME, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        return;
    }

    ImGui::TextUnformatted(
        "画面の設定・システムプロンプト・サーバーの URL・人格のテンプレートを、\n"
        "すべて既定値に戻します。\n"
        "保存した設定は消えて、元に戻せません。よろしいですか？\n"
        "(API キーは消えません)");
    ImGui::Spacing();

    const float buttonWidth = ImGui::CalcTextSize("キャンセル").x * 1.6f;

    if (ImGui::Button("既定に戻す", ImVec2(buttonWidth, 0.0f)))
    {
        try
        {
            ResetConfigToDefaults();

            settings.current = ReadUiSettings(LoadConfigOrEmpty("ui.json"));
            ApplyUiSettings(settings.current);
            ClearApiKeyInput(settings);
            settings.deleteApiKey = false;
            LoadSettings(settings);

            // チャット画面に API キーの有無を調べ直させる
            settings.saved = true;
        }
        catch (const std::exception& e)
        {
            settings.saveError = e.what();
        }

        ImGui::CloseCurrentPopup();
    }

    ImGui::SameLine();

    // Enter で消してしまわないように、キャンセルにフォーカスを置く
    if (ImGui::IsWindowAppearing())
    {
        ImGui::SetKeyboardFocusHere();
    }

    if (ImGui::Button("キャンセル", ImVec2(buttonWidth, 0.0f)))
    {
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

} // namespace

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

    ImGui::TextDisabled("保存先: %s", settings.configDirectory.c_str());

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
        settings.saved = true;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();

    ImGui::SameLine();

    if (ImGui::Button("キャンセル", ImVec2(buttonWidth, 0.0f)))
    {
        // 見ながら変えた不透明度と大きさを元に戻す
        ApplyUiSettings(settings.current);
        settings.editing = false;
        ClearApiKeyInput(settings);
        ImGui::CloseCurrentPopup();
    }

    // LOCALLLM_CONFIG_DIR を指定しているときは、そのフォルダーが既定値なので戻せない
    // (読めないファイルがあるときも、既定に戻せば直せる)
    if (CanResetConfig())
    {
        const char* resetLabel = "すべて既定に戻す";
        const float resetWidth =
            ImGui::CalcTextSize(resetLabel).x + style.FramePadding.x * 2.0f;
        ImGui::SameLine(ImGui::GetWindowWidth() - style.WindowPadding.x - resetWidth);

        if (ImGui::Button(resetLabel))
        {
            ImGui::OpenPopup(RESET_POPUP_NAME);
        }

        DrawResetConfirm(settings);
    }

    ImGui::EndPopup();
}
