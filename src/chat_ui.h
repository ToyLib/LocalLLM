#pragma once

#include "chat.h"
#include "rag.h"

#include <string>

// チャット画面全体を描く (毎フレーム呼ぶ)
void DrawUI(
    ChatState& state, RagState& rag, std::string& input, bool& focusInput);
