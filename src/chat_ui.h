#pragma once

#include "chat.h"

#include <string>

// チャット画面全体を描く (毎フレーム呼ぶ)
void DrawUI(ChatState& state, std::string& input, bool& focusInput);
