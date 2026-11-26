#include "sandbox/sandbox_ui.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace
{
constexpr float SceneWidth = 800.0f;
constexpr float SceneHeight = 450.0f;

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

void DrawHouse(const Scene& s, float x, float y, float w, float h, float night, int index)
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
    const float light = std::clamp(night * 2.0f - index * 0.13f - 0.32f, 0.0f, 1.0f);
    for (float offset : {0.23f, 0.62f})
    {
        const float wx = x + w * offset;
        s.Disc(wx + 5, y + 14, 15, IM_COL32(255, 177, 64, static_cast<int>(42 * light)));
        s.Rect(wx, y + 8, 10, 12,
            Color(IM_COL32(73, 109, 121, 255), IM_COL32(255, 215, 123, 255), light));
        s.Line(wx + 5, y + 8, wx + 5, y + 20, IM_COL32(95, 71, 75, 255));
    }
}

void DrawHarbor(const Scene& s, float night, float time)
{
    const auto skyTop = Color(IM_COL32(103, 184, 218, 255), IM_COL32(12, 19, 40, 255), night);
    const auto skyLow = Color(IM_COL32(245, 224, 184, 255), IM_COL32(62, 79, 112, 255), night);
    const auto seaTop = Color(IM_COL32(92, 169, 186, 255), IM_COL32(39, 67, 94, 255), night);
    const auto seaLow = Color(IM_COL32(40, 123, 151, 255), IM_COL32(14, 31, 57, 255), night);
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

    DrawHouse(s, 207, 194, 50, 49, night, 0);
    DrawHouse(s, 268, 213, 58, 55, night, 1);
    DrawHouse(s, 114, 230, 58, 49, night, 2);
    DrawHouse(s, 178, 253, 64, 54, night, 3);
    DrawHouse(s, 253, 278, 65, 54, night, 4);

    const float sweep = std::sin(time * 0.85f);
    const float beamX = 684 + sweep * 110;
    const float beamY = 226 + sweep * 82;
    s.draw.AddTriangleFilled(s.Point(389, 173), s.Point(beamX, beamY - 33),
        s.Point(beamX, beamY + 32), IM_COL32(255, 220, 133, static_cast<int>(37 * night)));
    s.draw.AddTriangleFilled(s.Point(389, 173), s.Point(beamX, beamY - 13),
        s.Point(beamX, beamY + 12), IM_COL32(255, 228, 155, static_cast<int>(37 * night)));
    s.draw.AddEllipseFilled(s.Point(beamX, beamY + 12), ImVec2(46 * s.scale, 6 * s.scale),
        IM_COL32(255, 220, 133, static_cast<int>(27 * night)));

    const auto tower = Color(IM_COL32(245, 225, 193, 255), IM_COL32(155, 168, 187, 255), night);
    s.Polygon({{377, 186}, {401, 186}, {409, 285}, {368, 285}}, tower);
    s.Polygon({{375, 211}, {403, 211}, {405, 231}, {373, 231}},
        Color(IM_COL32(190, 80, 81, 255), IM_COL32(117, 66, 85, 255), night));
    s.Rect(374, 161, 30, 26, IM_COL32(54, 64, 83, 255));
    s.Rect(379, 165, 20, 16,
        Color(IM_COL32(125, 164, 177, 255), IM_COL32(255, 231, 158, 255), night));
    s.Disc(389, 173, 27, IM_COL32(255, 209, 100, static_cast<int>(55 * night)));
    s.Polygon({{368, 161}, {388, 145}, {410, 161}}, IM_COL32(137, 64, 78, 255));

    const float route = std::sin(time * 0.29f);
    const float boatX = 507 + 112 * route * night;
    const float boatY = 330 + 34 * route * night;
    const float direction = std::cos(time * 0.29f) >= 0 ? 1.0f : -1.0f;
    for (int i = 0; i < 3; ++i)
    {
        const float wakeX = boatX - direction * (24 + i * 13);
        s.Line(wakeX, boatY + 9 + i * 4, wakeX - direction * (14 + i * 7),
            boatY + 10 + i * 7, IM_COL32(213, 241, 245, static_cast<int>((137 - i * 28) * night)));
    }
    s.Polygon({{boatX - 27, boatY}, {boatX + 27, boatY}, {boatX + 18, boatY + 9},
        {boatX - 17, boatY + 9}}, IM_COL32(179, 75, 80, 255));
    s.Rect(boatX - 12, boatY - 11, 24, 11, tower);
    s.Rect(boatX - 6, boatY - 8, 11, 5,
        Color(IM_COL32(75, 120, 137, 255), IM_COL32(255, 212, 112, 255), night));

    for (int i = 0; i < 135; ++i)
    {
        const float x = 800.0f - std::fmod((i * 193) % 800 + time * (25 + i % 70), 800.0f);
        const float y = std::fmod((i * 101) % 450 + time * (20 + i % 37), 450.0f);
        const auto snow = IM_COL32(225, 240, 255, static_cast<int>((95 + i % 111) * night));
        s.Line(x, y, x - 2.0f - i % 4, y + 2.0f, snow);
    }
}
} // namespace

void SandboxTab::Draw(const UiContext&)
{
    if (ImGui::Button("昼の港"))
    {
        mNight = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("吹雪の夜"))
    {
        mNight = true;
    }

    const float elapsed = std::min(ImGui::GetIO().DeltaTime, 0.05f);
    const float step = elapsed * 0.9f;
    mAnimationTime += elapsed * mBlend;
    mBlend = std::clamp(mBlend + (mNight ? step : -step), 0.0f, 1.0f);
    const float night = mBlend * mBlend * (3 - 2 * mBlend);
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
    DrawHarbor(scene, night, mAnimationTime);
    scene.draw.PopClipRect();
}
