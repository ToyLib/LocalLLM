#pragma once

#include "common/tab.h"

#include <atomic>
#include <future>
#include <optional>
#include <string>

class SandboxTab final : public Tab
{
public:
    ~SandboxTab() override;

    const char* Label() const override { return "箱庭"; }
    void Draw(const UiContext&) override;
    void Shutdown() override;
    bool IsAnimating() const override;

private:
    enum class Command
    {
        Day, BlizzardNight, Dusk, Snow, ClearWeather, Fog,
        LighthouseOn, LighthouseOff, HouseLightsOn, HouseLightsOff, Sail, Dock, Unsupported
    };
    enum class Weather { Clear, Snow, Fog };

    static Command Interpret(const std::string& instruction, const std::atomic<bool>& cancel);
    void TakeResult();
    void ApplyCommand(Command command);
    void Animate(float elapsed);
    float HouseLightTarget() const;

    std::string mInput;
    std::string mError;
    std::atomic<bool> mCancel{false};
    std::future<Command> mRequest;

    float mTimeOfDay = 0.0f;
    Weather mWeather = Weather::Clear;
    bool mLighthouseOn = false;
    std::optional<bool> mHouseLightsOn;
    bool mSailing = false;

    float mDarkness = 0.0f;
    float mHouseLight = 0.0f;
    float mSnow = 0.0f;
    float mFog = 0.0f;
    float mBeam = 0.0f;
    float mBoatPosition = 0.0f;
    float mBoatDirection = 1.0f;
    float mAnimationTime = 0.0f;
};
