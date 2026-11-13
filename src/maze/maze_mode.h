#pragma once

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// 迷路: 6×6 のマス目のロボットを、日本語の指示で上下左右に動かして、障害物を避けてゴールまで運ぶ
// LLM は指示を移動命令の JSON に訳すだけ (盤面は見せない)。動かすのと判定はアプリ
// 動きは doc/maze.md を参照
// worker が書く項目は mutex で守り、アニメーションの項目は UI スレッドだけが触る

constexpr int MAZE_SIZE = 6;

// 1 つの命令で進める最大のマス数
constexpr int MAZE_MAX_STEPS = 5;

// 障害物の数と、最短の道順で曲がる回数の下限 (簡単すぎる盤を避ける)
constexpr int MAZE_MIN_BLOCKS = 9;
constexpr int MAZE_MAX_BLOCKS = 12;
constexpr int MAZE_MIN_TURNS = 3;

// 画面から見た向き (y は下に向かって増える)
enum class MazeDir
{
    Up,
    Down,
    Left,
    Right
};

// ロボットから見た向き (None なら dir をそのまま使う)
// LLM には言葉を置き換えさせるだけにして、いまの向きからの計算はアプリが実行するときに行う
// (小さいモデルは「右を向いているときの時計回り」のような計算を間違えやすいため)
// JSON では turn_cw / turn_ccw (clockwise / counterclockwise という名前だと、Qwen3 4B が取り違えやすかった)
enum class MazeTurn
{
    None,
    Forward,
    Back,
    Clockwise,
    Counterclockwise
};

// facing を向いたロボットにとっての turn の向き (画面から見た向き)
MazeDir MazeResolveTurn(MazeDir facing, MazeTurn turn);

struct MazeMove
{
    MazeDir dir = MazeDir::Up;
    MazeTurn turn = MazeTurn::None;
    int steps = 1;

    // true なら steps によらず、進めるところまで進む (障害物や盤の端の手前で止まる。ぶつかった扱いにはしない)
    bool untilBlocked = false;
};

// ステージ (禁止ワードで、指示の言い方を縛る)
constexpr int MAZE_STAGE_COUNT = 3;

struct MazeStage
{
    const char* title;

    // 画面に出すルールの説明
    const char* rule;

    // 指示に入れてはいけない言葉 (英字は大文字・小文字を区別しない)
    std::vector<std::string> banned;

    // 盤を「進めるところまで進む」だけで解けるものに限る (数字を使えないステージ用)
    bool slideOnly = false;

    // LLM に、向きの言い換えの常識 (時計の文字盤・空は上など) を教える
    bool hints = false;
};

const MazeStage& GetMazeStage(int index);

// 指示に入っている禁止ワード (無ければ空)
std::string FindBannedWord(const MazeStage& stage, const std::string& text);

// 指示の履歴の 1 件
struct MazeLogEntry
{
    std::string text;

    // LLM が返した命令 (1 行に 1 命令)
    std::vector<std::string> commands;

    // 結果 (ゴール・端にぶつかった・読み取れなかったなど)
    std::string result;
    bool failed = false;

    // 訳すのにかかった時間 (ミリ秒)
    int elapsedMs = 0;
};

struct MazeCell
{
    int x = 0;
    int y = 0;

    bool operator==(const MazeCell&) const = default;
};

// スタートは左下、ゴールは右上
constexpr MazeCell MAZE_START{0, MAZE_SIZE - 1};
constexpr MazeCell MAZE_GOAL{MAZE_SIZE - 1, 0};

// 盤面 (障害物の位置)
struct MazeBoard
{
    bool blocked[MAZE_SIZE][MAZE_SIZE] = {};

    // 最短の道順で曲がる回数 (目安として画面に出す)
    int minTurns = 0;

    bool Blocked(MazeCell cell) const { return blocked[cell.y][cell.x]; }
};

// 隣のマス (盤の外かもしれない)
MazeCell MazeNeighbor(MazeCell cell, MazeDir dir);

bool MazeInside(MazeCell cell);

// ランダムな盤を作る (スタートからゴールまで通れて、曲がる回数が MAZE_MIN_TURNS 以上)
// slideOnly なら、さらに「進めるところまで進む」だけでゴールできる盤に限る
MazeBoard GenerateMazeBoard(bool slideOnly = false);

// 「進めるところまで進む」だけでゴールに着けるか (途中でゴールを通るのも着いたことにする)
bool MazeSlideSolvable(const MazeBoard& board);

// ゴールまでに曲がる回数の最小 (通れなければ -1)
int MazeMinTurns(const MazeBoard& board);

struct MazeState
{
    std::mutex mutex;

    // 指示を送るたび・リセットのたびに増やす。古いワーカーが書き込まないための目印
    int session = 0;
    std::shared_ptr<std::atomic<bool>> cancel = std::make_shared<std::atomic<bool>>(false);
    std::thread worker;

    // ワーカーが書く (mutex で守る)
    bool busy = false;
    bool hasPlan = false;
    std::vector<MazeMove> plan;
    std::string error;
    int elapsedMs = 0;

    // ここから下は UI スレッドだけが触る
    std::string input;
    bool focusInput = true;
    std::vector<MazeLogEntry> log;

    // いまのステージ (0 から) と、ステージごとの一番良かった★の数 (0 は未クリア)
    int stage = 0;
    int bestStars[MAZE_STAGE_COUNT] = {};

    // 禁止ワードで送れなかったときの注意 (次に送れたら消す)
    std::string warning;

    MazeBoard board = GenerateMazeBoard();

    // 話しかけた回数 (命令に訳せた指示の数) と、ゴールしたか
    int talks = 0;
    bool cleared = false;

    // ロボットのいるマスと、通ったマス
    MazeCell robot = MAZE_START;
    MazeDir facing = MazeDir::Right;
    std::vector<MazeCell> trail{MAZE_START};

    // 実行待ちの命令 (先頭の steps は、残りのマス数) と、いま動いている 1 マスの進み具合 (0〜1)
    std::deque<MazeMove> pending;

    // 実行中の命令が、最後の履歴の何番目の命令か (向きを書き足すのに使う)
    int commandIndex = 0;
    bool moving = false;
    MazeCell from = MAZE_START;
    float progress = 0.0f;

    // 訳した命令を実行中か (最後の 1 マスが終わるまで true)
    bool executing = false;
    int movedSteps = 0;

    // ぶつかったときの揺れ (残り秒)
    float bump = 0.0f;
};

// 指示を別スレッドで LLM に訳させる (結果は plan に入る)
// ステージとロボットの向きは、呼んだときの state のものを使う
void StartMazeCommand(MazeState& state, const std::string& text);

// ★の数 (1 回なら 3、2 回なら 2、それより多ければ 1)
int MazeStars(int talks);

// 訳した命令 (UI に出す文字列)
std::string DescribeMove(const MazeMove& move);

// 位置・通った跡・履歴・回数を最初に戻す (訳している途中なら中断する)
// newBoard なら新しい盤にする (false なら同じ盤でやり直す)
void ResetMaze(MazeState& state, bool newBoard);

// ステージを変えて、新しい盤で始める
void SelectMazeStage(MazeState& state, int stage);

// アプリを閉じるとき: 通信を止めて、スレッドの終了を待つ
void ShutdownMaze(MazeState& state);
