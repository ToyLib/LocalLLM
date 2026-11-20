#pragma once

#include "maze/maze_mode.h"
#include "common/tab.h"

// 迷路のタブ: 迷路の状態を持つ
class MazeTab : public Tab
{
public:
    const char* Label() const override { return "迷路"; }
    void Draw(const UiContext& context) override;
    void Shutdown() override;

private:
    MazeState mState;
};
