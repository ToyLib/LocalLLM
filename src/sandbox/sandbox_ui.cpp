#include "sandbox/sandbox_ui.h"

#include "common/agent.h"
#include "common/ime.h"
#include "common/server_manager.h"
#include "common/widgets.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <initializer_list>
#include <stdexcept>
#include <utility>

namespace
{
constexpr float SceneWidth = 800.0f;
constexpr float SceneHeight = 450.0f;
constexpr float Dusk = 0.55f;

ImU32 Color(ImU32 day, ImU32 night, float amount)
{
    return ImGui::ColorConvertFloat4ToU32(ImVec4(
        ((day & 255) * (1 - amount) + (night & 255) * amount) / 255,
        (((day >> 8) & 255) * (1 - amount) + ((night >> 8) & 255) * amount) / 255,
        (((day >> 16) & 255) * (1 - amount) + ((night >> 16) & 255) * amount) / 255,
        1.0f));
}

struct Scene
{
    ImDrawList& draw;
    ImVec2 origin;
    float scale;

    ImVec2 Point(float x, float y) const
    {
        return {origin.x + x * scale, origin.y + y * scale};
    }

    void Rect(float x, float y, float w, float h, ImU32 color) const
    {
        draw.AddRectFilled(Point(x, y), Point(x + w, y + h), color);
    }

    void Polygon(std::initializer_list<ImVec2> points, ImU32 color) const
    {
        for (const auto& p : points)
        {
            draw.PathLineTo(Point(p.x, p.y));
        }
        draw.PathFillConvex(color);
    }

    void Disc(float x, float y, float radius, ImU32 color) const
    {
        draw.AddCircleFilled(Point(x, y), radius * scale, color);
    }

