#include "config.h"

#include <SDL3/SDL.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace
{

fs::path Utf8ToPath(const std::string& text)
{
    return fs::path(std::u8string(text.begin(), text.end()));
}

std::string PathToUtf8(const fs::path& path)
{
    const std::u8string text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

fs::path ConfigDirectory()
{
    if (const char* env = SDL_getenv("LOCALLLM_CONFIG_DIR"))
    {
        return Utf8ToPath(env);
    }

    const char* basePath = SDL_GetBasePath();
    return Utf8ToPath(basePath ? basePath : "") / "config";
}

// ファイルを読む。開けなければ false
bool ReadFile(const fs::path& path, std::string& text)
{
    std::ifstream file(path, std::ios::binary);

    if (!file)
    {
        return false;
    }

    text.assign(std::istreambuf_iterator<char>(file), {});
    return true;
}

} // namespace

json LoadConfig(const std::string& fileName)
{
    const fs::path path = ConfigDirectory() / Utf8ToPath(fileName);
    std::ifstream file(path, std::ios::binary);

    if (!file)
    {
        throw std::runtime_error(
            "設定ファイルを開けません: " + PathToUtf8(path));
    }

    json config = json::parse(file, nullptr, false);

    if (config.is_discarded() || !config.is_object())
    {
        throw std::runtime_error(
            "設定ファイルを JSON として読めません: " + PathToUtf8(path));
    }

    return config;
}

json LoadConfigOrEmpty(const std::string& fileName)
{
    try
    {
        return LoadConfig(fileName);
    }
    catch (const std::exception& e)
    {
        SDL_Log("%s (using defaults)", e.what());
        return json::object();
    }
}

float ConfigNumberOr(const json& config, const std::string& key, float fallback)
{
    if (!config.contains(key))
    {
        return fallback;
    }

    if (!config[key].is_number())
    {
        SDL_Log("Config %s is not a number (using default)", key.c_str());
        return fallback;
    }

    return config[key].get<float>();
}

std::string ConfigStringOr(
    const json& config, const std::string& key, const std::string& fallback)
{
    if (!config.contains(key))
    {
        return fallback;
    }

    if (!config[key].is_string())
    {
        SDL_Log("Config %s is not a string (using default)", key.c_str());
        return fallback;
    }

    return config[key].get<std::string>();
}

std::string ConfigString(const std::string& fileName, const std::string& key)
{
    const json config = LoadConfig(fileName);

    if (!config.contains(key) || !config[key].is_string())
    {
        throw std::runtime_error(
            "設定ファイル " + fileName + " に " + key + " (文字列) がありません");
    }

    return config[key].get<std::string>();
}

nlohmann::ordered_json LoadConfigForEdit(const std::string& fileName)
{
    const fs::path path = ConfigDirectory() / Utf8ToPath(fileName);

    if (!fs::exists(path))
    {
        return nlohmann::ordered_json::object();
    }

    std::string text;

    if (!ReadFile(path, text))
    {
        throw std::runtime_error(
            "設定ファイルを開けません: " + PathToUtf8(path));
    }

    nlohmann::ordered_json config =
        nlohmann::ordered_json::parse(text, nullptr, false);

    if (config.is_discarded() || !config.is_object())
    {
        throw std::runtime_error(
            "設定ファイルを JSON として読めません: " + PathToUtf8(path));
    }

    return config;
}

void SaveConfig(const std::string& fileName, const nlohmann::ordered_json& config)
{
    const fs::path directory = ConfigDirectory();
    const fs::path path = directory / Utf8ToPath(fileName);
    fs::path temporary = path;
    temporary += ".tmp";

    std::error_code error;
    fs::create_directories(directory, error);

    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file << config.dump(2) << "\n";

        if (!file)
        {
            throw std::runtime_error(
                "設定ファイルを書けません: " + PathToUtf8(temporary));
        }
    }

    fs::rename(temporary, path, error);

    if (error)
    {
        fs::remove(temporary, error);
        throw std::runtime_error(
            "設定ファイルを置き換えられません: " + PathToUtf8(path));
    }
}
