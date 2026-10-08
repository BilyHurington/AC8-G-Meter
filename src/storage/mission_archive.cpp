#include "storage/mission_archive.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <fstream>
#include <string_view>
#include <system_error>
#include <utility>

namespace ft {
namespace {

namespace fs = std::filesystem;

// Temp files and orphaned traces older than this are leftovers of a crash mid-save (a save
// in progress, possibly by another process, is never that old).
constexpr auto kStaleAge = std::chrono::minutes(10);
constexpr std::uintmax_t kMaxJsonBytes = 4u << 20;
constexpr std::uintmax_t kMaxTraceBytes = 512u << 20;
constexpr const char* kTraceSuffix = ".trace.csv";

// UTF-8 for error messages (same as config's pathToUtf8, which this layer does not link).
std::string utf8(const fs::path& p) {
  try {
    const std::u8string u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
  } catch (const std::exception&) {
    return "(path not representable as UTF-8)";
  }
}

fs::path pathFromUtf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

bool readFile(const fs::path& p, std::uintmax_t maxBytes, std::string& out, std::string* error) {
  std::error_code ec;
  const std::uintmax_t size = fs::file_size(p, ec);
  if (ec || size > maxBytes) {
    if (error) *error = "cannot read " + utf8(p) + (ec ? ": " + ec.message() : ": file too large");
    return false;
  }
  std::ifstream in(p, std::ios::binary);
  out.resize(static_cast<std::size_t>(size));
  if (!in || !in.read(out.data(), static_cast<std::streamsize>(size))) {
    if (error) *error = "cannot read " + utf8(p);
    return false;
  }
  return true;
}

// Writes "<path>.tmp" and renames it over `path`: readers never see a half-written file.
bool writeFileAtomic(const fs::path& path, const std::string& text, std::string* error) {
  fs::path tmp = path;
  tmp += ".tmp";
  bool ok = false;
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (out) {
      out.write(text.data(), static_cast<std::streamsize>(text.size()));
      out.flush();
      ok = static_cast<bool>(out);
    }
  }
  std::error_code ec;
  if (ok) fs::rename(tmp, path, ec);
  if (!ok || ec) {
    if (error) *error = "cannot write " + utf8(path) + (ec ? ": " + ec.message() : "");
    fs::remove(tmp, ec);
    return false;
  }
  return true;
}

bool endsWith(const std::string& s, const char* suffix) {
  const std::string_view sv(suffix);
  return s.size() >= sv.size() && s.compare(s.size() - sv.size(), sv.size(), sv) == 0;
}

// "YYYYMMDD-HHMMSS_m....json": only files this archive named are subject to retention.
bool isArchiveJsonName(const std::string& name) {
  if (name.size() < 22 || !endsWith(name, ".json")) return false;
  for (std::size_t i = 0; i < 15; ++i) {
    const bool digit = name[i] >= '0' && name[i] <= '9';
    if (i == 8 ? name[i] != '-' : !digit) return false;
  }
  return name[15] == '_' && name[16] == 'm';
}

bool isStale(const fs::path& p) {
  std::error_code ec;
  const auto written = fs::last_write_time(p, ec);
  return !ec && fs::file_time_type::clock::now() - written > kStaleAge;
}

// Removes leftovers of interrupted saves: temp files, and traces whose JSON never got written.
void removeStaleFiles(const fs::path& p) {
  const std::string name = utf8(p.filename());
  std::error_code ec;
  if (endsWith(name, ".tmp")) {
    if (isStale(p)) fs::remove(p, ec);
  } else if (endsWith(name, kTraceSuffix)) {
    const std::string stem = name.substr(0, name.size() - std::string_view(kTraceSuffix).size());
    if (!fs::exists(p.parent_path() / pathFromUtf8(stem + ".json"), ec) && isStale(p)) fs::remove(p, ec);
  }
}

LoadedMission loadMission(const MissionIndexEntry& entry) {
  LoadedMission out;
  out.entry = entry;
  std::string text, error, traceName;
  std::size_t rows = 0;
  auto report = std::make_shared<MissionReport>();
  MissionIndexEntry e;
  if (!readFile(entry.path, kMaxJsonBytes, text, &error) ||
      !missionFromJson(text, *report, e, traceName, rows, &error)) {
    out.error = error;
    return out;
  }
  e.path = entry.path;
  e.tracePath = entry.path.parent_path() / pathFromUtf8(traceName);
  if (!readFile(e.tracePath, kMaxTraceBytes, text, &error) || !traceFromCsv(text, report->trace, &error)) {
    out.error = error;
    return out;
  }
  if (report->trace.size() != rows) {
    out.error = "trace has " + std::to_string(report->trace.size()) + " rows, expected " + std::to_string(rows);
    return out;
  }
  out.entry = e;
  out.report = std::move(report);
  return out;
}

}  // namespace

