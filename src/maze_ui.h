#pragma once

#include "maze_mode.h"
#include "settings_ui.h"

// 迷路タブの中身を描く (タブが開いているときに毎フレーム呼ぶ)
void DrawMazeTab(MazeState& state, const UiColors& colors);
