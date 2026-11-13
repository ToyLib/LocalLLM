#pragma once

#include <SDL3/SDL.h>

// macOS 専用のウィンドウの見た目の設定

// 透明ウィンドウでタイトルバーだけが透けないよう、
// 本文をタイトルバーの下まで広げる (SDL_CreateRenderer の後に呼ぶ)
void ApplyMacTitleBarStyle(SDL_Window* window);

// 本文に重なっているタイトルバーの高さ (フルスクリーン中は 0)
float GetMacTitleBarHeight(SDL_Window* window);
