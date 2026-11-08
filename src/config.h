#pragma once

#include <nlohmann/json.hpp>

#include <string>

// 設定ファイル (JSON) を読む
// 環境変数 LOCALLLM_CONFIG_DIR、無ければユーザーごとのフォルダーの config/ から探す (Python 版と共有)
// macOS: ~/Library/Application Support/LocalLLM/config/
// Linux: $XDG_CONFIG_HOME/LocalLLM/config/ (無ければ ~/.config/LocalLLM/config/)
// Windows: %APPDATA%\LocalLLM\config
// 無いファイルは、初めて使うときに既定値 (実行ファイルの隣の config/) からコピーする
// 呼ぶたびに読み直すので、ファイルを書き換えればアプリを再起動しなくても反映される
// ファイルが無い・JSON のオブジェクトとして読めないときは std::runtime_error を投げる
nlohmann::json LoadConfig(const std::string& fileName);

// 設定ファイルを読む。読めなければログに書いて空のオブジェクトを返す
// (画面の設定など、読めなくても既定値で動かしたいもの向け)
nlohmann::json LoadConfigOrEmpty(const std::string& fileName);

// config の key の値を読む。無い・型が違うときは fallback を返す
float ConfigNumberOr(const nlohmann::json& config, const std::string& key, float fallback);
std::string ConfigStringOr(
    const nlohmann::json& config, const std::string& key, const std::string& fallback);

// 設定ファイルの key の文字列を読む (無い・文字列でないときは std::runtime_error)
std::string ConfigString(const std::string& fileName, const std::string& key);

// 設定画面で書き換えるために、項目の順番を保ったまま読む
// ファイルが無ければ空のオブジェクト、JSON のオブジェクトとして読めなければ std::runtime_error
// (壊れたファイルを上書きして中身を消さないように、読めないときは保存させない)
nlohmann::ordered_json LoadConfigForEdit(const std::string& fileName);

// 設定ファイルに書く (一時ファイルに書いてから置き換えるので、途中で失敗しても元のファイルは残る)
// 失敗したら std::runtime_error
void SaveConfig(const std::string& fileName, const nlohmann::ordered_json& config);

// 設定ファイルを読み書きするフォルダー (設定画面に出す)
std::string ConfigDirectoryPath();

// 設定を既定値に戻せるか (LOCALLLM_CONFIG_DIR を指定しているときは、そのフォルダーが既定値なので戻せない)
bool CanResetConfig();

// ユーザーごとの設定を既定値で上書きする (CanResetConfig が false なら何もしない)
// 失敗したら std::runtime_error
void ResetConfigToDefaults();

// API キー (ChatGPT など) の値は設定ファイルに書かず、名前 (例: OPENAI_API_KEY) で探す
// 1. 同じ名前の環境変数
// 2. ユーザーごとの秘密ファイル (SecretsFilePath)
enum class ApiKeySource
{
    None,
    Environment,
    SecretsFile
};

struct ApiKey
{
    std::string value;
    ApiKeySource source = ApiKeySource::None;
};

// API キーを探す。見つからなければ value が空 (source は None)
ApiKey FindApiKey(const std::string& name);

// 秘密ファイルの場所 (リポジトリやビルドフォルダーの外。環境変数 LOCALLLM_SECRETS_FILE で変えられる)
// macOS: ~/Library/Application Support/LocalLLM/secrets.json
// Linux: $XDG_CONFIG_HOME/LocalLLM/secrets.json (無ければ ~/.config/LocalLLM/secrets.json)
// Windows: %APPDATA%\LocalLLM\secrets.json
std::string SecretsFilePath();

// 秘密ファイルに API キーを書く (value が空なら消す)
// macOS / Linux では、ファイルを本人だけが読み書きできるようにする (0600)
// 失敗したら std::runtime_error
void SaveApiKey(const std::string& name, const std::string& value);
