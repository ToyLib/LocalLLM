#pragma once

#include <SDL3/SDL.h>

#include <string>

// ウィンドウと描画の土台: SDL のウィンドウとレンダラー、ImGui、フォント、IME
// 画面の中身 (チャットやタブ) は知らない。Application が BeginFrame と EndFrame の間で描く
class Renderer
{
public:
    Renderer() = default;
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // SDL・ウィンドウ・レンダラー・ImGui を用意して、ウィンドウを表示する
    // width / height は 100% 表示のときの大きさ (画面の拡大率を掛けて作る)
    // fontFile / fontSize は config/ui.json の値 (LoadFont を参照)
    // 失敗したらログに書いて false (途中まで作ったものはデストラクタで片付ける)
    bool Init(
        const char* title,
        int width,
        int height,
        const std::string& fontFile,
        float fontSize);

    // ImGui と IME にイベントを渡す。ウィンドウを閉じる操作なら true
    bool ProcessEvent(const SDL_Event& event);

    bool IsMinimized() const;

    // 1 フレームの描画の始まりと終わり (間で ImGui の関数を呼ぶ)
    void BeginFrame();
    void EndFrame();

    // 画面上端に重なっているタイトルバーの高さ (macOS 以外は 0)
    float TopInset() const;

    // 終了処理に時間がかかるときに、先にウィンドウを隠す
    void Hide();

private:
    SDL_Window* mWindow = nullptr;
    SDL_Renderer* mRenderer = nullptr;

    // 後片付けで、作ったものだけを壊すための印
    bool mSdlInitialized = false;
    bool mImGuiCreated = false;
    bool mBackendInitialized = false;
};
