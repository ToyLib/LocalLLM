#pragma once

#include "chat.h"
#include "rag.h"
#include "settings_ui.h"

#include <string>

// チャット画面全体を描く (毎フレーム呼ぶ)
// topInset: 画面上端に重なっているタイトルバーの高さ (macOS)
void DrawUI(
    ChatState& state,
    RagState& rag,
    SettingsState& settings,
    std::string& input,
    bool& focusInput,
    float topInset);