MissionArchive::MissionArchive(std::filesystem::path directory, int maxSaved)
    : dir_(std::move(directory)), maxSaved_(std::max(maxSaved, 0)) {
  try {
    worker_ = std::thread([this] { workerLoop(); });
  } catch (const std::exception&) {
    // No thread available: post() runs jobs inline instead. Slower, but nothing is lost.
  }
}

MissionArchive::~MissionArchive() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  wake_.notify_all();
  if (worker_.joinable()) worker_.join();
}

void MissionArchive::post(std::function<void()> job) {
  if (!worker_.joinable()) {
    job();
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    jobs_.push_back(std::move(job));
  }
  wake_.notify_one();
}

void MissionArchive::workerLoop() {
  std::unique_lock<std::mutex> lock(mutex_);
  for (;;) {
    wake_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
    if (jobs_.empty()) break;  // stop requested and every queued job done
    std::function<void()> job = std::move(jobs_.front());
    jobs_.pop_front();
    busy_ = true;
    lock.unlock();
    job();  // jobs catch their own exceptions
    lock.lock();
    busy_ = false;
    if (jobs_.empty()) idle_.notify_all();
  }
  idle_.notify_all();
}

void MissionArchive::flush() {
  if (!worker_.joinable()) return;
  std::unique_lock<std::mutex> lock(mutex_);
  idle_.wait(lock, [this] { return jobs_.empty() && !busy_; });
}

void MissionArchive::saveAsync(std::shared_ptr<const MissionReport> report) {
  if (!report) return;
  try {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      lastSave_ = SaveStatus{SaveStatus::State::Pending, report, {}, {}};
    }
    post([this, report] { doSave(report); });
  } catch (const std::exception& e) {
    std::lock_guard<std::mutex> lock(mutex_);
    lastSave_ = SaveStatus{SaveStatus::State::Failed, report, {}, e.what()};
  }
}

SaveStatus MissionArchive::lastSave() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return lastSave_;
}

void MissionArchive::doSave(const std::shared_ptr<const MissionReport>& report) {
  SaveStatus st{SaveStatus::State::Failed, report, {}, {}};
  try {
    std::error_code ec;
    fs::create_directories(dir_, ec);
    if (ec) {
      st.error = "cannot create " + utf8(dir_) + ": " + ec.message();
    } else {
      const std::int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count();
      // Report ids restart with every process: the time stamp keeps names unique, and a
      // suffix resolves a collision within the same second.
      const std::string base = missionFileStamp(now) + "_m" + std::to_string(report->id);
      std::string stem = base;
      for (int n = 2; fs::exists(dir_ / (stem + ".json"), ec) || fs::exists(dir_ / (stem + kTraceSuffix), ec); ++n)
        stem = base + "-" + std::to_string(n);
      const fs::path json = dir_ / (stem + ".json");
      const fs::path trace = dir_ / (stem + kTraceSuffix);
      // Trace first, JSON last: a JSON file always has its complete trace next to it.
      if (writeFileAtomic(trace, traceToCsv(*report), &st.error)) {
        if (writeFileAtomic(json, missionToJson(*report, now, stem + kTraceSuffix), &st.error)) {
          st.state = SaveStatus::State::Saved;
          st.path = json;
        } else {
          fs::remove(trace, ec);
        }
      }
    }
  } catch (const std::exception& e) {
    st.error = e.what();
  }
  if (st.state == SaveStatus::State::Saved) applyRetention();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (lastSave_.report == report) lastSave_ = st;  // unless a newer save replaced it
  }
  doList();
}

