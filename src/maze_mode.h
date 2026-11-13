#pragma once

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// 迷路 (最初の版): 6×6 のマス目のロボットを、日本語の指示で上下左右に動かす
// LLM は指示を移動命令の JSON に訳すだけ (盤面は見せない)。動かすのはアプリ
// 動きは doc/maze.md を参照
// worker が書く項目は mutex で守り、アニメーションの項目は UI スレッドだけが触る

constexpr int MAZE_SIZE = 6;

// 1 つの命令で進める最大のマス数
constexpr int MAZE_MAX_STEPS = 5;

// 画面から見た向き (y は下に向かって増える)
enum class MazeDir
{
    Up,
    Down,
    Left,
    Right
};

struct MazeMove
{
    MazeDir dir = MazeDir::Up;
    int steps = 1;
};

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
};

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

    // ロボットのいるマスと、通ったマス
    MazeCell robot{0, MAZE_SIZE - 1};
    MazeDir facing = MazeDir::Right;
    std::vector<MazeCell> trail{{0, MAZE_SIZE - 1}};

    // 実行待ちの 1 マスずつの移動と、いま動いている 1 マスの進み具合 (0〜1)
    std::deque<MazeDir> pending;
    bool moving = false;
    MazeCell from{0, MAZE_SIZE - 1};
    float progress = 0.0f;

    // 訳した命令を実行中か (最後の 1 マスが終わるまで true)
    bool executing = false;
    int movedSteps = 0;

    // ぶつかったときの揺れ (残り秒)
    float bump = 0.0f;
};

// 指示を別スレッドで LLM に訳させる (結果は plan に入る)
void StartMazeCommand(MazeState& state, const std::string& text);

// 訳した命令 (UI に出す文字列)
std::string DescribeMove(const MazeMove& move);

// 位置・通った跡・履歴を最初に戻す (訳している途中なら中断する)
void ResetMaze(MazeState& state);

// アプリを閉じるとき: 通信を止めて、スレッドの終了を待つ
void ShutdownMaze(MazeState& state);
