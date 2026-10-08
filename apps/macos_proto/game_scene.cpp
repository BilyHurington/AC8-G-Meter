#include "game_scene.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>

#include <imgui.h>

namespace ft {
namespace {

// Same green as the vanilla HUD and the telemetry overlay default (#93F6A8).
constexpr ImU32 kHudGreen = IM_COL32(147, 246, 168, 255);
constexpr ImU32 kHudGreenDim = IM_COL32(147, 246, 168, 150);

ImVec2 add(ImVec2 a, ImVec2 b) { return ImVec2(a.x + b.x, a.y + b.y); }
ImVec2 mul(ImVec2 a, float s) { return ImVec2(a.x * s, a.y * s); }

void shadowText(ImDrawList* dl, ImFont* font, float size, ImVec2 p, ImU32 col, const char* text) {
  dl->AddText(font, size, ImVec2(p.x + 1.0f, p.y + 1.0f), IM_COL32(0, 0, 0, 140), text);
  dl->AddText(font, size, p, col, text);
}

}  // namespace

void attitudeAngles(const Basis& a, double& pitchDeg, double& rollDeg) {
  pitchDeg = std::asin(std::clamp(a.forward.z, -1.0, 1.0)) * kRadToDeg;
  rollDeg = std::atan2(-a.right.z, a.up.z) * kRadToDeg;
}

void GameScene::registerClick(float x, float y, double now) {
  clickX_ = x;
  clickY_ = y;
  clickAt_ = now;
}

void GameScene::draw(ImDrawList* dl, float w, float h, const SceneState& s, double now) {
  drawWorld(dl, w, h, s);
  drawGameHud(dl, w, h, s);
  drawClickMarker(dl, h, now);
}

void GameScene::drawWorld(ImDrawList* dl, float w, float h, const SceneState& s) {
  const float pxPerDeg = h / 55.0f;  // ~55 deg vertical field of view
  const ImVec2 center(w * 0.5f, h * 0.5f);
  const float roll = static_cast<float>(s.rollDeg * kDegToRad);
  // Right wing down -> the world appears to rotate counter-clockwise (right end of the
  // horizon goes up on screen, y grows downwards).
  const ImVec2 along(std::cos(roll), -std::sin(roll));
  const ImVec2 down(std::sin(roll), std::cos(roll));  // towards the ground side
  const float pitchPx = static_cast<float>(s.pitchDeg) * pxPerDeg;
  const ImVec2 horizon = add(center, mul(down, pitchPx));
  const float big = (w + h) * 4.0f;

  // Sky: deep blue at the top to pale haze near the horizon (screen-space gradient).
  dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(w, h), IM_COL32(28, 58, 104, 255),
                              IM_COL32(28, 58, 104, 255), IM_COL32(150, 178, 204, 255),
                              IM_COL32(150, 178, 204, 255));
  // Haze band on the sky side, following the horizon.
  const float hazePx = h * 0.18f;
  const ImVec2 hl = add(horizon, mul(along, -big)), hr = add(horizon, mul(along, big));
  const ImVec2 hlUp = add(hl, mul(down, -hazePx)), hrUp = add(hr, mul(down, -hazePx));
  dl->AddQuadFilled(hlUp, hrUp, hr, hl, IM_COL32(190, 205, 215, 70));

  // Ground: dark olive/blue-grey sea-and-land tone with a few distance bands.
  const ImVec2 gl = add(hl, mul(down, big)), gr = add(hr, mul(down, big));
  dl->AddQuadFilled(hl, hr, gr, gl, IM_COL32(52, 66, 58, 255));
  const float bands[] = {0.6f, 1.5f, 3.0f, 6.0f, 11.0f, 20.0f};  // deg below the horizon
  for (float deg : bands) {
    const ImVec2 o = mul(down, deg * pxPerDeg);
    dl->AddLine(add(hl, o), add(hr, o), IM_COL32(70, 86, 74, 110), 1.0f);
  }
  dl->AddQuadFilled(hl, hr, add(hr, mul(down, h * 0.05f)), add(hl, mul(down, h * 0.05f)),
                    IM_COL32(120, 135, 130, 60));
  dl->AddLine(hl, hr, IM_COL32(220, 230, 235, 150), 1.5f);

  // Sparse pitch ladder (every 10 deg) around the boresight, faint.
  for (int deg = -30; deg <= 30; deg += 10) {
    if (deg == 0) continue;
    const ImVec2 c = add(horizon, mul(down, -static_cast<float>(deg) * pxPerDeg));
    const float half = w * 0.05f, gap = w * 0.018f;
    const ImU32 col = IM_COL32(147, 246, 168, 70);
    dl->AddLine(add(c, mul(along, -half)), add(c, mul(along, -gap)), col, 1.0f);
    dl->AddLine(add(c, mul(along, gap)), add(c, mul(along, half)), col, 1.0f);
  }

  // Boresight "W" (fixed) and flight-path marker (offset by AoA / sideslip).
  const float u = h / 108.0f;  // 10 px at 1080p
  const ImVec2 b = center;
  const ImVec2 wPts[] = {ImVec2(b.x - 3 * u, b.y), ImVec2(b.x - 1.5f * u, b.y), ImVec2(b.x - 0.75f * u, b.y + u),
                         ImVec2(b.x, b.y), ImVec2(b.x + 0.75f * u, b.y + u), ImVec2(b.x + 1.5f * u, b.y),
                         ImVec2(b.x + 3 * u, b.y)};
  dl->AddPolyline(wPts, 7, kHudGreen, 1.6f, ImDrawFlags_None);
  const ImVec2 fpm(b.x - static_cast<float>(s.sideslipDeg) * pxPerDeg,
                   b.y + static_cast<float>(s.aoaDeg) * pxPerDeg);
  dl->AddCircle(fpm, 0.8f * u, kHudGreen, 20, 1.6f);
  dl->AddLine(ImVec2(fpm.x - 2.2f * u, fpm.y), ImVec2(fpm.x - 0.8f * u, fpm.y), kHudGreen, 1.6f);
  dl->AddLine(ImVec2(fpm.x + 0.8f * u, fpm.y), ImVec2(fpm.x + 2.2f * u, fpm.y), kHudGreen, 1.6f);
  dl->AddLine(ImVec2(fpm.x, fpm.y - 0.8f * u), ImVec2(fpm.x, fpm.y - 1.8f * u), kHudGreen, 1.6f);
}

