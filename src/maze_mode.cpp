// 迷路: 日本語の指示を LLM に移動命令 (JSON) へ訳させる

#include "maze_mode.h"

#include "agent.h"
#include "chat.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace
{

// 指示を命令に訳す係の system プロンプト
// 盤面 (ロボットの位置) は見せず、言葉を命令に置き換えることだけをさせる
constexpr const char* TRANSLATOR_PROMPT =
    "あなたは、ロボットへの日本語の指示を移動命令に変換する係です。"
    "ロボットはマス目の上を動きます。"
    "dir は画面から見た向きで、上は up、下は down、左は left、右は right です。"
    "steps は進むマス数 (1〜5) です。数が書かれていなければ 1 にします。"
    "「しばらく」「ちょっと」「ずっと」のようなあいまいな言い方は、あなたの判断でマス数を決めます。"
    "指示に書かれた順に、命令を並べます。"
    "移動と関係の無い指示なら、moves を空にします。"
    "指示に無い移動を足さないでください。";

constexpr int TRANSLATOR_MAX_TOKENS = 512;

// 1 回の指示で受け付ける命令の数
constexpr int MAX_MOVES = 10;

json MovesSchema()
{
    const json move = {
        {"type", "object"},
        {"properties",
         {{"dir", {{"type", "string"}, {"enum", json::array({"up", "down", "left", "right"})}}},
          {"steps", {{"type", "integer"}, {"minimum", 1}, {"maximum", MAZE_MAX_STEPS}}}}},
        {"required", json::array({"dir", "steps"})}};

    return {
        {"type", "object"},
        {"properties",
         {{"moves", {{"type", "array"}, {"items", move}, {"maxItems", MAX_MOVES}}}}},
        {"required", json::array({"moves"})}};
}

// LLM が返した JSON を命令の列にする。形が違えば std::runtime_error
std::vector<MazeMove> ValidateMoves(const json& raw)
{
    const auto moves = raw.find("moves");

    if (moves == raw.end() || !moves->is_array())
    {
        throw std::runtime_error("命令 (moves) がありません");
    }

    std::vector<MazeMove> result;

    for (const auto& item : *moves)
    {
        if (!item.is_object())
        {
            continue;
        }

        const std::string dir = item.value("dir", "");
        MazeMove move;

        if (dir == "up")
        {
            move.dir = MazeDir::Up;
        }
        else if (dir == "down")
        {
            move.dir = MazeDir::Down;
        }
        else if (dir == "left")
        {
            move.dir = MazeDir::Left;
        }
        else if (dir == "right")
        {
            move.dir = MazeDir::Right;
        }
        else
        {
            throw std::runtime_error("知らない向きです: " + dir);
        }

        const auto steps = item.find("steps");

        if (steps == item.end() || !steps->is_number_integer())
        {
            throw std::runtime_error("マス数 (steps) がありません");
        }

        move.steps = std::clamp(steps->get<int>(), 1, MAZE_MAX_STEPS);
        result.push_back(move);

        if (static_cast<int>(result.size()) >= MAX_MOVES)
        {
            break;
        }
    }

    return result;
}

void Translate(
    MazeState& state, int session, std::shared_ptr<std::atomic<bool>> cancel, std::string text)
{
    Agent agent;
    agent.name = "迷路";
    agent.systemPrompt = TRANSLATOR_PROMPT;
    agent.options.maxTokens = TRANSLATOR_MAX_TOKENS;
    agent.options.temperature = 0.2f;
    agent.options.disableThinking = true;
    agent.options.schema = MovesSchema();

    const auto started = std::chrono::steady_clock::now();

    std::vector<MazeMove> plan;
    std::string error;

    try
    {
        const json raw = RunAgentJson(
            agent, json::array({{{"role", "user"}, {"content", text}}}), *cancel);
        plan = ValidateMoves(raw);
    }
    catch (const std::exception& e)
    {
        error = e.what();
    }

    const int elapsed = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count());

    {
        std::lock_guard lock(state.mutex);

        if (state.session != session)
        {
            return;
        }

        state.busy = false;
        state.elapsedMs = elapsed;

        if (error.empty())
        {
            state.plan = std::move(plan);
            state.hasPlan = true;
        }
        else if (!cancel->load())
        {
            state.error = error;
        }
    }

    WakeUI();
}

void StopWorker(MazeState& state)
{
    {
        std::lock_guard lock(state.mutex);
        ++state.session;
        state.cancel->store(true);
        state.busy = false;
        state.hasPlan = false;
    }

    if (state.worker.joinable())
    {
        state.worker.join();
    }
}

} // namespace

void StartMazeCommand(MazeState& state, const std::string& text)
{
    StopWorker(state);

    int session;
    std::shared_ptr<std::atomic<bool>> cancel = std::make_shared<std::atomic<bool>>(false);

    {
        std::lock_guard lock(state.mutex);
        session = state.session;
        state.cancel = cancel;
        state.busy = true;
        state.error.clear();
        state.plan.clear();
    }

    state.worker = std::thread(Translate, std::ref(state), session, cancel, text);
}

std::string DescribeMove(const MazeMove& move)
{
    const char* dir = "up";

    switch (move.dir)
    {
    case MazeDir::Up:
        dir = "up";
        break;
    case MazeDir::Down:
        dir = "down";
        break;
    case MazeDir::Left:
        dir = "left";
        break;
    case MazeDir::Right:
        dir = "right";
        break;
    }

    return std::string("{\"dir\": \"") + dir + "\", \"steps\": " + std::to_string(move.steps) + "}";
}

void ResetMaze(MazeState& state)
{
    StopWorker(state);

    {
        std::lock_guard lock(state.mutex);
        state.error.clear();
        state.plan.clear();
    }

    const MazeCell start{0, MAZE_SIZE - 1};
    state.robot = start;
    state.from = start;
    state.facing = MazeDir::Right;
    state.trail = {start};
    state.pending.clear();
    state.moving = false;
    state.executing = false;
    state.movedSteps = 0;
    state.progress = 0.0f;
    state.bump = 0.0f;
    state.log.clear();
    state.focusInput = true;
}

void ShutdownMaze(MazeState& state)
{
    StopWorker(state);
}
