#pragma once

#include "quiz_mode.h"
#include "rag.h"
#include "settings_ui.h"

// クイズタブの中身を描く (タブが開いているときに毎フレーム呼ぶ)
void DrawQuizTab(QuizState& state, RagState& rag, const UiColors& colors);