    void Line(float x, float y, float endX, float endY, ImU32 color) const
    {
        draw.AddLine(Point(x, y), Point(endX, endY), color, std::max(1.0f, scale));
    }
};

void DrawHouse(const Scene& s, float x, float y, float w, float h, float night, float houseLight, int index)
{
    const float depth = 22.0f;
    s.Polygon({{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}},
        Color(IM_COL32(249, 213, 170, 255), IM_COL32(77, 86, 113, 255), night));
    s.Polygon({{x + w, y}, {x + w + depth, y - 13}, {x + w + depth, y + h - 13},
        {x + w, y + h}},
        Color(IM_COL32(200, 150, 126, 255), IM_COL32(45, 55, 78, 255), night));
    s.Polygon({{x, y}, {x + w / 2, y - 27}, {x + w, y}},
        Color(IM_COL32(198, 97, 89, 255), IM_COL32(85, 54, 76, 255), night));
    s.Polygon({{x + w / 2, y - 27}, {x + w / 2 + depth, y - 40},
        {x + w + depth, y - 13}, {x + w, y}},
        Color(IM_COL32(158, 63, 75, 255), IM_COL32(53, 43, 65, 255), night));
    const float light = std::clamp(houseLight * 2.0f - index * 0.13f - 0.32f, 0.0f, 1.0f);
    for (float offset : {0.23f, 0.62f})
    {
        const float wx = x + w * offset;
        s.Disc(wx + 5, y + 14, 15, IM_COL32(255, 177, 64, static_cast<int>(42 * light)));
        s.Rect(wx, y + 8, 10, 12,
            Color(IM_COL32(73, 109, 121, 255), IM_COL32(255, 215, 123, 255), light));
        s.Line(wx + 5, y + 8, wx + 5, y + 20, IM_COL32(95, 71, 75, 255));
    }
}

ImU32 SkyColor(ImU32 day, ImU32 dusk, ImU32 night, float darkness)
{
    return darkness <= Dusk ? Color(day, dusk, darkness / Dusk)
        : Color(dusk, night, (darkness - Dusk) / (1.0f - Dusk));
}

float Approach(float current, float target, float step)
{
    return std::abs(target - current) <= step ? target
        : current + std::copysign(step, target - current);
}

void DrawHarbor(const Scene& s, float night, float time, float snow, float fog,
    float beam, float houseLight, float boatPosition, float direction, bool boatMoving)
{
    const auto skyTop = SkyColor(IM_COL32(103, 184, 218, 255), IM_COL32(106, 91, 145, 255), IM_COL32(12, 19, 40, 255), night);
    const auto skyLow = SkyColor(IM_COL32(245, 224, 184, 255), IM_COL32(255, 158, 111, 255), IM_COL32(62, 79, 112, 255), night);
    const auto seaTop = SkyColor(IM_COL32(92, 169, 186, 255), IM_COL32(144, 112, 150, 255), IM_COL32(39, 67, 94, 255), night);
    const auto seaLow = SkyColor(IM_COL32(40, 123, 151, 255), IM_COL32(54, 70, 110, 255), IM_COL32(14, 31, 57, 255), night);
    s.draw.AddRectFilledMultiColor(s.Point(0, 0), s.Point(800, 191), skyTop, skyTop, skyLow, skyLow);
    s.draw.AddRectFilledMultiColor(s.Point(0, 191), s.Point(800, 450), seaTop, seaTop, seaLow, seaLow);

    s.Disc(650, 87, 37, IM_COL32(255, 237, 177, static_cast<int>(255 * (1 - night))));
    s.Disc(650, 87, 29, IM_COL32(211, 229, 250, static_cast<int>(230 * night)));
    const auto westHills = Color(IM_COL32(122, 161, 163, 255), IM_COL32(37, 49, 73, 255), night);
    s.Polygon({{0, 191}, {95, 123}, {246, 191}}, westHills);
    s.Polygon({{81, 191}, {218, 122}, {319, 191}}, westHills);
    const auto eastHills = Color(IM_COL32(145, 175, 170, 255), IM_COL32(43, 56, 80, 255), night);
    s.Polygon({{480, 191}, {567, 151}, {675, 191}}, eastHills);
    s.Polygon({{565, 191}, {703, 132}, {800, 191}}, eastHills);

    for (int i = 0; i < 48; ++i)
    {
        const float x = static_cast<float>((i * 173) % 747);
        const float y = 198.0f + static_cast<float>((i * 47) % 242);
        const float ripple = std::sin(time * 1.1f + i) * 9.0f * night;
        s.Line(x + ripple, y, x + ripple + 12 + i % 32, y,
            IM_COL32(204, 235, 250, 40 + (i % 4) * 10));
    }

    const auto mist = IM_COL32(193, 210, 222, static_cast<int>(180 * fog));
    const auto transparentMist = IM_COL32(193, 210, 222, 0);
    s.draw.AddRectFilledMultiColor(s.Point(0, 123), s.Point(800, 195),
        transparentMist, transparentMist, mist, mist);
    s.draw.AddRectFilledMultiColor(s.Point(0, 195), s.Point(800, 380),
        mist, mist, transparentMist, transparentMist);

    const auto quay = Color(IM_COL32(103, 104, 106, 255), IM_COL32(36, 45, 64, 255), night);
    s.Polygon({{48, 263}, {294, 356}, {294, 380}, {48, 287}}, quay);
    s.Polygon({{294, 356}, {470, 282}, {470, 306}, {294, 380}}, quay);
    s.Polygon({{48, 263}, {216, 189}, {470, 282}, {294, 356}},
        Color(IM_COL32(211, 185, 153, 255), IM_COL32(89, 105, 125, 255), night));
    for (int i = 0; i < 7; ++i)
    {
        s.Line(62 + 29 * i, 268 + 11 * i, 77 + 29 * i, 262 + 11 * i,
            IM_COL32(69, 77, 91, 120));
    }

    DrawHouse(s, 207, 194, 50, 49, night, houseLight, 0);
    DrawHouse(s, 268, 213, 58, 55, night, houseLight, 1);
    DrawHouse(s, 114, 230, 58, 49, night, houseLight, 2);
    DrawHouse(s, 178, 253, 64, 54, night, houseLight, 3);
    DrawHouse(s, 253, 278, 65, 54, night, houseLight, 4);

    const float sweep = std::sin(time * 0.85f);
    const float beamX = 684 + sweep * 110;
    const float beamY = 226 + sweep * 82;
    const float spread = 32 + 28 * fog;
    s.draw.AddTriangleFilled(s.Point(389, 173), s.Point(beamX, beamY - spread),
        s.Point(beamX, beamY + spread), IM_COL32(255, 220, 133, static_cast<int>((37 + 19 * fog) * beam)));
    s.draw.AddTriangleFilled(s.Point(389, 173), s.Point(beamX, beamY - 13),
        s.Point(beamX, beamY + 12), IM_COL32(255, 228, 155, static_cast<int>(37 * beam)));
    s.draw.AddEllipseFilled(s.Point(beamX, beamY + 12), ImVec2(46 * s.scale, 6 * s.scale),
        IM_COL32(255, 220, 133, static_cast<int>(27 * beam)));

    const auto tower = Color(IM_COL32(245, 225, 193, 255), IM_COL32(155, 168, 187, 255), night);
    s.Polygon({{377, 186}, {401, 186}, {409, 285}, {368, 285}}, tower);
    s.Polygon({{375, 211}, {403, 211}, {405, 231}, {373, 231}},
        Color(IM_COL32(190, 80, 81, 255), IM_COL32(117, 66, 85, 255), night));
    s.Rect(374, 161, 30, 26, IM_COL32(54, 64, 83, 255));
    s.Rect(379, 165, 20, 16,
        Color(IM_COL32(125, 164, 177, 255), IM_COL32(255, 231, 158, 255), beam));
    s.Disc(389, 173, 27, IM_COL32(255, 209, 100, static_cast<int>(55 * beam)));
    s.Polygon({{368, 161}, {388, 145}, {410, 161}}, IM_COL32(137, 64, 78, 255));

    const float boatX = 474 + 224 * boatPosition;
    const float boatY = 319 + 55 * boatPosition;
    if (boatMoving)
    {
        const float wake = std::min(1.0f, boatPosition * 12.0f);
        for (int i = 0; i < 3; ++i)
        {
            const float wakeX = boatX - direction * (24 + i * 13);
            s.Line(wakeX, boatY + 9 + i * 4, wakeX - direction * (14 + i * 7),
                boatY + 10 + i * 7, IM_COL32(213, 241, 245, static_cast<int>((137 - i * 28) * wake)));
        }
    }
    const float bow = direction * 6;
    const float haze = fog * boatPosition * 0.65f;
    constexpr ImU32 FogColor = IM_COL32(193, 210, 222, 255);
    s.Polygon({{boatX - 24 + bow, boatY}, {boatX + 24 + bow, boatY},
        {boatX + 17, boatY + 9}, {boatX - 17, boatY + 9}},
        Color(IM_COL32(179, 75, 80, 255), FogColor, haze));
    s.Rect(boatX - 12, boatY - 11, 24, 11, Color(tower, FogColor, haze));
    const auto cabinLight = Color(IM_COL32(75, 120, 137, 255), IM_COL32(255, 212, 112, 255), night);
    s.Rect(boatX - 6, boatY - 8, 11, 5, Color(cabinLight, FogColor, haze));

    for (int i = 0; i < 135; ++i)
    {
        const float x = 800.0f - std::fmod((i * 193) % 800 + time * (25 + i % 70), 800.0f);
        const float y = std::fmod((i * 101) % 450 + time * (20 + i % 37), 450.0f);
        const auto flake = IM_COL32(225, 240, 255, static_cast<int>((95 + i % 111) * snow));
        s.Line(x, y, x - 2.0f - i % 4, y + 2.0f, flake);
    }
}
} // namespace

