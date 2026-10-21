#pragma once

#include <SDL3/SDL.h>

// IMEで変換中 (未確定) の文字列の表示
// ImGuiは描画しないため、入力カーソルの位置に重ねて自前で描画する

// 変換中の文字列はアプリで描画することをSDLに伝える (SDL_Init の前に呼ぶ)
void SetImeHint();

// 入力カーソルの位置を受け取るため、ImGuiのバックエンドの処理を包む
// (ImGui_ImplSDL3_Init の後に呼ぶ)
void InstallImeHook();

void HandleCompositionEvent(const SDL_Event& event);

// 入力欄を描いた後に呼ぶ
void DrawComposition();
