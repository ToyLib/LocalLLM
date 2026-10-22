#include "rag.h"

#include "chat.h"
#include "llm_client.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <numeric>
#include <optional>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace
{

// 1チャンクの最大文字数 (UTF-8 のコードポイント数)
constexpr size_t MAX_CHUNK_CHARS = 400;

// 1回の埋め込みリクエストで送るチャンク数
constexpr size_t EMBED_BATCH = 16;

// 質問に添えるチャンクの最大数と、関係ありとみなす類似度の下限
constexpr size_t TOP_K = 3;
constexpr float MIN_SCORE = 0.5f;

// チャンクの分け方を変えたら上げる (保存済みのインデックスを作り直す)
constexpr int INDEX_VERSION = 2;

// ルーターに見せる、見出しの無い資料の書き出しの文字数
constexpr size_t PREVIEW_CHARS = 40;

// インデックスの保存先 (実行ファイルの隣)
constexpr const char* INDEX_FILE = "rag-index.json";

fs::path Utf8ToPath(const std::string& text)
{
    return fs::path(std::u8string(text.begin(), text.end()));
}

std::string PathToUtf8(const fs::path& path)
{
    const std::u8string text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

fs::path BaseDirectory()
{
    const char* basePath = SDL_GetBasePath();
    return Utf8ToPath(basePath ? basePath : "");
}

// 環境変数 LOCALLLM_RAG_DIR、無ければ実行ファイルの隣の rag/
fs::path RagDirectory()
{
    if (const char* env = SDL_getenv("LOCALLLM_RAG_DIR"))
    {
        return Utf8ToPath(env);
    }

    return BaseDirectory() / "rag";
}

void SetStatus(RagState& state, std::string status)
{
    {
        std::lock_guard lock(state.mutex);
        state.status = std::move(status);
    }
    WakeUI();
}

bool IsContinuationByte(char c)
{
    return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

size_t CountChars(std::string_view text)
{
    return static_cast<size_t>(
        std::count_if(text.begin(), text.end(), [](char c) {
            return !IsContinuationByte(c);
        }));
}

bool IsValidUtf8(std::string_view text)
{
    size_t i = 0;

    while (i < text.size())
    {
        const auto c = static_cast<unsigned char>(text[i]);

        size_t length;
        if (c < 0x80)
        {
            length = 1;
        }
        else if ((c & 0xE0) == 0xC0)
        {
            length = 2;
        }
        else if ((c & 0xF0) == 0xE0)
        {
            length = 3;
        }
        else if ((c & 0xF8) == 0xF0)
        {
            length = 4;
        }
        else
        {
            return false;
        }

        if (i + length > text.size())
        {
            return false;
        }

        for (size_t j = 1; j < length; ++j)
        {
            if (!IsContinuationByte(text[i + j]))
            {
                return false;
            }
        }

        i += length;
    }

    return true;
}

// ファイルの変更検出用 (64bit FNV-1a)
std::string HashText(std::string_view text)
{
    uint64_t hash = 14695981039346656037ull;

    for (unsigned char c : text)
    {
        hash ^= c;
        hash *= 1099511628211ull;
    }

    char buffer[17];
    std::snprintf(
        buffer, sizeof(buffer), "%016llx",
        static_cast<unsigned long long>(hash));
    return buffer;
}

// UTF-8 のテキストファイルを読む (UTF-8 でなければ nullopt)
std::optional<std::string> ReadTextFile(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary);

    if (!file)
    {
        return std::nullopt;
    }

    std::ostringstream stream;
    stream << file.rdbuf();
    std::string text = stream.str();

    if (text.starts_with("\xEF\xBB\xBF"))
    {
        text.erase(0, 3);
    }

    if (!IsValidUtf8(text))
    {
        return std::nullopt;
    }

    std::erase(text, '\r');
    return text;
}

// 見出しで区切った文書の一部分
struct Section
{
    std::string heading;
    std::vector<std::string> paragraphs;
};

// 見出しと空行で、セクションと段落に分ける
std::vector<Section> ParseSections(const std::string& text, bool markdown)
{
    std::vector<Section> sections(1);

    // 階層ごとの見出し (headings[0] が # の見出し)
    std::vector<std::string> headings;

    std::string paragraph;
    bool inCodeBlock = false;

    auto endParagraph = [&] {
        if (paragraph.find_first_not_of(" \t\n") != std::string::npos)
        {
            sections.back().paragraphs.push_back(paragraph);
        }
        paragraph.clear();
    };

    std::istringstream stream(text);
    std::string line;

    while (std::getline(stream, line))
    {
        if (markdown && line.starts_with("```"))
        {
            inCodeBlock = !inCodeBlock;
        }

        if (markdown && !inCodeBlock)
        {
            const size_t level = line.find_first_not_of('#');

            if (level >= 1 && level <= 6 &&
                level < line.size() && line[level] == ' ')
            {
                endParagraph();

                headings.resize(level - 1);
                headings.push_back(line.substr(level + 1));

                // 「文書名 > 章 > 節」の形にする
                std::string path;
                for (const auto& heading : headings)
                {
                    if (heading.empty())
                    {
                        continue;
                    }
                    if (!path.empty())
                    {
                        path += " > ";
                    }
                    path += heading;
                }

                sections.push_back({path, {}});
                continue;
            }
        }

        if (!inCodeBlock &&
            line.find_first_not_of(" \t") == std::string::npos)
        {
            endParagraph();
            continue;
        }

        if (!paragraph.empty())
        {
            paragraph += '\n';
        }
        paragraph += line;
    }

    endParagraph();
    return sections;
}

// MAX_CHUNK_CHARS 文字ごとに切る
void SplitByLength(std::string_view text, std::vector<std::string>& out)
{
    size_t start = 0;
    size_t chars = 0;

    for (size_t i = 0; i < text.size(); ++i)
    {
        if (IsContinuationByte(text[i]))
        {
            continue;
        }

        if (chars == MAX_CHUNK_CHARS)
        {
            out.emplace_back(text.substr(start, i - start));
            start = i;
            chars = 0;
        }
        ++chars;
    }

    out.emplace_back(text.substr(start));
}

std::vector<RagChunk> SplitIntoChunks(
    const std::string& source,
    const std::string& text,
    bool markdown)
{
    std::vector<RagChunk> chunks;

    for (const auto& section : ParseSections(text, markdown))
    {
        // 長すぎる段落は行で、それでも長い行は文字数で分ける
        std::vector<std::string> units;

        for (const auto& paragraph : section.paragraphs)
        {
            if (CountChars(paragraph) <= MAX_CHUNK_CHARS)
            {
                units.push_back(paragraph);
                continue;
            }

            std::istringstream stream(paragraph);
            std::string line;

            while (std::getline(stream, line))
            {
                if (CountChars(line) <= MAX_CHUNK_CHARS)
                {
                    units.push_back(line);
                }
                else
                {
                    SplitByLength(line, units);
                }
            }
        }

        // 上限を超えない範囲で段落をまとめる
        // (見出しの無いテキストは話題が混ざらないように段落ごとに分ける)
        std::string current;
        size_t chars = 0;

        auto flush = [&] {
            if (!current.empty())
            {
                chunks.push_back({source, section.heading, current, {}});
            }
            current.clear();
            chars = 0;
        };

        for (const auto& unit : units)
        {
            const size_t length = CountChars(unit);

            if (!current.empty() &&
                (!markdown || chars + length > MAX_CHUNK_CHARS))
            {
                flush();
            }

            if (!current.empty())
            {
                current += "\n\n";
                chars += 2;
            }

            current += unit;
            chars += length;
        }

        flush();
    }

    return chunks;
}

// 埋め込みに使う文章 (見出しを付けると、どの話題か伝わりやすい)
std::string EmbeddingText(const RagChunk& chunk)
{
    const std::string title = chunk.heading.empty()
        ? PathToUtf8(Utf8ToPath(chunk.source).stem())
        : chunk.heading;

    return title + "\n" + chunk.text;
}

void Normalize(std::vector<float>& vector)
{
    const float norm = std::sqrt(std::inner_product(
        vector.begin(), vector.end(), vector.begin(), 0.0f));

    if (norm > 0.0f)
    {
        for (float& value : vector)
        {
            value /= norm;
        }
    }
}

// 保存済みインデックスの1ファイル分
struct IndexedFile
{
    std::string hash;
    std::vector<RagChunk> chunks;
};

// rag/ からの相対パス → ファイル
using IndexMap = std::map<std::string, IndexedFile>;

IndexMap LoadIndex(const fs::path& path)
{
    IndexMap index;

    std::ifstream file(path, std::ios::binary);

    if (!file)
    {
        return index;
    }

    const json data = json::parse(file, nullptr, false);

    if (data.is_discarded() ||
        data.value("version", 0) != INDEX_VERSION)
    {
        return index;
    }

    try
    {
        for (const auto& [source, entry] : data.at("files").items())
        {
            IndexedFile indexed;
            indexed.hash = entry.at("hash").get<std::string>();

            for (const auto& chunk : entry.at("chunks"))
            {
                indexed.chunks.push_back({
                    source,
                    chunk.at("heading").get<std::string>(),
                    chunk.at("text").get<std::string>(),
                    chunk.at("vector").get<std::vector<float>>()
                });
            }

            index[source] = std::move(indexed);
        }
    }
    catch (const json::exception&)
    {
        // 壊れていたら作り直す
        index.clear();
    }

    return index;
}

void SaveIndex(const fs::path& path, const IndexMap& index)
{
    json files = json::object();

    for (const auto& [source, indexed] : index)
    {
        json chunks = json::array();

        for (const auto& chunk : indexed.chunks)
        {
            chunks.push_back({
                {"heading", chunk.heading},
                {"text", chunk.text},
                {"vector", chunk.vector}
            });
        }

        files[source] = {
            {"hash", indexed.hash},
            {"chunks", chunks}
        };
    }

    const json data = {
        {"version", INDEX_VERSION},
        {"files", files}
    };

    // 書き込み途中で落ちても壊れないように、一時ファイルから置き換える
    fs::path temp = path;
    temp += ".tmp";

    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);

        if (!file)
        {
            throw std::runtime_error(
                "Cannot write " + PathToUtf8(temp));
        }

        file << data.dump();
    }

    fs::rename(temp, path);
}