SandboxTab::~SandboxTab()
{
    Shutdown();
}

void SandboxTab::Shutdown()
{
    mCancel = true;
    if (mRequest.valid())
    {
        mRequest.wait();
    }
}

SandboxTab::Command SandboxTab::Interpret(
    const std::string& instruction, const std::atomic<bool>& cancel)
{
    constexpr std::pair<const char*, Command> commands[] = {
        {"day", Command::Day}, {"blizzard_night", Command::BlizzardNight}, {"dusk", Command::Dusk},
        {"snow", Command::Snow}, {"clear_weather", Command::ClearWeather}, {"fog", Command::Fog},
        {"lighthouse_on", Command::LighthouseOn}, {"lighthouse_off", Command::LighthouseOff},
        {"house_lights_on", Command::HouseLightsOn}, {"house_lights_off", Command::HouseLightsOff},
        {"sail", Command::Sail}, {"dock", Command::Dock}, {"unsupported", Command::Unsupported}
    };
    json names = json::array();
    for (const auto& [name, command] : commands)
    {
        names.push_back(name);
    }

    Agent agent;
    agent.name = "箱庭";
    agent.systemPrompt = R"(港町への指示を、次の操作のうち1つに分類し、JSONのcommandに入れてください。
day: 晴れた昼の港全体に戻す。「昼の港に戻して」「明るい昼にして」
blizzard_night: 吹雪の夜の港全体に切り替える。「吹雪の夜にして」
dusk: 時刻だけ夕暮れにする。「夕暮れにして」
snow: 天気だけ雪にする。「雪を降らせて」「吹雪にして」
clear_weather: 天気だけ晴れにする。「雪だけ止めて」「霧を晴らして」
fog: 天気だけ霧にする。「霧を出して」
lighthouse_on: 灯台だけ点灯する。「灯台をつけて」
lighthouse_off: 灯台だけ消灯する。「灯台を消して」
house_lights_on: 家々の窓の明かりだけをつける。「家のあかりをつけて」「窓の明かりをつけて」
house_lights_off: 家々の窓の明かりだけを消す。「家のあかりを消して」「窓の明かりを消して」
sail: 船を走らせる。「船を出して」
dock: 船を港に戻して停泊させる。「船を港に戻して」「船を停泊させて」
unsupported: 一覧に無い操作、複数の異なる操作、または判断できない指示。「雨にして」「船をその場で止めて」
各操作に書かれた範囲を守り、指示に無い操作を選ばないでください。)";
    agent.options.maxTokens = 256;
    agent.options.temperature = 0.0f;
    agent.options.disableThinking = true;
    agent.options.schema = {
        {"type", "object"},
        {"properties", {{"command", {{"type", "string"}, {"enum", std::move(names)}}}}},
        {"required", json::array({"command"})},
        {"additionalProperties", false}
    };

    const json result = RunAgentJson(
        agent, json::array({{{"role", "user"}, {"content", instruction}}}), cancel);
    if (result.is_object() && result.size() == 1)
    {
        const auto value = result.find("command");
        if (value != result.end() && value->is_string())
        {
            for (const auto& [name, command] : commands)
            {
                if (*value == name)
                {
                    return command;
                }
            }
        }
    }
    throw std::runtime_error("風景の指定を読み取れませんでした");
}

