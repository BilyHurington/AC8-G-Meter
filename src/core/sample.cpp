#include "core/sample.h"

namespace ft {

const char* toString(MissionPhase phase) {
  switch (phase) {
    case MissionPhase::None: return "none";
    case MissionPhase::Active: return "active";
    case MissionPhase::Paused: return "paused";
    case MissionPhase::Ended: return "ended";
  }
  return "none";
}

}  // namespace ft