std::string LowerExtension(const fs::path& path)
{
    std::string extension = PathToUtf8(path.extension());

    std::transform(
        extension.begin(), extension.end(), extension.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    return extension;
}

// rag/ の中の .md / .txt を集める (隠しファイルと README.md は除く)
std::vector<fs::path> FindDocuments(const fs::path& directory)
{
    std::vector<fs::path> files;
    std::error_code error;

    auto it = fs::recursive_directory_iterator(
        directory, fs::directory_options::skip_permission_denied, error);

    for (; !error && it != fs::recursive_directory_iterator();
         it.increment(error))
    {
        const fs::path& path = it->path();

        if (path.filename().u8string().starts_with(u8"."))
        {
            if (it->is_directory(error))
            {
                it.disable_recursion_pending();
            }
            continue;
        }

        if (!it->is_regular_file(error))
        {
            continue;
        }

        // rag/README.md はフォルダの説明なので対象外
        if (path == directory / "README.md")
        {
            continue;
        }

        const std::string extension = LowerExtension(path);

        if (extension == ".md" || extension == ".markdown" ||
            extension == ".txt")
        {
            files.push_back(path);
        }
    }

    std::sort(files.begin(), files.end());
    return files;
}

void BuildIndex(RagState& state)
{
    const fs::path directory = RagDirectory();
    const fs::path indexPath = BaseDirectory() / INDEX_FILE;

    std::error_code error;

    if (!fs::is_directory(directory, error))
    {
        std::lock_guard lock(state.mutex);
        state.chunks.clear();
        state.status = "フォルダがありません: " + PathToUtf8(directory);
        return;
    }

    const IndexMap previous = LoadIndex(indexPath);
    IndexMap next;
    size_t skipped = 0;

    for (const auto& path : FindDocuments(directory))
    {
        const std::string source =
            PathToUtf8(path.lexically_relative(directory));

        const auto text = ReadTextFile(path);

        if (!text)
        {
            SDL_Log("RAG: skipped (not UTF-8): %s", source.c_str());
            ++skipped;
            continue;
        }

        const std::string hash = HashText(*text);

        // 変更の無いファイルは埋め込み済みのチャンクを使い回す
        if (auto found = previous.find(source);
            found != previous.end() && found->second.hash == hash)
        {
            next[source] = found->second;
            continue;
        }

        const bool markdown = LowerExtension(path) != ".txt";

        next[source] = {hash, SplitIntoChunks(source, *text, markdown)};
    }

    // まだ埋め込んでいないチャンクを埋め込む
    std::vector<RagChunk*> pending;

    for (auto& [source, indexed] : next)
    {
        for (auto& chunk : indexed.chunks)
        {
            if (chunk.vector.empty())
            {
                pending.push_back(&chunk);
            }
        }
    }

    for (size_t i = 0; i < pending.size(); i += EMBED_BATCH)
    {
        SetStatus(
            state,
            "インデックス作成中 " + std::to_string(i) + " / " +
            std::to_string(pending.size()));

        const size_t end = std::min(i + EMBED_BATCH, pending.size());

        std::vector<std::string> inputs;
        for (size_t j = i; j < end; ++j)
        {
            inputs.push_back(EmbeddingText(*pending[j]));
        }

        auto vectors = Embed(inputs, state.cancel);

        for (size_t j = i; j < end; ++j)
        {
            pending[j]->vector = std::move(vectors[j - i]);
            Normalize(pending[j]->vector);
        }
    }

    SaveIndex(indexPath, next);

    std::vector<RagChunk> chunks;

    for (auto& [source, indexed] : next)
    {
        for (auto& chunk : indexed.chunks)
        {
            chunks.push_back(std::move(chunk));
        }
    }

    std::string status =
        std::to_string(next.size()) + " ファイル / " +
        std::to_string(chunks.size()) + " チャンク";

    if (skipped > 0)
    {
        status += " (UTF-8 でない " + std::to_string(skipped) +
            " ファイルを除外)";
    }

    std::lock_guard lock(state.mutex);
    state.chunks = std::move(chunks);
    state.status = std::move(status);
}

} // namespace

