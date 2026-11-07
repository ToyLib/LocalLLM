#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

// 画面の設定 (config/ui.json)。書いていない・正しくない項目は次の既定値を使う
struct UiSettings
{
    // チャット画面の背景の不透明度 (0〜1、文字は不透明のまま)
    float windowOpacity = 0.85f;

    // フォントファイル (相対パスは実行ファイルのフォルダーから) と大きさ (ピクセル)
    std::string fontFile = "Asset/MPLUSRounded1c-Bold.ttf";
    float fontSize = 18.0f;
};

// ui.json の値から画面の設定を作る (無い・正しくない項目はログに書いて既定値)
UiSettings ReadUiSettings(const nlohmann::json& ui);

// 背景の不透明度と文字の大きさを ImGui に反映する (次のフレームから)
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
};

// 設定画面の状態 (UIスレッドだけが触る)
struct SettingsState
{
    // true にすると次のフレームで設定画面を開く
    bool requestOpen = false;

    // 適用中の画面の設定 (キャンセルしたときに戻す値)
    UiSettings current;

    // 編集中の値
    UiSettings ui;
    std::string chatUrl;
    std::string embedUrl;
    std::vector<AgentSettings> agents;

    // 開いたときに読んだファイルの中身 (設定画面で扱わない項目もそのまま残して保存する)
    nlohmann::ordered_json uiJson;
    nlohmann::ordered_json serverJson;
    nlohmann::ordered_json agentsJson;

    // 読めなかった・保存できなかったときのメッセージ
    std::string loadError;
    std::string saveError;
};

// 設定画面 (モーダル) を描く。DrawUI の中から毎フレーム呼ぶ
void DrawSettings(SettingsState& settings);
