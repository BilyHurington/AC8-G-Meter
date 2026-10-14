#include "runtime/debug_recorder.h"

#include <chrono>
#include <cmath>
#include <ctime>
#include <limits>

#include "core/path_utf8.h"

namespace ft::mod {

namespace {

constexpr double kCmToM = 0.01;
constexpr std::uint64_t kFlushEveryRows = 120;  // about 2 s at 60 fps

// Local time with milliseconds: switching recording off and on again makes a new file.
std::string stamp() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm local{};
  localtime_s(&local, &t);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y%m%d_%H%M%S", &local);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
  char full[48];
  std::snprintf(full, sizeof full, "%s_%03d", buf, static_cast<int>(ms));
  return full;
}

// Appends ",value"; NaN / infinities as an empty field.
void field(std::string& row, double v, const char* format = "%.6f") {
  row += ',';
  if (!std::isfinite(v)) return;
  char buf[64];
  std::snprintf(buf, sizeof buf, format, v);
  row += buf;
}

void field(std::string& row, const Vec3& v, const char* format = "%.6f") {
  field(row, v.x, format);
  field(row, v.y, format);
  field(row, v.z, format);
}

}  // namespace

bool DebugRecorder::open(const std::filesystem::path& dir, std::string* error) {
  close();
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  path_ = dir / ("ac8_debug_" + stamp() + ".csv");
  file_.open(path_, std::ios::out | std::ios::trunc | std::ios::binary);
  if (!file_.is_open()) {
    if (error) *error = "cannot open '" + pathToUtf8(path_) + "' for writing";
    return false;
  }
  buffer_.resize(256 * 1024);
  file_.rdbuf()->pubsetbuf(buffer_.data(), static_cast<std::streamsize>(buffer_.size()));  // MSVC: after open
  rows_ = 0;
  hasPrevious_ = false;

  std::string header =
      "frame,wall_s,world_t,delta_s,paused,has_plane,pawn_id,"
      "loc_x_cm,loc_y_cm,loc_z_cm,vel_x_cms,vel_y_cms,vel_z_cms,"
      "fwd_x,fwd_y,fwd_z,right_x,right_y,right_z,up_x,up_y,up_z";
  for (int i = 0; i < kLuaDebugFieldCount; ++i) {
    header += ',';
    header += luaDebugFieldName(static_cast<LuaDebugField>(i));
  }
  header +=
      ",pushed,pushed_phase,"
      "fd_dt_s,fd_ax_ms2,fd_ay_ms2,fd_az_ms2,fd_nx_g,fd_ny_g,fd_nz_g,fd_total_g,"
      "phase,valid,g,g_raw,n_x_g,n_y_g,n_z_g,g_total,onset_gs,aoa_deg,sideslip_deg,speed_ms,altitude_m,"
      "accel_window_s,tau_s,rate_hz,vel_noise_ms,accepted,rejected_spike,rejected_time,resets_teleport,resets_jump";
  file_ << "# AC8 G-Meter debug recording. Units: cm and cm/s as the game reports them (UE units),\n"
           "# fd_* = finite difference of vel between consecutive frames (m/s^2; n = load factor in\n"
           "# body axes incl. gravity, G), then the pipeline's output for the frame. Empty = not available.\n"
        << header << '\n';
  return true;
}

void DebugRecorder::close() {
  if (file_.is_open()) {
    file_.flush();
    file_.close();
  }
}

void DebugRecorder::write(const LuaFrame& f, std::span<const RawFlightSample> pushed, const MetricsSnapshot& s) {
  if (!file_.is_open()) return;
  constexpr double nan = std::numeric_limits<double>::quiet_NaN();
  static const auto start = std::chrono::steady_clock::now();
  const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

  std::string row = std::to_string(rows_);
  field(row, wall);
  field(row, f.worldTimeS);
  field(row, f.deltaS);
  field(row, f.paused ? 1.0 : 0.0, "%.0f");
  field(row, f.hasPlane ? 1.0 : 0.0, "%.0f");
  field(row, f.pawnId, "%.0f");
  field(row, f.locationCm, "%.3f");
  field(row, f.velocityCmS, "%.3f");
  field(row, f.attitude.forward);
  field(row, f.attitude.right);
  field(row, f.attitude.up);
  for (int i = 0; i < kLuaDebugFieldCount; ++i)
    field(row, f.debug ? (*f.debug)[static_cast<std::size_t>(i)] : nan);
  field(row, static_cast<double>(pushed.size()), "%.0f");
  row += ',';
  if (!pushed.empty()) row += toString(pushed.back().phase);

  // Plain finite difference of the reported velocity: a = dv/dt, load factor n = (a + g)/g0
  // resolved in body axes, exactly what the pipeline measures before its gate and filters.
  const Vec3 velocity = f.velocityCmS * kCmToM;
  const double dt = f.worldTimeS - previousT_;
  if (f.hasPlane && hasPrevious_ && f.pawnId == previousPawn_ && dt > 0.0) {
    const Vec3 a = (velocity - previousVelocity_) / dt;
    const Vec3 n = f.attitude.toBody(a - kGravityWorld) / kG0;
    field(row, dt);
    field(row, a, "%.3f");
    field(row, n, "%.3f");
    field(row, length(n), "%.3f");
  } else {
    for (int i = 0; i < 8; ++i) row += ',';
  }
  if (f.hasPlane && !f.paused) {  // paused frames repeat the same state
    hasPrevious_ = true;
    previousT_ = f.worldTimeS;
    previousPawn_ = f.pawnId;
    previousVelocity_ = velocity;
  } else if (!f.hasPlane) {
    hasPrevious_ = false;
  }

  const FlightMetrics& m = s.current;
  row += ',';
  row += toString(s.phase);
  field(row, m.valid ? 1.0 : 0.0, "%.0f");
  field(row, m.g, "%.3f");
  field(row, m.gRaw, "%.3f");
  field(row, m.loadFactorBody, "%.3f");
  field(row, m.gTotal, "%.3f");
  field(row, m.onsetRate, "%.2f");
  field(row, m.aoaValid ? m.aoaDeg : nan, "%.2f");
  field(row, m.aoaValid ? m.sideslipDeg : nan, "%.2f");
  field(row, m.speed, "%.2f");
  field(row, m.altitude, "%.2f");
  field(row, s.filter.accelWindowS, "%.4f");
  field(row, s.filter.smoothingTauS, "%.4f");
  field(row, s.filter.sampleRateHz, "%.1f");
  field(row, s.filter.velocityNoiseMS, "%.4f");
  for (const std::uint64_t q : {s.quality.accepted, s.quality.rejectedSpike, s.quality.rejectedTime,
                                s.quality.resetsTeleport, s.quality.resetsJump})
    field(row, static_cast<double>(q), "%.0f");
  file_ << row << '\n';
  if (++rows_ % kFlushEveryRows == 0) file_.flush();
}

}  // namespace ft::mod
