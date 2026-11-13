// macOS 専用のウィンドウの見た目の設定

#include "common/macos_window.h"

#import <AppKit/AppKit.h>

namespace
{

NSWindow* GetNSWindow(SDL_Window* window)
{
    return (__bridge NSWindow*)SDL_GetPointerProperty(
        SDL_GetWindowProperties(window),
        SDL_PROP_WINDOW_COCOA_WINDOW_POINTER,
        nullptr);
}

} // namespace

void ApplyMacTitleBarStyle(SDL_Window* window)
{
    NSWindow* nsWindow = GetNSWindow(window);

    if (!nsWindow)
    {
        return;
    }

    // 本文をタイトルバーの下まで広げ、タイトルバーも
    // ImGui の半透明の背景で塗って本文と一体にする
    nsWindow.styleMask |= NSWindowStyleMaskFullSizeContentView;
    nsWindow.titlebarAppearsTransparent = YES;
}

float GetMacTitleBarHeight(SDL_Window* window)
{
    NSWindow* nsWindow = GetNSWindow(window);

    if (!nsWindow)
    {
        return 0.0f;
    }

    // contentLayoutRect はタイトルバーに隠れない部分
    const CGFloat height =
        NSHeight(nsWindow.contentView.bounds) -
        NSHeight(nsWindow.contentLayoutRect);

    return static_cast<float>(height);
}
