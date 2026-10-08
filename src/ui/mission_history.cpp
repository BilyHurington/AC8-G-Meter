#include "ui/mission_history.h"

#include <algorithm>
#include <chrono>
#include <cfloat>
#include <exception>

#include <imgui.h>

#include "config/settings_store.h"
#include "ui/format.h"
#include "ui/style.h"

namespace ft {

MissionHistoryWindow::Result MissionHistoryWindow::draw(MissionArchive& archive, bool& open) {
  Result result;
  if (!open) {
    wasOpen_ = false;
    confirmDelete_.reset();
    return result;
  }
  if (!wasOpen_) archive.refreshAsync();  // the directory may have changed while closed
  wasOpen_ = true;
  if (archive.indexVersion() != seenVersion_ || index_.refreshing) {
    index_ = archive.index();  // copied only while something changes
    seenVersion_ = index_.version;
  }

  // A load finished: hand it to the summary (or show why it failed).
  if (pending_.valid() && pending_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    try {
      LoadedMission loaded = pending_.get();
      if (loaded.report) {
        result.report = std::move(loaded.report);
        result.entry = std::move(loaded.entry);
        loadError_.clear();
      } else {
        loadError_ = loaded.error;
      }
    } catch (const std::exception& e) {
      loadError_ = e.what();
    }
    pendingPath_.clear();
  }

  const float fs = ImGui::GetFontSize();
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(std::min(fs * 52.0f, vp->WorkSize.x * 0.9f), std::min(fs * 30.0f, vp->WorkSize.y * 0.8f)),
                           ImGuiCond_Appearing);
  ImGui::SetNextWindowSizeConstraints(ImVec2(fs * 30.0f, fs * 14.0f), ImVec2(FLT_MAX, FLT_MAX));
  ImGui::SetNextWindowBgAlpha(1.0f);
  if (!ImGui::Begin("Mission history###ft_history", &open, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse)) {
    ImGui::End();
    return result;
  }

  if (ImGui::Button("Refresh")) archive.refreshAsync();
  ImGui::SameLine();
  if (pending_.valid()) ImGui::TextColored(kHudWarn, "Loading...");
  else if (index_.refreshing) ImGui::TextColored(kHudWarn, "Reading folder...");
  else ImGui::TextDisabled("%zu saved mission%s", index_.entries.size(), index_.entries.size() == 1 ? "" : "s");
  if (!index_.error.empty()) ImGui::TextColored(kHudError, "%s", index_.error.c_str());
  if (!loadError_.empty()) ImGui::TextColored(kHudError, "Cannot open: %s", loadError_.c_str());

  // Room below the table for the folder path (two lines) and the empty-archive note.
  const float footer = ImGui::GetTextLineHeightWithSpacing() * (index_.entries.empty() ? 3.5f : 2.5f);
  const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                                ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable;
  if (ImGui::BeginTable("##ft_history", 7, flags, ImVec2(0.0f, -footer))) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Date");
    ImGui::TableSetupColumn("Mission", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Time");
    ImGui::TableSetupColumn("Max G");
    ImGui::TableSetupColumn("Min G");
    ImGui::TableSetupColumn("");
    ImGui::TableSetupColumn("##del");
    ImGui::TableHeadersRow();
    for (std::size_t i = 0; i < index_.entries.size(); ++i) {
      const MissionIndexEntry& e = index_.entries[i];
      ImGui::PushID(static_cast<int>(i));
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      const bool loadingThis = pending_.valid() && pendingPath_ == e.path;
      const std::string date = formatLocalDateTime(e.savedAtUnix);
      if (ImGui::Selectable(date.c_str(), loadingThis,
                            ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap) &&
          !pending_.valid()) {
        pending_ = archive.loadAsync(e);
        pendingPath_ = e.path;
        loadError_.clear();
      }
      if (ImGui::BeginItemTooltip()) {
        ImGui::TextUnformatted(pathToUtf8(e.path.filename()).c_str());
        ImGui::TextDisabled("Click to open the summary");
        ImGui::EndTooltip();
      }
      ImGui::TableNextColumn();
      if (e.label.empty()) ImGui::TextDisabled("mission %u", e.id);
      else ImGui::TextUnformatted(e.label.c_str());
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(formatClock(e.durationS).c_str());
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(e.hasGData ? formatNumber(e.maxG, 1, true, 5).c_str() : "   --");
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(e.hasGData ? formatNumber(e.minG, 1, true, 5).c_str() : "   --");
      ImGui::TableNextColumn();
      if (e.aborted) ImGui::TextColored(kHudWarn, "aborted");
      ImGui::TableNextColumn();
      if (ImGui::SmallButton("Delete")) confirmDelete_ = e;
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  if (index_.entries.empty() && !index_.refreshing)
    ImGui::TextDisabled("No saved missions yet. Finished missions are saved here automatically.");
  ImGui::TextDisabled("Folder:");
  ImGui::SameLine();
  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextDisabled("%s", pathToUtf8(archive.directory()).c_str());
  ImGui::PopTextWrapPos();

  if (confirmDelete_) ImGui::OpenPopup("Delete mission?");
  ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  if (ImGui::BeginPopupModal("Delete mission?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    if (confirmDelete_) {
      ImGui::Text("Delete the mission saved %s?", formatLocalDateTime(confirmDelete_->savedAtUnix).c_str());
      if (!confirmDelete_->label.empty()) ImGui::TextDisabled("%s", confirmDelete_->label.c_str());
      ImGui::TextDisabled("Both files (summary and G trace) are removed from disk.");
    }
    ImGui::Spacing();
    if (ImGui::Button("Delete")) {
      if (confirmDelete_) archive.removeAsync(*confirmDelete_);
      confirmDelete_.reset();
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
      confirmDelete_.reset();
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  ImGui::End();
  return result;
}

}  // namespace ft