void StartRagIndexing(RagState& state)
{
    JoinRagWorker(state);

    {
        std::lock_guard lock(state.mutex);
        state.building = true;
        state.status = "インデックスを準備中...";
    }

    state.cancel = false;

    state.worker = std::thread([&state] {
        try
        {
            BuildIndex(state);
        }
        catch (const std::exception& e)
        {
            // 失敗しても前回までのインデックスは使えるように残す
            std::lock_guard lock(state.mutex);
            state.status =
                std::string("インデックス作成に失敗: ") + e.what();
        }

        {
            std::lock_guard lock(state.mutex);
            state.building = false;
        }
        WakeUI();
    });
}

void JoinRagWorker(RagState& state)
{
    if (state.worker.joinable())
    {
        state.worker.join();
    }
}

std::vector<RagHit> SearchRag(
    RagState& state,
    const std::string& query,
    const std::atomic<bool>& cancel)
{
    {
        std::lock_guard lock(state.mutex);

        if (state.chunks.empty())
        {
            throw std::runtime_error("インデックスがありません");
        }
    }

    std::vector<float> queryVector = Embed({query}, cancel).front();
    Normalize(queryVector);

    std::vector<RagHit> hits;

    {
        std::lock_guard lock(state.mutex);

        for (const auto& chunk : state.chunks)
        {
            if (chunk.vector.size() != queryVector.size())
            {
                continue;
            }

            // 正規化済みなので内積がコサイン類似度
            const float score = std::inner_product(
                chunk.vector.begin(), chunk.vector.end(),
                queryVector.begin(), 0.0f);

            hits.push_back({chunk.source, chunk.heading, chunk.text, score});
        }
    }

    std::sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) {
        return a.score > b.score;
    });

    // 調整用に上位の類似度をログに出す
    for (size_t i = 0; i < std::min<size_t>(hits.size(), 5); ++i)
    {
        SDL_Log(
            "RAG: %.3f %s / %s",
            hits[i].score, hits[i].source.c_str(), hits[i].heading.c_str());
    }

    std::erase_if(hits, [](const auto& hit) {
        return hit.score < MIN_SCORE;
    });

    if (hits.size() > TOP_K)
    {
        hits.resize(TOP_K);
    }

    return hits;
}

