// 迷路タブの画面: マス目とロボット、指示の入力欄、指示と命令の履歴

#include "maze_ui.h"

#include "chat.h"
#include "ime.h"
#include "server_manager.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace
{

// 1 マス進むのにかける秒数
constexpr float STEP_SECONDS = 0.22f;

// ぶつかったときに揺れる秒数
constexpr float BUMP_SECONDS = 0.4f;

ImVec2 DirVector(MazeDir dir)
{
    switch (dir)
    {
    case MazeDir::Up:
        return ImVec2(0.0f, -1.0f);
    case MazeDir::Down:
        return ImVec2(0.0f, 1.0f);
    case MazeDir::Left:
        return ImVec2(-1.0f, 0.0f);
    case MazeDir::Right:
        return ImVec2(1.0f, 0.0f);
    }

    return ImVec2(1.0f, 0.0f);
}

ImU32 WithAlpha(const ImVec4& color, float alpha)
{
    return ImGui::GetColorU32(ImVec4(color.x, color.y, color.z, alpha));
}

// 話しかけた回数の評価 (1 回なら ★3、2 回なら ★2、それより多ければ ★1)
std::string StarsFor(int talks)
{
    const int stars = talks <= 1 ? 3 : talks == 2 ? 2 : 1;
    std::string text;

    for (int i = 0; i < 3; ++i)
    {
        text += i < stars ? "★" : "☆";
    }

    return text;
}

float EaseInOut(float t)
{
    return t * t * (3.0f - 2.0f * t);
}

// ワーカーの結果 (訳した命令・エラー) を受け取って、履歴に書き、実行を始める
void TakeResult(MazeState& state)
{
    bool hasPlan;
    std::vector<MazeMove> plan;
    std::string error;
    int elapsed;

    {
        std::lock_guard lock(state.mutex);
        hasPlan = state.hasPlan;
        plan = std::move(state.plan);
        error = std::move(state.error);
        elapsed = state.elapsedMs;
        state.hasPlan = false;
        state.plan.clear();
        state.error.clear();
    }

    if ((!hasPlan && error.empty()) || state.log.empty())
    {
        return;
    }

    MazeLogEntry& entry = state.log.back();
    entry.elapsedMs = elapsed;

    if (!error.empty())
    {
        entry.result = "エラー: " + error;
        entry.failed = true;
        return;
    }

    for (const MazeMove& move : plan)
    {
        entry.commands.push_back(DescribeMove(move));

        for (int i = 0; i < move.steps; ++i)
        {
            state.pending.push_back(move.dir);
        }
    }

    if (plan.empty())
    {
        entry.result = "動きの指示として読み取れませんでした (回数には数えません)";
        entry.failed = true;
        return;
    }

    ++state.talks;
    state.executing = true;
    state.movedSteps = 0;
}

// 1 マス分のアニメーションを進める (盤の外に出る移動なら、そこで止める)
void Animate(MazeState& state)
{
    const float dt = ImGui::GetIO().DeltaTime;
    state.bump = std::max(0.0f, state.bump - dt);

    if (state.moving)
    {
        state.progress += dt / STEP_SECONDS;

        if (state.progress >= 1.0f)
        {
            state.moving = false;
            state.progress = 0.0f;
            state.trail.push_back(state.robot);
            ++state.movedSteps;

            // ゴールに着いたら、残りの命令は捨てる
            if (state.robot == MAZE_GOAL)
            {
                state.pending.clear();
                state.executing = false;
                state.cleared = true;

                if (!state.log.empty())
                {
                    state.log.back().result = "ゴール！ (" + std::to_string(state.talks) + " 回目の指示で到着)";
                }
            }
        }
    }

    if (!state.moving && !state.pending.empty())
    {
        const MazeDir dir = state.pending.front();
        state.pending.pop_front();
        state.facing = dir;

        const MazeCell next = MazeNeighbor(state.robot, dir);

        if (MazeInside(next) && !state.board.Blocked(next))
        {
            state.from = state.robot;
            state.robot = next;
            state.moving = true;
        }
        else
        {
            state.pending.clear();
            state.bump = BUMP_SECONDS;
            state.executing = false;

            if (!state.log.empty())
            {
                const char* what = MazeInside(next) ? "障害物" : "盤の端";
                state.log.back().result =
                    std::to_string(state.movedSteps) + " マス進んで、" + what + "にぶつかりました";
                state.log.back().failed = true;
            }
        }
    }

    if (state.executing && !state.moving && state.pending.empty())
    {
        state.executing = false;

        if (!state.log.empty())
        {
            state.log.back().result = std::to_string(state.movedSteps) + " マス進みました";
        }
    }
}

void DrawBoard(const MazeState& state, float size, const UiColors& colors)
{
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##board", ImVec2(size, size));

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float cell = size / MAZE_SIZE;
    const float rounding = cell * 0.12f;

    draw->AddRectFilled(origin, ImVec2(origin.x + size, origin.y + size), WithAlpha(colors.text, 0.05f), rounding);

    // 通ったマス
    for (const MazeCell& c : state.trail)
    {
        const ImVec2 min(origin.x + c.x * cell + 2.0f, origin.y + c.y * cell + 2.0f);
        draw->AddRectFilled(min, ImVec2(min.x + cell - 4.0f, min.y + cell - 4.0f), WithAlpha(colors.ai, 0.18f), rounding);
    }

    // 障害物
    for (int y = 0; y < MAZE_SIZE; ++y)
    {
        for (int x = 0; x < MAZE_SIZE; ++x)
        {
            if (state.board.blocked[y][x])
            {
                const ImVec2 min(origin.x + x * cell + 3.0f, origin.y + y * cell + 3.0f);
                draw->AddRectFilled(
                    min, ImVec2(min.x + cell - 6.0f, min.y + cell - 6.0f), WithAlpha(colors.subText, 0.75f), rounding);
            }
        }
    }

    // ゴールのマス (旗の代わりに枠と G)
    {
        const ImVec2 min(origin.x + MAZE_GOAL.x * cell + 3.0f, origin.y + MAZE_GOAL.y * cell + 3.0f);
        const ImVec2 max(min.x + cell - 6.0f, min.y + cell - 6.0f);
        const float pulse = state.cleared ? 1.0f : 0.6f;
        draw->AddRectFilled(min, max, WithAlpha(colors.ai, 0.25f * pulse), rounding);
        draw->AddRect(min, max, WithAlpha(colors.ai, pulse), rounding, 0, 2.0f);

        const char* label = "G";
        const ImVec2 textSize = ImGui::CalcTextSize(label);
        draw->AddText(
            ImVec2(max.x - textSize.x - cell * 0.08f, max.y - textSize.y - cell * 0.04f),
            WithAlpha(colors.ai, 1.0f), label);
    }

    // スタートのマス
    {
        const ImVec2 min(origin.x + MAZE_START.x * cell, origin.y + MAZE_START.y * cell);
        const char* label = "S";
        const ImVec2 textSize = ImGui::CalcTextSize(label);
        draw->AddText(
            ImVec2(min.x + cell * 0.1f, min.y + cell * 0.9f - textSize.y), WithAlpha(colors.subText, 1.0f), label);
    }

    // マス目の線
    for (int i = 0; i <= MAZE_SIZE; ++i)
    {
        const float p = i * cell;
        const ImU32 line = WithAlpha(colors.subText, 0.5f);
        draw->AddLine(ImVec2(origin.x + p, origin.y), ImVec2(origin.x + p, origin.y + size), line);
        draw->AddLine(ImVec2(origin.x, origin.y + p), ImVec2(origin.x + size, origin.y + p), line);
    }

    // ロボット (動いている間は前のマスとの間を補う)
    float x = static_cast<float>(state.robot.x);
    float y = static_cast<float>(state.robot.y);

    if (state.moving)
    {
        const float t = EaseInOut(std::clamp(state.progress, 0.0f, 1.0f));
        x = state.from.x + (state.robot.x - state.from.x) * t;
        y = state.from.y + (state.robot.y - state.from.y) * t;
    }

    const ImVec2 dir = DirVector(state.facing);
    ImVec2 center(origin.x + (x + 0.5f) * cell, origin.y + (y + 0.5f) * cell);

    if (state.bump > 0.0f)
    {
        // ぶつかった向きに小刻みに揺らす
        const float shake = std::sin(state.bump * 60.0f) * cell * 0.08f * (state.bump / BUMP_SECONDS);
        center.x += dir.x * shake;
        center.y += dir.y * shake;
    }

    const float radius = cell * 0.32f;
    const ImVec4 body = state.bump > 0.0f ? colors.error : colors.user;
    draw->AddCircleFilled(center, radius, ImGui::GetColorU32(body), 32);

    // 向きを示す三角
    const ImVec2 side(-dir.y, dir.x);
    const ImVec2 tip(center.x + dir.x * radius * 0.85f, center.y + dir.y * radius * 0.85f);
    const ImVec2 baseCenter(center.x + dir.x * radius * 0.2f, center.y + dir.y * radius * 0.2f);
    const float half = radius * 0.4f;
    draw->AddTriangleFilled(
        tip,
        ImVec2(baseCenter.x + side.x * half, baseCenter.y + side.y * half),
        ImVec2(baseCenter.x - side.x * half, baseCenter.y - side.y * half),
        WithAlpha(colors.background, 1.0f));

    // ゴールしたら、盤の上に結果を重ねる
    if (state.cleared)
    {
        const std::string title = "ゴール！";
        const std::string stars = StarsFor(state.talks);
        const std::string detail = std::to_string(state.talks) + " 回の指示で到着";

        const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
        const float bandTop = origin.y + size * 0.5f - lineHeight * 1.8f;
        draw->AddRectFilled(
            ImVec2(origin.x, bandTop), ImVec2(origin.x + size, bandTop + lineHeight * 3.6f),
            WithAlpha(colors.background, 0.85f));

        const std::string lines[] = {title, stars, detail};
        const ImVec4 lineColors[] = {colors.ai, colors.text, colors.subText};

        for (int i = 0; i < 3; ++i)
        {
            const ImVec2 textSize = ImGui::CalcTextSize(lines[i].c_str());
            draw->AddText(
                ImVec2(origin.x + (size - textSize.x) * 0.5f, bandTop + lineHeight * (0.3f + i * 1.0f)),
                ImGui::GetColorU32(lineColors[i]), lines[i].c_str());
        }
    }
}

void DrawLog(const MazeState& state, bool busy, const UiColors& colors)
{
    if (state.log.empty())
    {
        ImGui::PushStyleColor(ImGuiCol_Text, colors.subText);
        ImGui::TextWrapped("指示を入力すると、LLM が命令 (JSON) に訳し、ロボットが動きます。");
        ImGui::TextWrapped("例: 「右に3つ進んで、上に2つ」「上にしばらく行ってから右へ」");
        ImGui::PopStyleColor();
        return;
    }

    for (size_t i = 0; i < state.log.size(); ++i)
    {
        const MazeLogEntry& entry = state.log[i];
        ImGui::PushID(static_cast<int>(i));

        ImGui::PushStyleColor(ImGuiCol_Text, colors.user);
        ImGui::TextWrapped("「%s」", entry.text.c_str());
        ImGui::PopStyleColor();

        ImGui::Indent();
        ImGui::PushStyleColor(ImGuiCol_Text, colors.subText);

        for (const std::string& command : entry.commands)
        {
            ImGui::TextUnformatted(command.c_str());
        }

        ImGui::PopStyleColor();

        const bool last = i + 1 == state.log.size();

        if (last && busy)
        {
            ImGui::TextColored(colors.subText, "命令に訳しています…");
        }
        else if (!entry.result.empty())
        {
            char seconds[32];
            std::snprintf(seconds, sizeof(seconds), " (%.1f 秒で訳しました)", entry.elapsedMs / 1000.0);

            ImGui::PushStyleColor(ImGuiCol_Text, entry.failed ? colors.error : colors.ai);
            ImGui::TextWrapped("%s", entry.result.c_str());
            ImGui::PopStyleColor();

            if (entry.elapsedMs > 0)
            {
                ImGui::SameLine(0.0f, 0.0f);
                ImGui::TextColored(colors.subText, "%s", seconds);
            }
        }

        ImGui::Unindent();
        ImGui::Spacing();
        ImGui::PopID();
    }
}

} // namespace

