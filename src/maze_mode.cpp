// 迷路: 日本語の指示を LLM に移動命令 (JSON) へ訳させる

#include "maze_mode.h"

#include "agent.h"
#include "chat.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <random>
#include <stdexcept>

namespace
{

// 指示を命令に訳す係の system プロンプト
// 盤面 (ロボットの位置) は見せず、言葉を命令に置き換えることだけをさせる
// 向きの言葉は、断りが無ければ画面から見た向きにする (ロボットから見た右と取り違えないように)
constexpr const char* TRANSLATOR_PROMPT =
    "あなたは、ロボットへの日本語の指示を移動命令に変換する係です。"
    "ロボットはマス目の上を動きます。"
    "dir は画面から見た向きで、上は up、下は down、左は left、右は right です。"
    "向きを表す言葉 (右・上・北・3時の方向など) は、特に断りが無ければ、すべて画面から見た向きです。"
    "steps は進むマス数 (1〜5) です。数が書かれていなければ 1 にします。"
    "「ぶつかるまで」「行けるところまで」「行けるだけ」「突き当たりまで」「ずっと」のように、"
    "止まる場所まで進む指示なら until_blocked を true にします (steps は 1 にしておきます)。それ以外は false です。"
    "「しばらく」「ちょっと」のようなあいまいな言い方は、あなたの判断でマス数を決めます。"
    "ロボットから見た向きで言われたときは、dir に次の値を使います。向きの計算はしなくてよく、言葉をそのまま置き換えます。"
    "まっすぐ・前へ・そのまま進む → forward、振り返って・後ろへ・来た方へ戻る・向いている方の反対へ → back、"
    "時計回りに曲がって → turn_cw、反時計回りに曲がって → turn_ccw。"
    "turn_cw と turn_ccw は、曲がってから、そのまま steps マス (until_blocked なら進めるところまで) 進む 1 つの命令です。"
    "曲がった後の移動を forward に分けないでください。"
    "指示に書かれた順に、命令を並べます。"
    "移動と関係の無い指示なら、moves を空にします。"
    "指示に無い移動を足さないでください。";

// 向きの言葉を禁止するステージで足す説明 (小さいモデルでも言い換えが通じるように、よくある言い方の常識を添える)
constexpr const char* HINT_PROMPT =
    "時計の文字盤の向きは、12時が up、3時が right、6時が down、9時が left です。"
    "空・天井・頭の方は up、地面・床・足もとの方は down です。"
    "お箸を持つ手の方は right、お茶碗を持つ手の方は left です。";

constexpr int TRANSLATOR_MAX_TOKENS = 512;

// 1 回の指示で受け付ける命令の数
constexpr int MAX_MOVES = 10;

json MovesSchema()
{
    const json move = {
        {"type", "object"},
        {"properties",
         {{"dir",
           {{"type", "string"},
            {"enum", json::array({"up", "down", "left", "right",
                                  "forward", "back", "turn_cw", "turn_ccw"})}}},
          {"steps", {{"type", "integer"}, {"minimum", 1}, {"maximum", MAZE_MAX_STEPS}}},
          {"until_blocked", {{"type", "boolean"}}}}},
        {"required", json::array({"dir", "steps", "until_blocked"})}};

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
        else if (dir == "forward")
        {
            move.turn = MazeTurn::Forward;
        }
        else if (dir == "back")
        {
            move.turn = MazeTurn::Back;
        }
        else if (dir == "turn_cw")
        {
            move.turn = MazeTurn::Clockwise;
        }
        else if (dir == "turn_ccw")
        {
            move.turn = MazeTurn::Counterclockwise;
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

        const auto until = item.find("until_blocked");
        move.untilBlocked = until != item.end() && until->is_boolean() && until->get<bool>();

        result.push_back(move);

        if (static_cast<int>(result.size()) >= MAX_MOVES)
        {
            break;
        }
    }

    return result;
}

const char* DirName(MazeDir dir)
{
    switch (dir)
    {
    case MazeDir::Up:
        return "up";
    case MazeDir::Down:
        return "down";
    case MazeDir::Left:
        return "left";
    case MazeDir::Right:
        return "right";
    }

    return "up";
}

const char* TurnName(MazeTurn turn)
{
    switch (turn)
    {
    case MazeTurn::Forward:
        return "forward";
    case MazeTurn::Back:
        return "back";
    case MazeTurn::Clockwise:
        return "turn_cw";
    case MazeTurn::Counterclockwise:
        return "turn_ccw";
    case MazeTurn::None:
        break;
    }

    return "";
}

// ロボットの向きは LLM に見せない (向きからの計算はアプリが行う)
void Translate(
    MazeState& state, int session, std::shared_ptr<std::atomic<bool>> cancel, std::string text, bool hints)
{
    Agent agent;
    agent.name = "迷路";
    agent.systemPrompt = TRANSLATOR_PROMPT;

    if (hints)
    {
        agent.systemPrompt += HINT_PROMPT;
    }

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

    const bool hints = GetMazeStage(state.stage).hints;
    state.worker = std::thread(Translate, std::ref(state), session, cancel, text, hints);
}

std::string DescribeMove(const MazeMove& move)
{
    const char* name = move.turn == MazeTurn::None ? DirName(move.dir) : TurnName(move.turn);
    const std::string dir = std::string("{\"dir\": \"") + name + "\", ";

    if (move.untilBlocked)
    {
        return dir + "\"until_blocked\": true}";
    }

    return dir + "\"steps\": " + std::to_string(move.steps) + "}";
}

MazeDir MazeResolveTurn(MazeDir facing, MazeTurn turn)
{
    // 時計回りの順 (画面の y は下向きなので、上 → 右 → 下 → 左)
    constexpr MazeDir ORDER[4] = {MazeDir::Up, MazeDir::Right, MazeDir::Down, MazeDir::Left};

    int index = 0;

    while (ORDER[index] != facing)
    {
        ++index;
    }

    switch (turn)
    {
    case MazeTurn::None:
    case MazeTurn::Forward:
        return facing;
    case MazeTurn::Back:
        return ORDER[(index + 2) % 4];
    case MazeTurn::Clockwise:
        return ORDER[(index + 1) % 4];
    case MazeTurn::Counterclockwise:
        return ORDER[(index + 3) % 4];
    }

    return facing;
}

int MazeStars(int talks)
{
    return talks <= 1 ? 3 : talks == 2 ? 2 : 1;
}

const MazeStage& GetMazeStage(int index)
{
    static const MazeStage STAGES[MAZE_STAGE_COUNT] = {
        {"ステージ1", "自由に指示してよい", {}, false, false},
        {"ステージ2",
         "数字禁止 (0〜9・０〜９・一〜十・ひとつ〜いつつ・one〜five)。「ぶつかるまで」などで伝えよう",
         {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
          "０", "１", "２", "３", "４", "５", "６", "７", "８", "９",
          "一", "二", "三", "四", "五", "六", "七", "八", "九", "十",
          "ひとつ", "ふたつ", "みっつ", "よっつ", "いつつ",
          "one", "two", "three", "four", "five"},
         true, false},
        {"ステージ3",
         "向きの言葉禁止 (上下左右・東西南北・うえ・みぎ・ひだり・英語)。ロボットの向き (三角) や身近なものを使って伝えよう",
         {"上", "下", "左", "右", "東", "西", "南", "北",
          "うえ", "みぎ", "ひだり", "ウエ", "シタ", "ミギ", "ヒダリ",
          "up", "down", "left", "right", "north", "south", "east", "west"},
         false, true},
    };

    return STAGES[std::clamp(index, 0, MAZE_STAGE_COUNT - 1)];
}

std::string FindBannedWord(const MazeStage& stage, const std::string& text)
{
    // 英字だけ小文字にそろえる (UTF-8 の多バイト文字は 0x80 以上なので変わらない)
    std::string lower = text;

    for (char& c : lower)
    {
        if (c >= 'A' && c <= 'Z')
        {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }

    for (const std::string& word : stage.banned)
    {
        if (lower.find(word) != std::string::npos)
        {
            return word;
        }
    }

    return "";
}

void ResetMaze(MazeState& state, bool newBoard)
{
    StopWorker(state);

    {
        std::lock_guard lock(state.mutex);
        state.error.clear();
        state.plan.clear();
    }

    if (newBoard)
    {
        state.board = GenerateMazeBoard(GetMazeStage(state.stage).slideOnly);
    }

    state.warning.clear();

    state.talks = 0;
    state.cleared = false;
    state.robot = MAZE_START;
    state.from = MAZE_START;
    state.facing = MazeDir::Right;
    state.trail = {MAZE_START};
    state.pending.clear();
    state.moving = false;
    state.executing = false;
    state.movedSteps = 0;
    state.progress = 0.0f;
    state.bump = 0.0f;
    state.log.clear();
    state.focusInput = true;
}

MazeCell MazeNeighbor(MazeCell cell, MazeDir dir)
{
    switch (dir)
    {
    case MazeDir::Up:
        --cell.y;
        break;
    case MazeDir::Down:
        ++cell.y;
        break;
    case MazeDir::Left:
        --cell.x;
        break;
    case MazeDir::Right:
        ++cell.x;
        break;
    }

    return cell;
}

bool MazeInside(MazeCell cell)
{
    return cell.x >= 0 && cell.x < MAZE_SIZE && cell.y >= 0 && cell.y < MAZE_SIZE;
}

// (マス, 向き) を頂点にして、まっすぐ進むのは 0、曲がるのは 1 の 0-1 BFS
// スタートではどの向きを向いてもよい (最初の 1 歩は曲がりに数えない)
int MazeMinTurns(const MazeBoard& board)
{
    constexpr int DIRS = 4;
    constexpr int UNREACHED = 1 << 20;
    constexpr MazeDir ALL[DIRS] = {MazeDir::Up, MazeDir::Down, MazeDir::Left, MazeDir::Right};

    int cost[MAZE_SIZE][MAZE_SIZE][DIRS];

    for (auto& row : cost)
    {
        for (auto& cell : row)
        {
            std::fill(std::begin(cell), std::end(cell), UNREACHED);
        }
    }

    struct Node
    {
        MazeCell cell;
        int dir;
    };

    std::deque<Node> queue;

    for (int d = 0; d < DIRS; ++d)
    {
        cost[MAZE_START.y][MAZE_START.x][d] = 0;
        queue.push_back({MAZE_START, d});
    }

    while (!queue.empty())
    {
        const Node node = queue.front();
        queue.pop_front();
        const int current = cost[node.cell.y][node.cell.x][node.dir];

        for (int d = 0; d < DIRS; ++d)
        {
            const MazeCell next = MazeNeighbor(node.cell, ALL[d]);

            if (!MazeInside(next) || board.Blocked(next))
            {
                continue;
            }

            const int weight = d == node.dir ? 0 : 1;

            if (current + weight < cost[next.y][next.x][d])
            {
                cost[next.y][next.x][d] = current + weight;

                if (weight == 0)
                {
                    queue.push_front({next, d});
                }
                else
                {
                    queue.push_back({next, d});
                }
            }
        }
    }

    const int* goal = cost[MAZE_GOAL.y][MAZE_GOAL.x];
    const int best = *std::min_element(goal, goal + DIRS);

    return best >= UNREACHED ? -1 : best;
}

bool MazeSlideSolvable(const MazeBoard& board)
{
    constexpr MazeDir ALL[] = {MazeDir::Up, MazeDir::Down, MazeDir::Left, MazeDir::Right};

    bool visited[MAZE_SIZE][MAZE_SIZE] = {};
    std::deque<MazeCell> queue{MAZE_START};
    visited[MAZE_START.y][MAZE_START.x] = true;

    while (!queue.empty())
    {
        const MazeCell cell = queue.front();
        queue.pop_front();

        for (MazeDir dir : ALL)
        {
            MazeCell stop = cell;

            for (MazeCell next = MazeNeighbor(stop, dir); MazeInside(next) && !board.Blocked(next);
                 next = MazeNeighbor(stop, dir))
            {
                stop = next;

                if (stop == MAZE_GOAL)
                {
                    return true;
                }
            }

            if (!visited[stop.y][stop.x])
            {
                visited[stop.y][stop.x] = true;
                queue.push_back(stop);
            }
        }
    }

    return false;
}

MazeBoard GenerateMazeBoard(bool slideOnly)
{
    static std::mt19937 random{std::random_device{}()};

    std::vector<MazeCell> cells;

    for (int y = 0; y < MAZE_SIZE; ++y)
    {
        for (int x = 0; x < MAZE_SIZE; ++x)
        {
            const MazeCell cell{x, y};

            if (!(cell == MAZE_START) && !(cell == MAZE_GOAL))
            {
                cells.push_back(cell);
            }
        }
    }

    // 条件に合う盤が出なかったときは、曲がる回数が一番多かった盤 (障害物なしの盤より悪くはしない)
    MazeBoard best;
    best.minTurns = MazeMinTurns(best);

    // 条件に合う盤が出るまで作り直す (6×6 なら、たいてい数回で出る)
    for (int attempt = 0; attempt < 2000; ++attempt)
    {
        std::shuffle(cells.begin(), cells.end(), random);
        const int count = std::uniform_int_distribution<int>(MAZE_MIN_BLOCKS, MAZE_MAX_BLOCKS)(random);

        MazeBoard board;

        for (int i = 0; i < count; ++i)
        {
            board.blocked[cells[i].y][cells[i].x] = true;
        }

        board.minTurns = MazeMinTurns(board);

        const bool solvable = !slideOnly || MazeSlideSolvable(board);

        if (solvable && board.minTurns >= MAZE_MIN_TURNS)
        {
            return board;
        }

        if (solvable && board.minTurns > best.minTurns)
        {
            best = board;
        }
    }

    return best;
}

void SelectMazeStage(MazeState& state, int stage)
{
    state.stage = std::clamp(stage, 0, MAZE_STAGE_COUNT - 1);
    ResetMaze(state, true);
}

void ShutdownMaze(MazeState& state)
{
    StopWorker(state);
}
