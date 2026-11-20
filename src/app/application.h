#pragma once

#include "app/renderer.h"
#include "common/rag.h"
#include "common/settings_ui.h"
#include "common/tab.h"

#include <memory>
#include <vector>

// アプリ本体: タブ (チャット・クイズ・迷路) と、タブが共有するもの (資料・設定) を持ち、
// メインループを回す。ウィンドウと ImGui の細かいことは Renderer に任せる
class Application
{
public:
    Application() = default;
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    // 初期化してメインループを回す。終わったら終了コード (0 = 正常)
    int Run();

private:
    bool Init();

    // イベントを処理する。ウィンドウが閉じられたら false
    bool ProcessEvents();

    void DrawFrame();

    // 画面全体: サーバーの状態・設定ボタン・タブ・設定画面
    void DrawMainWindow();

    // タブのワーカーと資料のインデックス作成を止めて待ち、サーバーを止める
    void Shutdown();

    Renderer mRenderer;

    // タブが参照するので、タブより先に作り、後に壊す (メンバーの並び順)
    RagState mRag;
    SettingsState mSettings;

    std::vector<std::unique_ptr<Tab>> mTabs;

    // 最初のフレームだけ、先頭のタブを選ぶ
    bool mFirstFrame = true;

    // 入力後しばらくは毎フレーム描画し、それ以外はイベント待ちで省電力
    int mFramesToRender = 3;

    bool mCurlInitialized = false;
    bool mShutDown = false;
};
