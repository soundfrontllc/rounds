#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "latency_probe.h"
#include "looper.h"
#include "rounds_core.h"
#include "spsc_queue.h"

namespace rounds {

// Owns the looper and everything the audio thread touches. process() is the
// real-time entry point; every other public method is for other threads.
class Engine {
public:
    Engine(double sampleRate, double maxLoopSeconds, int maxBlockFrames);

    // Real-time.
    void process(const float* input, float* outL, float* outR, int frames);
    void processTimed(const float* input, float* outL, float* outR, int frames, uint64_t hostTime,
                      double hostTicksPerSample);

    // Any non-real-time thread.
    bool send(RoundsCommand cmd, int64_t sampleTime);
    bool sendAtHostTime(RoundsCommand cmd, uint64_t hostTime);
    void setLatency(int inputLatency, int outputLatency);
    void setMonitorGain(float gain) { monitorGain_.store(gain); }
    void setLoopGain(float gain) { loopGain_.store(gain); }
    void startCalibration() { calibrateRequested_.store(true); }
    void getStatus(RoundsStatus* out);
    bool pollEvent(RoundsEvent* out) { return events_.pop(*out); }

    double sampleRate() const { return sampleRate_; }

private:
    struct Command {
        int32_t cmd;
        int64_t time;
    };

    void apply(int32_t cmd);
    void publish();
    void emit(RoundsEventType type, int64_t value);
    void processChunk(const float* input, float* out, int frames);

    double sampleRate_;
    int maxBlock_;
    Looper looper_;
    LatencyProbe probe_;

    // Commands: producers serialize on a mutex (never touched by the audio
    // thread), the audio thread pops lock-free.
    std::mutex producerMutex_;
    SpscQueue<Command, 256> commands_;
    SpscQueue<RoundsEvent, 1024> events_;

    // Audio-thread-only state.
    Command pending_[64];
    int pendingCount_ = 0;
    int64_t now_ = 0;
    std::vector<float> zeros_;
    int lastState_ = -1;
    int64_t lastLength_ = -1;
    int lastLayers_ = -1;
    int appliedInputLatency_ = -1;
    int appliedOutputLatency_ = -1;

    // Settings written by other threads.
    std::atomic<int> inputLatency_{0};
    std::atomic<int> outputLatency_{0};
    std::atomic<float> monitorGain_{0.0f};
    std::atomic<float> loopGain_{1.0f};
    std::atomic<bool> calibrateRequested_{false};

    // Published by the audio thread for the UI.
    std::atomic<int> statState_{0};
    std::atomic<int64_t> statLength_{0};
    std::atomic<int64_t> statRecorded_{0};
    std::atomic<int64_t> statPosition_{0};
    std::atomic<int> statLayers_{0};
    std::atomic<int> statLayerOpen_{0};
    std::atomic<float> statInputPeak_{0.0f};
    std::atomic<float> statOutputPeak_{0.0f};
    std::atomic<int> statCalibrating_{0};
    std::atomic<int64_t> statNow_{0};
    std::atomic<int> statBlock_{0};

    // Host time <-> sample time mapping (seqlock: audio thread writes).
    std::atomic<uint32_t> mapSeq_{0};
    std::atomic<uint64_t> mapHost_{0};
    std::atomic<int64_t> mapSample_{0};
    std::atomic<double> mapTicksPerSample_{0.0};
};

}  // namespace rounds
