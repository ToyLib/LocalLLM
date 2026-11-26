#pragma once

#include "common/tab.h"

class SandboxTab final : public Tab
{
public:
    const char* Label() const override { return "箱庭"; }
    void Draw(const UiContext&) override;
    bool IsAnimating() const override { return mNight || mBlend > 0.0f; }

private:
    bool mNight = false;
    float mBlend = 0.0f;
    float mAnimationTime = 0.0f;
};
