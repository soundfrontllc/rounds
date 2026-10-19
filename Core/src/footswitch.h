#pragma once

#include "rounds_core.h"

namespace rounds {

// Turns footswitch press/release times into gestures.
//
// A tap fires immediately on press so recording starts without delay. A
// second press shortly after a tap is a double-tap, and a hold fires while
// the switch is still down. Because the tap already happened, the engine
// commands for double-tap and hold first revert what that tap did (see
// Looper::undoRevertingTap / stopRevertingTap).
class Footswitch {
public:
    explicit Footswitch(const RoundsFootswitchConfig& config) : config_(config) {}

    RoundsGesture press(double time);
    RoundsGesture release(double time);
    RoundsGesture tick(double time);

private:
    RoundsFootswitchConfig config_;
    bool down_ = false;
    double pressTime_ = -1e9;
    bool pressWasTap_ = false;   // this press fired a TAP (holds only follow taps)
    bool holdFired_ = false;
    bool longHoldFired_ = false;
    bool lastPressCanDouble_ = false;  // previous press was a short tap
    double lastPressTime_ = -1e9;
};

}  // namespace rounds
