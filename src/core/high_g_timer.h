#pragma once
// Sustained high-G episode detector with hysteresis, so G hovering around the
// threshold does not chatter between episodes.

namespace ft {

class HighGTimer {
 public:
  struct Step {
    bool entered = false;  // an episode started on this update
    bool exited = false;   // an episode ended on this update
  };

  void setThresholds(double enterG, double exitG) {
    enterG_ = enterG;
    exitG_ = exitG;
  }
  void reset() {
    active_ = false;
    durationS_ = 0.0;
  }

  // Enter at g >= enter, leave at g < exit. The episode clock starts at 0 on entry and
  // advances by dt on each following update while still inside.
  Step update(double g, double dtS) {
    Step step;
    if (!active_ && g >= enterG_) {
      active_ = true;
      durationS_ = 0.0;
      step.entered = true;
    } else if (active_ && g < exitG_) {
      active_ = false;
      durationS_ = 0.0;
      step.exited = true;
    } else if (active_) {
      durationS_ += dtS;
    }
    return step;
  }

  bool active() const { return active_; }
  double durationS() const { return durationS_; }

 private:
  double enterG_ = 6.0;
  double exitG_ = 5.5;
  bool active_ = false;
  double durationS_ = 0.0;
};

}  // namespace ft
