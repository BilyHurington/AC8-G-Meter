#include "ui/hud_overlay.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

#include <imgui.h>

#include "ui/format.h"
#include "ui/layout.h"

namespace ft {
namespace {

ImU32 toU32(const Color& c, float alpha) {
  return ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, std::clamp(alpha, 0.0f, 1.0f)));
}

void drawDashedLine(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float dash, float thickness) {
  const float dx = b.x - a.x, dy = b.y - a.y;
  const float len = std::sqrt(dx * dx + dy * dy);
  if (len <= 0.0f) return;
  const float ux = dx / len, uy = dy / len;
  for (float s = 0.0f; s < len; s += 2.0f * dash) {
    const float e = std::min(s + dash, len);
    dl->AddLine(ImVec2(a.x + ux * s, a.y + uy * s), ImVec2(a.x + ux * e, a.y + uy * e), col, thickness);
  }
}

void drawDashedRect(ImDrawList* dl, ImVec2 mn, ImVec2 mx, ImU32 col, float dash, float thickness) {
  drawDashedLine(dl, mn, ImVec2(mx.x, mn.y), col, dash, thickness);
  drawDashedLine(dl, ImVec2(mx.x, mn.y), mx, col, dash, thickness);
  drawDashedLine(dl, mx, ImVec2(mn.x, mx.y), col, dash, thickness);
  drawDashedLine(dl, ImVec2(mn.x, mx.y), mn, col, dash, thickness);
}

}  // namespace

void HudOverlayLayer::refreshLines(const OverlayFrame& f) {
  const auto& visible = f.settings.overlay.visible;
  // Throttled so numbers stay readable; a visibility change or a clock reset refreshes at once.
  const bool due = f.now - lastRefresh_ >= kRefreshPeriodS || f.now < lastRefresh_;
  if (!due && visible == lastVisible_) return;
  lines_ = formatHudLines(f.settings.overlay, f.snapshot);
  lastVisible_ = visible;
  lastRefresh_ = f.now;
}

