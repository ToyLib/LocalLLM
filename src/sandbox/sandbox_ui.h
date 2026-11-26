#pragma once

#include "common/tab.h"

#include <atomic>
#include <future>
#include <string>

class SandboxTab final : public Tab
{
public:
    ~SandboxTab() override;

    const char* Label() const override { return "箱庭"; }
    void Draw(const UiContext&) override;
    void Shutdown() override;
    bool IsAnimating() const override { return mNight || mBlend > 0.0f; }

private:
    enum class Preset { Day, BlizzardNight, Unsupported };

    static Preset Interpret(const std::string& instruction, const std::atomic<bool>& cancel);
    void TakeResult();
    void ApplyPreset(Preset preset);

    std::string mInput;
    std::string mError;
    std::atomic<bool> mCancel{false};
    std::future<Preset> mRequest;
    bool mNight = false;
    float mBlend = 0.0f;
    float mAnimationTime = 0.0f;
};
