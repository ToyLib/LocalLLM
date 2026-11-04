#pragma once

#include <imgui.h>

#include <string>

// 日本語フォントを読み込む
// LOCALLLM_FONT → fontFile (config/ui.json の font_file) → OSのフォント の順に探し、
// 足りない文字はOSのフォントで補う
// fontFile が相対パスなら実行ファイルのフォルダーから探す (パスはUTF-8)
void LoadFont(ImGuiIO& io, const std::string& fontFile, float fontSize);
