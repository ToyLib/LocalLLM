#pragma once

#include "code_mode.h"
#include "settings_ui.h"

// コーディング練習タブの中身を描く (タブが開いているときに毎フレーム呼ぶ)
void DrawCodeTab(CodeState& state, const UiColors& colors);
