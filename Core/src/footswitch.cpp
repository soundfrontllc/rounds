#include "footswitch.h"

namespace rounds {

RoundsGesture Footswitch::press(double time) {
    if (down_) return ROUNDS_GESTURE_NONE;  // repeated "down" messages
    down_ = true;
    holdFired_ = false;
    longHoldFired_ = false;
    const bool isDouble = lastPressCanDouble_ && time - lastPressTime_ <= config_.doubleTapSeconds;
    pressTime_ = time;
    lastPressTime_ = time;
    pressWasTap_ = !isDouble;
    // A third quick press starts a new tap rather than another double-tap.
    lastPressCanDouble_ = !isDouble;
    return isDouble ? ROUNDS_GESTURE_DOUBLE_TAP : ROUNDS_GESTURE_TAP;
}

RoundsGesture Footswitch::tick(double time) {
    if (!down_ || !pressWasTap_) return ROUNDS_GESTURE_NONE;
    const double held = time - pressTime_;
    if (!holdFired_ && held >= config_.holdSeconds) {
        holdFired_ = true;
        lastPressCanDouble_ = false;
        return ROUNDS_GESTURE_HOLD;
    }
    if (holdFired_ && !longHoldFired_ && held >= config_.longHoldSeconds) {
        longHoldFired_ = true;
        return ROUNDS_GESTURE_LONG_HOLD;
    }
    return ROUNDS_GESTURE_NONE;
}

RoundsGesture Footswitch::release(double time) {
    if (!down_) return ROUNDS_GESTURE_NONE;
    // Catch a hold that crossed its threshold between ticks.
    const RoundsGesture late = tick(time);
    down_ = false;
    if (time - pressTime_ > config_.doubleTapSeconds) lastPressCanDouble_ = false;
    return late;
}

}  // namespace rounds