bool HudOverlayLayer::draw(OverlayFrame& f) {
  refreshLines(f);
  OverlaySettings& o = f.settings.overlay;
  if (lines_.empty() && !f.editMode) return false;

  ImFont* font = font_ ? font_ : ImGui::GetFont();
  const OverlayLayout layout = computeOverlayLayout(o, f.screenW, f.screenH);
  const float scale = resolutionScale(f.screenH);

  // In edit mode an empty block still needs something to grab.
  static const std::vector<std::string> kPlaceholder{"(no metrics)"};
  const std::vector<std::string>& lines = lines_.empty() ? kPlaceholder : lines_;

  float textW = 0.0f;
  for (const std::string& line : lines)
    textW = std::max(textW, font->CalcTextSizeA(layout.fontPx, FLT_MAX, 0.0f, line.c_str()).x);
  const float textH = static_cast<float>(lines.size() - 1) * layout.lineHeightPx + layout.fontPx;

  // Keep the whole block on screen (e.g. after a font-size increase near an edge).
  auto clampX = [&](float x) { return std::clamp(x, 0.0f, std::max(0.0f, f.screenW - textW)); };
  auto clampY = [&](float y) { return std::clamp(y, 0.0f, std::max(0.0f, f.screenH - textH)); };
  if (!f.editMode) dragging_ = false;
  const float originX = dragging_ ? dragX_ : clampX(layout.x);
  const float originY = dragging_ ? dragY_ : clampY(layout.y);

  const float pad = std::round(0.4f * layout.fontPx);
  const ImVec2 winPos(originX - pad, originY - pad);
  const ImVec2 winSize(textW + 2.0f * pad, textH + 2.0f * pad);

  ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                           ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                           ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus |
                           ImGuiWindowFlags_NoMove;
  if (!f.editMode) flags |= ImGuiWindowFlags_NoInputs;

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1.0f, 1.0f));
  ImGui::SetNextWindowPos(winPos, ImGuiCond_Always);
  ImGui::SetNextWindowSize(winSize, ImGuiCond_Always);
  ImGui::Begin("##ft_hud", nullptr, flags);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->PushClipRectFullScreen();  // shadow / hint may extend past the window rect

  const ImVec2 boxMin = winPos;
  const ImVec2 boxMax(winPos.x + winSize.x, winPos.y + winSize.y);
  const float rounding = 0.5f * pad;
  const float opacity = o.opacity;

  if (o.background)
    dl->AddRectFilled(boxMin, boxMax, toU32(Color::fromHex(0x050A08), o.backgroundAlpha * opacity), rounding);
  if (f.editMode) {
    // Visible even at zero opacity so the user can always find and grab the block.
    dl->AddRectFilled(boxMin, boxMax, IM_COL32(10, 20, 14, 90), rounding);
    const ImU32 borderCol = toU32(o.textColor, 0.9f);
    if (o.locked) dl->AddRect(boxMin, boxMax, borderCol, rounding, 1.0f);
    else drawDashedRect(dl, boxMin, boxMax, borderCol, std::max(4.0f, 6.0f * scale), 1.0f);
  }

  // Text: 1 px (scaled) drop shadow, then the text. Pixel-snapped for crisp glyphs.
  const float shadowOffset = std::max(1.0f, std::round(scale));
  const ImU32 textCol = toU32(o.textColor, opacity);
  const ImU32 shadowCol = IM_COL32(0, 0, 0, static_cast<int>(std::lround(255.0f * std::clamp(o.shadowAlpha * opacity, 0.0f, 1.0f))));
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const ImVec2 p(std::round(originX), std::round(originY + static_cast<float>(i) * layout.lineHeightPx));
    const char* text = lines[i].c_str();
    if (o.shadow) dl->AddText(font, layout.fontPx, ImVec2(p.x + shadowOffset, p.y + shadowOffset), shadowCol, text);
    dl->AddText(font, layout.fontPx, p, textCol, text);
  }

  bool modified = false;
  if (f.editMode) {
    ImGui::SetCursorScreenPos(winPos);
    ImGui::InvisibleButton("##ft_hud_drag", winSize);
    if (!o.locked) {
      if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
      if (ImGui::IsItemActivated()) {
        dragging_ = true;
        dragStartX_ = dragX_ = originX;
        dragStartY_ = dragY_ = originY;
      }
      if (dragging_ && ImGui::IsItemActive()) {
        const ImVec2 delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f);
        dragX_ = clampX(dragStartX_ + delta.x);
        dragY_ = clampY(dragStartY_ + delta.y);
      } else if (dragging_) {
        // Released: persist as normalized coordinates so it follows resolution changes.
        storeOverlayPosition(o, dragX_, dragY_, textW, textH, f.screenW, f.screenH);
        dragging_ = false;
        modified = true;
      }
    }

    const std::string hint = (o.locked ? std::string("locked") : std::string("drag to move")) +
                             " \xC2\xB7 " + f.settings.hotkeys.toggleEditMode.toString() + " to finish";
    const float hintSize = ImGui::GetFontSize();
    const ImVec2 hintDim = ImGui::GetFont()->CalcTextSizeA(hintSize, FLT_MAX, 0.0f, hint.c_str());
    const ImVec2 hintPos(boxMin.x, boxMax.y + 4.0f);
    dl->AddRectFilled(ImVec2(hintPos.x - 3.0f, hintPos.y - 1.0f),
                      ImVec2(hintPos.x + hintDim.x + 3.0f, hintPos.y + hintDim.y + 1.0f),
                      IM_COL32(0, 0, 0, 150), 3.0f);
    dl->AddText(ImGui::GetFont(), hintSize, hintPos, toU32(o.textColor, 0.95f), hint.c_str());
  }

  dl->PopClipRect();
  ImGui::End();
  ImGui::PopStyleVar(3);
  return modified;
}

}  // namespace ft