void GameScene::drawGameHud(ImDrawList* dl, float w, float h, const SceneState& s) {
  ImFont* font = font_ ? font_ : ImGui::GetFont();
  const float scale = h / 1080.0f;
  const float size = 20.0f * scale;
  const float x = std::round(w * 0.045f);
  char buf[64];

  // Vanilla-style top-left block: TIME / SCORE / TARGET at ~6 %, 9.5 %, 13 % of the height.
  // The telemetry overlay defaults to 16 %, i.e. directly underneath.
  const double remaining = std::max(0.0, 600.0 - s.missionTimeS);
  const int mm = static_cast<int>(remaining / 60.0);
  const double ss = remaining - mm * 60.0;
  std::snprintf(buf, sizeof buf, "TIME   %02d:%05.2f", mm, ss);
  shadowText(dl, font, size, ImVec2(x, std::round(h * 0.06f)), kHudGreen, buf);
  std::snprintf(buf, sizeof buf, "SCORE  %d", 12500 + 300 * static_cast<int>(s.missionTimeS / 9.0));
  shadowText(dl, font, size, ImVec2(x, std::round(h * 0.095f)), kHudGreen, buf);
  std::snprintf(buf, sizeof buf, "TARGET %d", std::max(0, 14 - static_cast<int>(s.missionTimeS / 9.0)));
  shadowText(dl, font, size, ImVec2(x, std::round(h * 0.13f)), kHudGreen, buf);

  // Speed and altitude boxes either side of the boresight, like most flight-game HUDs.
  const float boxW = 96.0f * scale, boxH = 30.0f * scale;
  const ImVec2 spd(w * 0.5f - w * 0.17f - boxW, h * 0.5f - boxH * 0.5f);
  const ImVec2 alt(w * 0.5f + w * 0.17f, h * 0.5f - boxH * 0.5f);
  const float small = 18.0f * scale;
  dl->AddRect(spd, ImVec2(spd.x + boxW, spd.y + boxH), kHudGreen, 0.0f, 1.2f);
  dl->AddRect(alt, ImVec2(alt.x + boxW, alt.y + boxH), kHudGreen, 0.0f, 1.2f);
  std::snprintf(buf, sizeof buf, "%4.0f", s.speedKmh);
  shadowText(dl, font, size, ImVec2(spd.x + 12.0f * scale, spd.y + (boxH - size) * 0.5f), kHudGreen, buf);
  std::snprintf(buf, sizeof buf, "%5.0f", s.altitudeM);
  shadowText(dl, font, size, ImVec2(alt.x + 8.0f * scale, alt.y + (boxH - size) * 0.5f), kHudGreen, buf);
  shadowText(dl, font, small, ImVec2(spd.x, spd.y + boxH + 4.0f * scale), kHudGreenDim, "SPEED");
  shadowText(dl, font, small, ImVec2(alt.x, alt.y + boxH + 4.0f * scale), kHudGreenDim, "ALT");

  // Game-flow banners.
  const char* banner = nullptr;
  if (s.phase == MissionPhase::Ended) banner = "MISSION ACCOMPLISHED";
  else if (s.phase == MissionPhase::Paused) banner = "PAUSE";
  else if (s.phase == MissionPhase::None) banner = "MAIN MENU";
  if (banner) {
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), IM_COL32(0, 0, 0, 70));
    const float bs = 40.0f * scale;
    const float tw = font->CalcTextSizeA(bs, FLT_MAX, 0.0f, banner).x;
    shadowText(dl, font, bs, ImVec2(std::round((w - tw) * 0.5f), std::round(h * 0.08f)), kHudGreen, banner);
  }

  // Watermark so screenshots are never mistaken for the real game.
  const char* mark = "FAKE GAME SCENE (prototype background)";
  const float ms = 13.0f * std::max(scale, 0.8f);
  const float mw = font->CalcTextSizeA(ms, FLT_MAX, 0.0f, mark).x;
  dl->AddText(font, ms, ImVec2(w - mw - 12.0f, h - ms - 10.0f), IM_COL32(255, 255, 255, 90), mark);
}

void GameScene::drawClickMarker(ImDrawList* dl, float h, double now) {
  const double age = now - clickAt_;
  constexpr double kDuration = 0.8;
  if (age < 0.0 || age > kDuration) return;
  const float t = static_cast<float>(age / kDuration);
  const int alpha = static_cast<int>(255.0f * (1.0f - t));
  const float scale = h / 1080.0f;
  const ImVec2 c(clickX_, clickY_);
  dl->AddCircle(c, (8.0f + 30.0f * t) * scale, IM_COL32(255, 210, 90, alpha), 32, 2.0f);
  dl->AddCircleFilled(c, 3.0f * scale, IM_COL32(255, 210, 90, alpha));
  dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(c.x + 14.0f * scale, c.y - 22.0f * scale),
              IM_COL32(255, 210, 90, alpha), "game received click");
}

}  // namespace ft
