#pragma once

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <string>
#include <vector>

// 画面の色 (config/ui.json の colors に "#RRGGBB" で書く。透明度は使わない)
struct UiColors
{
    ImVec4 background = Rgb(0x0F, 0x0F, 0x0F); // チャット画面の背景 (透け具合は windowOpacity)
    ImVec4 text = Rgb(0xFF, 0xFF, 0xFF);       // 文字
    ImVec4 subText = Rgb(0x80, 0x80, 0x80);    // 補足の文字 (考え中・参照した資料・RAG の状態)
    ImVec4 user = Rgb(0x73, 0xBF, 0xFF);       // 送信者名「You」
    ImVec4 ai = Rgb(0x8C, 0xE6, 0x8C);         // 回答したエージェントの名前
    ImVec4 error = Rgb(0xFF, 0x66, 0x66);      // エラー

    static constexpr ImVec4 Rgb(int r, int g, int b)
    {
        return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
    }
};

// 画面の設定 (config/ui.json)。書いていない・正しくない項目は次の既定値を使う
struct UiSettings
{
    // チャット画面の背景の不透明度 (0〜1、文字は不透明のまま)
    float windowOpacity = 0.85f;

    // フォントファイル (相対パスは実行ファイルのフォルダーから) と大きさ (ピクセル)
    std::string fontFile = "Asset/MPLUSRounded1c-Bold.ttf";
    float fontSize = 18.0f;

    UiColors colors;
};

// ui.json の値から画面の設定を作る (無い・正しくない項目はログに書いて既定値)
UiSettings ReadUiSettings(const nlohmann::json& ui);

// 背景の不透明度・文字の大きさ・色を ImGui に反映する (次のフレームから)
// 送信者名とエラーの色は、描くときに ActiveUiSettings から読む
// フォントファイルは起動時に読み込むので、ここでは変わらない
void ApplyUiSettings(const UiSettings& ui);

// 設定画面で編集するエージェント (config/agents.json の1件)
struct AgentSettings
{
    std::string key;
    std::string name;

    // 1行が system_prompt の配列の1要素
    std::string prompt;

    // 元の system_prompt が文字列だったか (保存するときに形を変えない)
    bool promptIsString = false;

    // テンプレートの選択欄で選んでいるもの (SettingsState::templates の番号)
    int templateIndex = 0;

    // 回答役のみ: ChatGPT に聞き直した回答を、この口調で書き直すか
    bool rewrite = true;
};

// system プロンプトのテンプレート (config/personas.json の1件)
// 設定画面で回答役の名前とプロンプトに読み込んで、そこから直して使う
struct PromptTemplate
{
    std::string name;
    std::string prompt;
    bool promptIsString = false;
};

// 設定画面の状態 (UIスレッドだけが触る)
struct SettingsState
{
    // true にすると次のフレームで設定画面を開く
    bool requestOpen = false;

    // 適用中の画面の設定 (キャンセルしたときに戻す値)
    UiSettings current;

    // 設定画面を開いているか (開いている間は編集中の値で画面を描く)
    bool editing = false;

    // 編集中の値
    UiSettings ui;
    std::string chatUrl;
    std::string embedUrl;

    // llama-server の自動起動 (server.json の managed)
    bool managedEnabled = false;
    std::string managedServerPath;
    std::string managedChatModel;
    std::string managedEmbedModel;
    int managedContext = 4096;
    int managedEmbedContext = 8192;
    int managedGpuLayers = 99;

    std::vector<AgentSettings> agents;
    std::vector<PromptTemplate> templates;

    // クラウド (ChatGPT。server.json の cloud)
    std::string cloudName;
    std::string cloudUrl;
    std::string cloudModel;
    std::string cloudKeyName;

    // 新しく登録する API キー (空なら変えない。保存したら消す)
    std::string apiKeyInput;

    // 保存するときに秘密ファイルの API キーを消すか
    bool deleteApiKey = false;

    // 今の API キーの状態 (keyStatusName のキーを調べたもの)
    std::string keyStatusName;
    std::string keyStatus;

    // 保存したら true (チャット画面が API キーの有無を調べ直したら false に戻す)
    bool saved = false;

    // 開いたときに読んだファイルの中身 (設定画面で扱わない項目もそのまま残して保存する)
    nlohmann::ordered_json uiJson;
    nlohmann::ordered_json serverJson;
    nlohmann::ordered_json agentsJson;

    // 設定ファイルを読み書きするフォルダー (開いたときに調べて表示する)
    std::string configDirectory;

    // 読めなかった・保存できなかったときのメッセージ
    std::string loadError;
    std::string templateError;
    std::string saveError;
};

// 画面を描くのに使う設定 (設定画面を開いている間は、見ながら調整できるように編集中の値)
const UiSettings& ActiveUiSettings(const SettingsState& settings);

// 設定画面 (モーダル) を描く。DrawUI の中から毎フレーム呼ぶ
void DrawSettings(SettingsState& settings);