void SandboxTab::ApplyCommand(Command command)
{
    switch (command)
    {
    case Command::Day:
        mTimeOfDay = 0.0f;
        mWeather = Weather::Clear;
        mHouseLightsOn.reset();
        mLighthouseOn = false;
        mSailing = false;
        break;
    case Command::BlizzardNight:
        mTimeOfDay = 1.0f;
        mWeather = Weather::Snow;
        mHouseLightsOn.reset();
        mLighthouseOn = true;
        mSailing = true;
        break;
    case Command::Dusk: mTimeOfDay = Dusk; break;
    case Command::Snow: mWeather = Weather::Snow; break;
    case Command::ClearWeather: mWeather = Weather::Clear; break;
    case Command::Fog: mWeather = Weather::Fog; break;
    case Command::LighthouseOn: mLighthouseOn = true; break;
    case Command::LighthouseOff: mLighthouseOn = false; break;
    case Command::HouseLightsOn: mHouseLightsOn = true; break;
    case Command::HouseLightsOff: mHouseLightsOn = false; break;
    case Command::Sail: mSailing = true; break;
    case Command::Dock: mSailing = false; break;
    case Command::Unsupported:
        mError = "例：吹雪の夜にして、雪だけ止めて、灯台を消して、家の明かりを消して、船を港に戻して、夕暮れにして、霧を出して";
        return;
    }
    mError.clear();
}

float SandboxTab::HouseLightTarget() const
{
    if (mHouseLightsOn.has_value())
    {
        return *mHouseLightsOn ? 1.0f : 0.0f;
    }
    return mTimeOfDay;
}

bool SandboxTab::IsAnimating() const
{
    return mDarkness != mTimeOfDay || mHouseLight != HouseLightTarget()
        || mWeather == Weather::Snow || mSnow > 0.0f
        || mFog != (mWeather == Weather::Fog ? 1.0f : 0.0f)
        || mLighthouseOn || mBeam > 0.0f || mSailing || mBoatPosition > 0.0f;
}

