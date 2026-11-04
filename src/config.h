#pragma once

#include <nlohmann/json.hpp>

#include <string>

// config/ の設定ファイル (JSON) を読む
// 環境変数 LOCALLLM_CONFIG_DIR、無ければ実行ファイルの隣の config/ から探す
// 呼ぶたびに読み直すので、ファイルを書き換えればアプリを再起動しなくても反映される
// ファイルが無い・JSON のオブジェクトとして読めないときは std::runtime_error を投げる
nlohmann::json LoadConfig(const std::string& fileName);

// 設定ファイルの key の文字列を読む (無い・文字列でないときは std::runtime_error)
std::string ConfigString(const std::string& fileName, const std::string& key);
