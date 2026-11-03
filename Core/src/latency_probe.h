#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace rounds {

// Measures round-trip latency: emits clicks on the output and times how long
// each takes to show up on the input. Real-time safe, no allocation.
class LatencyProbe {
public:
    static constexpr int kClicks = 8;

    explicit LatencyProbe(double sampleRate)
        : quietSamples_(static_cast<int64_t>(sampleRate * 0.25)),
          period_(static_cast<int64_t>(sampleRate * 0.5)) {}

    void start() {
        active_ = true;
        finished_ = false;
        counter_ = 0;
        noisePeak_ = 0.0f;
        found_ = 0;
        clickIndex_ = -1;
        detected_ = false;
    }

    bool active() const { return active_; }
    // Valid after active() turns false: median round trip in samples, or -1.
    int64_t result() const { return result_; }
    bool takeFinished() {
        const bool f = finished_;
        finished_ = false;
        return f;
    }

    // Consumes one input sample and returns the output sample to play.
    float process(float in) {
        if (!active_) return 0.0f;
        const float level = std::fabs(in);
        float out = 0.0f;
        if (counter_ < quietSamples_) {
            noisePeak_ = std::max(noisePeak_, level);
        } else {
            const int64_t t = counter_ - quietSamples_;
            const int64_t phase = t % period_;
            const int click = static_cast<int>(t / period_);
            if (click >= kClicks) {
                finish();
                return 0.0f;
            }
            if (phase == 0) {
                clickIndex_ = click;
                detected_ = false;
                out = 0.9f;
            } else if (!detected_ && level > threshold()) {
                detected_ = true;
                delays_[found_++] = phase;
            }
        }
        counter_++;
        return out;
    }

private:
    float threshold() const { return std::max(0.05f, noisePeak_ * 4.0f); }

    void finish() {
        active_ = false;
        finished_ = true;
        if (found_ < kClicks / 2 + 1) {
            result_ = -1;
            return;
        }
        std::sort(delays_, delays_ + found_);
        result_ = delays_[found_ / 2];
    }

    int64_t quietSamples_;
    int64_t period_;
    bool active_ = false;
    bool finished_ = false;
    int64_t counter_ = 0;
    float noisePeak_ = 0.0f;
    int64_t delays_[kClicks] = {};
    int found_ = 0;
    int clickIndex_ = -1;
    bool detected_ = false;
    int64_t result_ = -1;
};

}  // namespace rounds
