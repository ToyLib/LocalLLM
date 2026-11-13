#pragma once

#include "chat/chat.h"
#include "maze/maze_mode.h"
#include "quiz/quiz_mode.h"
#include "common/rag.h"
#include "common/settings_ui.h"

#include <string>

// チャット画面全体を描く (毎フレーム呼ぶ)
// topInset: 画面上端に重なっているタイトルバーの高さ (macOS)
void DrawUI(
    ChatState& state,
    RagState& rag,
    QuizState& quiz,
    MazeState& maze,
    SettingsState& settings,
    std::string& input,
    bool& focusInput,
    float topInset);
