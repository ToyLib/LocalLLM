#pragma once

#include <imgui.h>

// 日本語フォントを読み込む
// LOCALLLM_FONT → 同梱フォント → OSのフォント の順に探し、
// 足りない文字はOSのフォントで補う
void LoadFont(ImGuiIO& io);
