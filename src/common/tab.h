#pragma once

#include "common/settings_ui.h"

// タブを描くときに、どのタブにも渡すもの
struct UiContext
{
    // 画面の色 (設定画面を開いている間は、編集中の色)
    const UiColors& colors;
};

// メインウィンドウのタブ (チャット・クイズ・迷路・箱庭) の共通の形
// Application が持ち、選ばれているタブの Draw を毎フレーム呼ぶ
// 閉じているタブの仕事も、ワーカースレッドで動き続ける
class Tab
{
public:
    virtual ~Tab() = default;

    // タブに出す名前
    virtual const char* Label() const = 0;

    // タブの中身を描く (タブが選ばれているときだけ、毎フレーム呼ぶ)
    virtual void Draw(const UiContext& context) = 0;

    // 表示中にアニメーション用のフレームを続けて描く
    virtual bool IsAnimating() const { return false; }

    // 設定画面で保存したあとに呼ぶ (設定を読み直す)
    virtual void OnSettingsSaved() {}

    // 終了するときに呼ぶ (ワーカースレッドを止めて、終わるのを待つ)
    virtual void Shutdown() {}
};