void SandboxTab::Animate(float elapsed)
{
    if (IsAnimating())
    {
        mAnimationTime += elapsed;
    }
    const float step = elapsed * 0.9f;
    mDarkness = Approach(mDarkness, mTimeOfDay, step);
    mHouseLight = Approach(mHouseLight, HouseLightTarget(), step);
    mSnow = Approach(mSnow, mWeather == Weather::Snow ? 1.0f : 0.0f, step);
    mFog = Approach(mFog, mWeather == Weather::Fog ? 1.0f : 0.0f, step);
    mBeam = Approach(mBeam, mLighthouseOn ? 1.0f : 0.0f, step);

    constexpr float BoatSpeed = 0.23f;
    if (mSailing)
    {
        if (mBoatPosition >= 1.0f)
        {
            mBoatDirection = -1.0f;
        }
        else if (mBoatPosition <= 0.0f)
        {
            mBoatDirection = 1.0f;
        }
        mBoatPosition = std::clamp(mBoatPosition + mBoatDirection * elapsed * BoatSpeed, 0.0f, 1.0f);
    }
    else if (mBoatPosition > 0.0f)
    {
        mBoatDirection = -1.0f;
        mBoatPosition = Approach(mBoatPosition, 0.0f, elapsed * BoatSpeed);
    }
}

void SandboxTab::TakeResult()
{
    if (!mRequest.valid() || mRequest.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
    {
        return;
    }

    try
    {
        const Command command = mRequest.get();
        ApplyCommand(command);
        if (command != Command::Unsupported)
        {
            mInput.clear();
        }
    }
    catch (const std::exception& error)
    {
        mError = std::string("風景を切り替えられませんでした。") + error.what();
    }
}

void SandboxTab::Draw(const UiContext& context)
{
    TakeResult();
    const bool busy = mRequest.valid();
    const bool serverAvailable = IsChatServerAvailable();

    if (ui::Button("昼の港", !busy))
    {
        ApplyCommand(Command::Day);
    }
    ImGui::SameLine();
    if (ui::Button("吹雪の夜", !busy))
    {
        ApplyCommand(Command::BlizzardNight);
    }

    ImGui::SetNextItemWidth(-(ui::ButtonWidth("指示する") + ImGui::GetStyle().ItemSpacing.x));
    const bool entered = ImGui::InputTextWithHint("##sandbox_input", "例：吹雪の夜にして",
        &mInput, ImGuiInputTextFlags_EnterReturnsTrue | (busy ? ImGuiInputTextFlags_ReadOnly : 0));
    ImGui::SameLine();
    const bool hasText = mInput.find_first_not_of(" \t\r\n") != std::string::npos;
    const bool canSend = !busy && serverAvailable && hasText;
    const bool clicked = ui::Button("指示する", canSend);
    DrawComposition();

    if ((clicked || entered) && canSend)
    {
        mError.clear();
        try
        {
            mRequest = std::async(std::launch::async, Interpret, mInput, std::cref(mCancel));
        }
        catch (const std::exception& error)
        {
            mError = std::string("指示を送信できませんでした。") + error.what();
        }
    }

    if (mRequest.valid())
    {
        ImGui::TextDisabled("ローカルLLMが指示を読み取っています…");
    }
    else if (!mError.empty())
    {
        ui::WrappedColored(context.colors.error, mError);
    }
    else if (!serverAvailable)
    {
        ImGui::TextDisabled("ローカルLLMに接続できません。ボタンで切り替えられます。");
    }
    else
    {
        ImGui::Dummy(ImVec2(0.0f, ImGui::GetTextLineHeight()));
    }

    Animate(std::min(ImGui::GetIO().DeltaTime, 0.05f));
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float scale = std::min(available.x / SceneWidth, available.y / SceneHeight);
    if (scale <= 0.0f)
    {
        return;
    }

    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const ImVec2 origin(cursor.x + (available.x - SceneWidth * scale) * 0.5f,
        cursor.y + (available.y - SceneHeight * scale) * 0.5f);
    ImGui::Dummy(available);
    const Scene scene{*ImGui::GetWindowDrawList(), origin, scale};
    scene.draw.PushClipRect(origin, scene.Point(SceneWidth, SceneHeight), true);
    DrawHarbor(scene, mDarkness, mAnimationTime, mSnow, mFog, mBeam, mHouseLight,
        mBoatPosition, mBoatDirection, mSailing || mBoatPosition > 0.0f);
    scene.draw.PopClipRect();
}
