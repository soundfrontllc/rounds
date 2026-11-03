#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "latency_probe.h"
#include "looper.h"
#include "rounds_core.h"
#include "spsc_queue.h"

namespace rounds {

// Owns the loopers and everything the audio thread touches. process() is the
// real-time entry point; every other public method is for other threads.
//
// All loops advance on one sample clock. The first loop recorded sets the
// tempo: its length is taken as 1, 2, 4 or 8 bars of 4/4 (whichever puts the
// tempo in 80-160 BPM). Later loops snap to 1, 2, 4, 8 ... bars, so loops of
// different lengths stay in step forever. With quantize on, record, play and
// stop wait for the next bar while anything plays.
class Engine {
public:
    static constexpr int kLoops = ROUNDS_MAX_LOOPS;

    Engine(double sampleRate, double maxLoopSeconds, int maxBlockFrames);

    // Real-time.
    void process(const float* input, float* outL, float* outR, int frames);
    void processTimed(const float* input, float* outL, float* outR, int frames, uint64_t hostTime,
                      double hostTicksPerSample);

    // Any non-real-time thread.
    bool send(int loop, RoundsCommand cmd, int64_t sampleTime);
    bool sendAtHostTime(int loop, RoundsCommand cmd, uint64_t hostTime);
    void setLatency(int inputLatency, int outputLatency);
    void setMonitorGain(float gain) { monitorGain_.store(gain); }
    void setLoopGain(int loop, float gain);
    void setQuantize(bool on) { quantize_.store(on); }
    void setClickGain(float gain) { clickGain_.store(std::max(0.0f, gain)); }
    // Reinterprets the base loop as `bars` bars (1, 2, 4 or 8); the audio is untouched.
    void setBars(int bars) { requestedBars_.store(bars); }
    static int chooseBars(int64_t baseLength, double sampleRate);
    void startCalibration() { calibrateRequested_.store(true); }
    void getStatus(RoundsStatus* out);
    void getLoopStatus(int loop, RoundsLoopStatus* out);
    bool pollEvent(RoundsEvent* out) { return events_.pop(*out); }

    // Session save/load: one loop at a time through a buffer the audio thread
    // copies a slice of per block (see rounds_core.h).
    bool beginExport(int loop);
    bool beginImport(int loop, const float* samples, int64_t length);
    int transferStatus();
    int64_t takeExport(float* dest, int64_t capacity);
    int64_t maxLoopSamples() const { return loops_[0]->maxLoopSamples(); }

    double sampleRate() const { return sampleRate_; }

private:
    struct Command {
        int32_t loop;
        int32_t cmd;
        int64_t time;
    };

    // What the UI last heard about each loop, to emit changes only.
    struct Published {
        int state = -1;
        int64_t length = -1;
        int layers = -1;
        int queued = 0;
    };

    // An action waiting for the next bar.
    enum Queued { kNone = 0, kQueuedTap = 1, kQueuedStop = 2 };
    struct Scheduled {
        int action = kNone;
        int64_t time = 0;
    };

    // Published by the audio thread for the UI, one per loop.
    struct LoopStats {
        std::atomic<int> state{0};
        std::atomic<int64_t> length{0};
        std::atomic<int64_t> recorded{0};
        std::atomic<int64_t> position{0};
        std::atomic<int> layers{0};
        std::atomic<int> layerOpen{0};
        std::atomic<float> peak{0.0f};
        std::atomic<int> queued{0};
    };

    void apply(int loop, int32_t cmd);
    void tap(int loop);
    void stop(int loop);
    void startStopAll();
    void fireScheduled(int loop);
    bool cancelScheduled(int loop);
    void shiftAll(int64_t delta);
    bool quantizing() const;
    int64_t clock() const { return loops_[0]->clock(); }
    void updateBase();
    void applyBars(int bars);
    bool anyAudible() const;
    bool running() const;
    void renderClick(float* out, int frames, int64_t start);
    void publish();
    void emit(RoundsEventType type, int loop, int64_t value);
    void processChunk(const float* input, float* out, int frames, int64_t start);
    void stepTransfer();
    bool importing(int loop) const;

    enum Transfer { kIdle, kExportRequested, kExporting, kExportDone, kImportRequested, kImporting, kImportDone, kFailed };
    static constexpr int64_t kTransferSlice = 32768;

    double sampleRate_;
    int maxBlock_;
    std::vector<std::unique_ptr<Looper>> loops_;
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
    std::vector<float> scratch_;          // one loop's output, for its peak
    std::array<float, kLoops> chunkPeak_{};
    std::array<Published, kLoops> published_{};
    int64_t baseLength_ = 0;
    int baseLoop_ = -1;
    int bars_ = 0;              // bars in the base loop, 0 = no tempo yet
    int64_t barLength_ = 0;     // samples
    int64_t gridOrigin_ = 0;    // real time a bar starts at (mod barLength_)
    std::array<Scheduled, kLoops> scheduled_{};
    // Click synth.
    float clickPhase_ = 0.0f;
    float clickStep_ = 0.0f;
    float clickLevel_ = 0.0f;
    float clickDecay_ = 0.0f;
    int appliedInputLatency_ = -1;
    int appliedOutputLatency_ = -1;

    // Settings written by other threads.
    std::atomic<int> inputLatency_{0};
    std::atomic<int> outputLatency_{0};
    std::atomic<float> monitorGain_{0.0f};
    std::array<std::atomic<float>, kLoops> loopGain_;
    std::atomic<bool> calibrateRequested_{false};
    std::atomic<bool> quantize_{false};
    std::atomic<float> clickGain_{0.0f};
    std::atomic<int> requestedBars_{0};

    // Transfer: other threads own the buffer while the state is Idle, *Done or
    // Failed; the audio thread owns it from *Requested until it sets *Done or Failed.
    std::atomic<int> transfer_{kIdle};
    std::vector<float> transferBuffer_;
    int transferLoop_ = -1;
    int64_t transferLength_ = 0;
    int64_t transferPos_ = 0;
    int64_t transferRotation_ = 0;  // export: loop index of saved sample 0

    std::array<LoopStats, kLoops> loopStats_;
    std::atomic<int64_t> statBase_{0};
    std::atomic<int> statBaseLoop_{-1};
    std::atomic<int> statBars_{0};
    std::atomic<int64_t> statBarLength_{0};
    std::atomic<int64_t> statCycle_{0};
    std::atomic<int64_t> statCyclePosition_{0};
    std::atomic<int> statRunning_{0};
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