void DrawMazeTab(MazeState& state, const UiColors& colors)
{
    TakeResult(state);
    Animate(state);

    bool busy;
    {
        std::lock_guard lock(state.mutex);
        busy = state.busy;
    }

    // 動いている間は、イベントが無くても描き続ける
    if (state.moving || state.bump > 0.0f || !state.pending.empty())
    {
        WakeUI();
    }

    const ImGuiStyle& style = ImGui::GetStyle();

    // 遊び方と、いまの回数
    ImGui::TextColored(colors.subText, "S から G まで、障害物を避けて運ぼう。少ない指示でゴールするほど高評価");
    ImGui::Text("指示: %d 回", state.talks);
    ImGui::SameLine(0.0f, ImGui::GetFontSize() * 1.5f);
    ImGui::TextColored(colors.subText, "(最短の道順で曲がる回数: %d)", state.board.minTurns);

    const float bottomHeight = ImGui::GetFrameHeight() + style.ItemSpacing.y;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float boardSize = std::max(
        60.0f, std::min(avail.y - bottomHeight - style.ItemSpacing.y, avail.x * 0.55f));

    DrawBoard(state, boardSize, colors);

    ImGui::SameLine();

    // 履歴 (新しい指示が増えたら一番下へ)
    static size_t shownCount = 0;
    static bool shownBusy = false;

    ImGui::BeginChild("maze_log", ImVec2(0.0f, boardSize), ImGuiChildFlags_Borders);
    DrawLog(state, busy, colors);

    if (shownCount != state.log.size() || shownBusy != busy)
    {
        ImGui::SetScrollHereY(1.0f);
        shownCount = state.log.size();
        shownBusy = busy;
    }

    ImGui::EndChild();

    // 入力欄 (Enter で動かす)
    const bool running = busy || state.executing || state.cleared;
    const bool available = IsChatServerAvailable();
    const float sendWidth = ImGui::CalcTextSize("動かす").x + style.FramePadding.x * 2.0f;
    const float resetWidth = ImGui::CalcTextSize("やり直す").x + ImGui::CalcTextSize("新しい迷路").x +
        style.FramePadding.x * 4.0f + style.ItemSpacing.x;

    if (state.focusInput)
    {
        ImGui::SetKeyboardFocusHere();
        state.focusInput = false;
    }

    ImGui::SetNextItemWidth(-(sendWidth + resetWidth + style.ItemSpacing.x * 2.0f));
    const bool entered = ImGui::InputTextWithHint(
        "##maze_input", "ロボットへの指示 (例: 右に3つ、上に2つ)", &state.input,
        ImGuiInputTextFlags_EnterReturnsTrue);

    ImGui::SameLine();
    ImGui::BeginDisabled(running || !available);
    const bool clicked = ImGui::Button("動かす");
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("やり直す"))
    {
        ResetMaze(state, false);
    }

    ImGui::SameLine();
    if (ImGui::Button("新しい迷路"))
    {
        ResetMaze(state, true);
    }

    DrawComposition();

    if (entered)
    {
        state.focusInput = true;
    }

    const bool hasText = state.input.find_first_not_of(" \t\r\n") != std::string::npos;

    if ((clicked || entered) && !running && available && hasText)
    {
        MazeLogEntry entry;
        entry.text = state.input;
        state.log.push_back(std::move(entry));
        StartMazeCommand(state, state.input);
        state.input.clear();
        state.focusInput = true;
    }
}
