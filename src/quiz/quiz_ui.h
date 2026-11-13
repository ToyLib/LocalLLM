#pragma once

#include "quiz/quiz_mode.h"
#include "common/rag.h"
#include "common/settings_ui.h"

// クイズタブの中身を描く (タブが開いているときに毎フレーム呼ぶ)
void DrawQuizTab(QuizState& state, RagState& rag, const UiColors& colors);