std::vector<std::string> DescribeRagSources(RagState& state)
{
    // 資料ごとの見出し (階層の最後の部分) と、最初のチャンクの本文
    struct Source
    {
        std::vector<std::string> headings;
        std::string firstText;
    };

    std::map<std::string, Source> sources;

    {
        std::lock_guard lock(state.mutex);

        for (const auto& chunk : state.chunks)
        {
            auto& source = sources[chunk.source];

            if (source.firstText.empty())
            {
                source.firstText = chunk.text;
            }

            if (chunk.heading.empty())
            {
                continue;
            }

            const size_t separator = chunk.heading.rfind(" > ");
            std::string heading = separator == std::string::npos
                ? chunk.heading
                : chunk.heading.substr(separator + 3);

            if (std::find(source.headings.begin(), source.headings.end(),
                          heading) == source.headings.end())
            {
                source.headings.push_back(std::move(heading));
            }
        }
    }

    std::vector<std::string> descriptions;

    for (const auto& [name, source] : sources)
    {
        std::string description = name + ": ";

        if (!source.headings.empty())
        {
            for (size_t i = 0; i < source.headings.size(); ++i)
            {
                description += (i == 0 ? "" : " / ") + source.headings[i];
            }
        }
        else
        {
            // 見出しが無いときは書き出しを見せる (文字の途中で切らない)
            std::string preview;
            size_t chars = 0;

            for (const char c : source.firstText)
            {
                if (c == '\n' || c == '\r')
                {
                    preview += ' ';
                    continue;
                }

                if (!IsContinuationByte(c) && ++chars > PREVIEW_CHARS)
                {
                    preview += "…";
                    break;
                }

                preview += c;
            }

            description += preview;
        }

        descriptions.push_back(std::move(description));
    }

    return descriptions;
}

std::string BuildRagPrompt(
    const std::vector<RagHit>& hits,
    const std::string& question)
{
    std::string prompt =
        "以下の資料を参考にして、最後の質問に答えてください。"
        "資料に関係する質問なら資料に基づいて答え、"
        "資料に書かれていないことは推測しないでください。"
        "資料が質問と関係なければ、資料は無視して普通に答えてください。\n\n";

    for (size_t i = 0; i < hits.size(); ++i)
    {
        prompt += "[資料" + std::to_string(i + 1) + "] " + hits[i].source;

        if (!hits[i].heading.empty())
        {
            prompt += " (" + hits[i].heading + ")";
        }

        prompt += "\n" + hits[i].text + "\n\n";
    }

    prompt += "質問: " + question;
    return prompt;
}