void MissionArchive::applyRetention() {
  const int maxSaved = maxSaved_.load();
  if (maxSaved <= 0) return;
  try {
    std::vector<fs::path> files;
    std::error_code ec;
    for (fs::directory_iterator it(dir_, ec), end; !ec && it != end; it.increment(ec))
      if (isArchiveJsonName(utf8(it->path().filename()))) files.push_back(it->path());
    if (files.size() <= static_cast<std::size_t>(maxSaved)) return;
    // The names start with the UTC save time, so name order is age order.
    std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) { return a.filename() < b.filename(); });
    for (std::size_t i = 0; i + static_cast<std::size_t>(maxSaved) < files.size(); ++i) {
      const std::string name = utf8(files[i].filename());
      fs::remove(files[i], ec);  // JSON first: the pair disappears from the index at once
      fs::remove(dir_ / pathFromUtf8(name.substr(0, name.size() - 5) + kTraceSuffix), ec);
    }
  } catch (const std::exception&) {
    // Retention is best effort; the next save tries again.
  }
}

void MissionArchive::refreshAsync() {
  try {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++pendingLists_;
      index_.refreshing = true;
    }
    post([this] {
      doList();
      std::lock_guard<std::mutex> lock(mutex_);
      --pendingLists_;
      index_.refreshing = pendingLists_ > 0;
    });
  } catch (const std::exception& e) {
    std::lock_guard<std::mutex> lock(mutex_);
    index_.error = e.what();
  }
}

void MissionArchive::doList() {
  std::vector<MissionIndexEntry> entries;
  std::string error;
  try {
    std::error_code ec;
    if (fs::is_directory(dir_, ec)) {
      for (fs::directory_iterator it(dir_, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path p = it->path();
        removeStaleFiles(p);
        if (p.extension() != ".json") continue;
        std::string text, traceName;
        std::size_t rows = 0;
        MissionReport report;
        MissionIndexEntry e;
        // Partial, corrupt or foreign files are skipped silently.
        if (!readFile(p, kMaxJsonBytes, text, nullptr) || !missionFromJson(text, report, e, traceName, rows, nullptr))
          continue;
        e.path = p;
        e.tracePath = dir_ / pathFromUtf8(traceName);
        std::error_code existsEc;
        if (!fs::is_regular_file(e.tracePath, existsEc)) continue;
        entries.push_back(std::move(e));
      }
      if (ec) error = "cannot list " + utf8(dir_) + ": " + ec.message();
    }
    std::sort(entries.begin(), entries.end(), [](const MissionIndexEntry& a, const MissionIndexEntry& b) {
      if (a.savedAtUnix != b.savedAtUnix) return a.savedAtUnix > b.savedAtUnix;
      return a.path.filename() > b.path.filename();
    });
  } catch (const std::exception& e) {
    error = e.what();
  }
  std::lock_guard<std::mutex> lock(mutex_);
  index_.entries = std::move(entries);
  index_.error = error;
  indexVersion_.store(++index_.version);
}

MissionIndex MissionArchive::index() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return index_;
}

std::future<LoadedMission> MissionArchive::loadAsync(const MissionIndexEntry& entry) {
  auto promise = std::make_shared<std::promise<LoadedMission>>();
  std::future<LoadedMission> result = promise->get_future();
  try {
    post([promise, entry] {
      try {
        promise->set_value(loadMission(entry));
      } catch (const std::exception& e) {
        LoadedMission failed;
        failed.entry = entry;
        failed.error = e.what();
        promise->set_value(std::move(failed));
      }
    });
  } catch (const std::exception& e) {
    LoadedMission failed;
    failed.entry = entry;
    failed.error = e.what();
    promise->set_value(std::move(failed));
  }
  return result;
}

void MissionArchive::removeAsync(const MissionIndexEntry& entry) {
  try {
    post([this, entry] {
      std::string error;
      try {
        std::error_code ec;
        if (!fs::remove(entry.path, ec) && ec) error = "cannot delete " + utf8(entry.path) + ": " + ec.message();
        if (!entry.tracePath.empty()) fs::remove(entry.tracePath, ec);
      } catch (const std::exception& e) {
        error = e.what();
      }
      doList();
      if (!error.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        index_.error = error;
      }
    });
  } catch (const std::exception& e) {
    std::lock_guard<std::mutex> lock(mutex_);
    index_.error = e.what();
  }
}

}  // namespace ft
