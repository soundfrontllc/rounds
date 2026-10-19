#include "engine.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace rounds {

namespace {
Looper::Config looperConfig(double sampleRate, double maxLoopSeconds) {
    Looper::Config config;
    config.maxLoopSamples = static_cast<int64_t>(sampleRate * maxLoopSeconds);
    config.crossfadeSamples = std::max(32, static_cast<int>(sampleRate * 0.005));  // 5 ms
    config.minLoopSamples = static_cast<int64_t>(sampleRate * 0.1);
    return config;
}

void storeMax(std::atomic<float>& target, float value) {
    float current = target.load(std::memory_order_relaxed);
    while (value > current && !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
}
}  // namespace

Engine::Engine(double sampleRate, double maxLoopSeconds, int maxBlockFrames)
    : sampleRate_(sampleRate),
      maxBlock_(maxBlockFrames),
      looper_(looperConfig(sampleRate, maxLoopSeconds)),
      probe_(sampleRate) {
    zeros_.assign(static_cast<size_t>(maxBlockFrames), 0.0f);
}

// ---------------------------------------------------------------------------
// Non-real-time side

bool Engine::send(RoundsCommand cmd, int64_t sampleTime) {
    std::lock_guard<std::mutex> lock(producerMutex_);
    return commands_.push(Command{cmd, sampleTime});
}

bool Engine::sendAtHostTime(RoundsCommand cmd, uint64_t hostTime) {
    if (hostTime == 0) return send(cmd, -1);
    uint64_t mapHost;
    int64_t mapSample;
    double ticksPerSample;
    uint32_t seq;
    do {
        seq = mapSeq_.load(std::memory_order_acquire);
        mapHost = mapHost_.load(std::memory_order_relaxed);
        mapSample = mapSample_.load(std::memory_order_relaxed);
        ticksPerSample = mapTicksPerSample_.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
    } while ((seq & 1u) || seq != mapSeq_.load(std::memory_order_relaxed));

    if (ticksPerSample <= 0.0) return send(cmd, -1);
    const double deltaTicks = static_cast<double>(hostTime) - static_cast<double>(mapHost);
    const int64_t sampleAt = mapSample + static_cast<int64_t>(std::llround(deltaTicks / ticksPerSample));
    // One block of fixed delay: the moment has usually already been rendered,
    // and a constant delay feels tighter than a jittery one.
    return send(cmd, sampleAt + statBlock_.load(std::memory_order_relaxed));
}

void Engine::setLatency(int inputLatency, int outputLatency) {
    inputLatency_.store(inputLatency);
    outputLatency_.store(outputLatency);
}

void Engine::getStatus(RoundsStatus* out) {
    out->state = statState_.load();
    out->loopLength = statLength_.load();
    out->recordedLength = statRecorded_.load();
    out->playPosition = statPosition_.load();
    out->layers = statLayers_.load();
    out->layerOpen = statLayerOpen_.load();
    out->inputPeak = statInputPeak_.exchange(0.0f);
    out->outputPeak = statOutputPeak_.exchange(0.0f);
    out->inputLatency = inputLatency_.load();
    out->outputLatency = outputLatency_.load();
    out->calibrating = statCalibrating_.load();
    out->sampleTime = statNow_.load();
    out->sampleRate = sampleRate_;
}

// ---------------------------------------------------------------------------
// Real-time side

void Engine::emit(RoundsEventType type, int64_t value) {
    events_.push(RoundsEvent{type, value, now_});  // dropped if the UI isn't reading
}

void Engine::apply(int32_t cmd) {
    switch (cmd) {
        case ROUNDS_CMD_TAP: looper_.tap(); break;
        case ROUNDS_CMD_UNDO: looper_.undo(); break;
        case ROUNDS_CMD_STOP: looper_.stop(); break;
        case ROUNDS_CMD_CLEAR: looper_.clear(); break;
        case ROUNDS_CMD_UNDO_REVERTING_TAP: looper_.undoRevertingTap(); break;
        case ROUNDS_CMD_STOP_REVERTING_TAP: looper_.stopRevertingTap(); break;
        default: break;
    }
    publish();
}

void Engine::publish() {
    const int state = static_cast<int>(looper_.state());
    // Looper::State and RoundsLoopState share their order.
    static_assert(static_cast<int>(Looper::State::Stopped) == ROUNDS_LOOP_STOPPED, "state order");
    if (state != lastState_) {
        lastState_ = state;
        emit(ROUNDS_EVENT_STATE_CHANGED, state);
    }
    if (looper_.loopLength() != lastLength_) {
        lastLength_ = looper_.loopLength();
        emit(ROUNDS_EVENT_LOOP_LENGTH, lastLength_);
    }
    if (looper_.closedLayers() != lastLayers_) {
        lastLayers_ = looper_.closedLayers();
        emit(ROUNDS_EVENT_LAYER_COUNT, lastLayers_);
    }
}

void Engine::processChunk(const float* input, float* out, int frames) {
    if (frames <= 0) return;
    if (probe_.active()) {
        // While calibrating the looper hears silence and stays quiet.
        looper_.process(zeros_.data(), out, frames, 0.0f);
        for (int i = 0; i < frames; i++) out[i] = probe_.process(input[i]);
        if (probe_.takeFinished()) {
            const int64_t roundTrip = probe_.result();
            if (roundTrip >= 0) {
                // Keep the device's input/output split, scaled to the measurement.
                const int in = inputLatency_.load();
                const int outLat = outputLatency_.load();
                const int64_t total = in + outLat;
                const int newIn = total > 0 ? static_cast<int>(roundTrip * in / total) : static_cast<int>(roundTrip / 2);
                inputLatency_.store(newIn);
                outputLatency_.store(static_cast<int>(roundTrip) - newIn);
            }
            emit(ROUNDS_EVENT_CALIBRATION_DONE, roundTrip);
        }
        return;
    }
    looper_.process(input, out, frames, loopGain_.load(std::memory_order_relaxed));
    const float monitor = monitorGain_.load(std::memory_order_relaxed);
    if (monitor > 0.0f) {
        for (int i = 0; i < frames; i++) out[i] += input[i] * monitor;
    }
}

void Engine::processTimed(const float* input, float* outL, float* outR, int frames, uint64_t hostTime,
                          double hostTicksPerSample) {
    const uint32_t seq = mapSeq_.load(std::memory_order_relaxed);
    mapSeq_.store(seq + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    mapHost_.store(hostTime, std::memory_order_relaxed);
    mapSample_.store(now_, std::memory_order_relaxed);
    mapTicksPerSample_.store(hostTicksPerSample, std::memory_order_relaxed);
    mapSeq_.store(seq + 2, std::memory_order_release);
    process(input, outL, outR, frames);
}

void Engine::process(const float* input, float* outL, float* outR, int frames) {
    frames = std::min(frames, maxBlock_);
    statBlock_.store(frames, std::memory_order_relaxed);

    const int in = inputLatency_.load(std::memory_order_relaxed);
    const int out = outputLatency_.load(std::memory_order_relaxed);
    if (in != appliedInputLatency_ || out != appliedOutputLatency_) {
        looper_.setLatency(in, out);
        appliedInputLatency_ = in;
        appliedOutputLatency_ = out;
    }
    if (calibrateRequested_.exchange(false, std::memory_order_relaxed) && !probe_.active()) {
        probe_.start();
    }

    // Move queued commands into the time-ordered pending list.
    Command incoming;
    while (pendingCount_ < 64 && commands_.pop(incoming)) {
        if (incoming.time < 0) incoming.time = now_;
        int i = pendingCount_++;
        while (i > 0 && pending_[i - 1].time > incoming.time) {
            pending_[i] = pending_[i - 1];
            i--;
        }
        pending_[i] = incoming;
    }

    std::memset(outL, 0, sizeof(float) * static_cast<size_t>(frames));

    // Render in chunks split exactly at command times.
    int pos = 0;
    while (pos < frames) {
        while (pendingCount_ > 0 && pending_[0].time <= now_ + pos) {
            const int32_t cmd = pending_[0].cmd;
            for (int i = 1; i < pendingCount_; i++) pending_[i - 1] = pending_[i];
            pendingCount_--;
            apply(cmd);
        }
        int chunk = frames - pos;
        if (pendingCount_ > 0) chunk = static_cast<int>(std::min<int64_t>(chunk, pending_[0].time - (now_ + pos)));
        processChunk(input + pos, outL + pos, chunk);
        pos += chunk;
    }

    float inPeak = 0.0f;
    float outPeak = 0.0f;
    for (int i = 0; i < frames; i++) {
        inPeak = std::max(inPeak, std::fabs(input[i]));
        outPeak = std::max(outPeak, std::fabs(outL[i]));
    }
    if (outR) std::memcpy(outR, outL, sizeof(float) * static_cast<size_t>(frames));

    now_ += frames;
    publish();
    storeMax(statInputPeak_, inPeak);
    storeMax(statOutputPeak_, outPeak);
    statState_.store(static_cast<int>(looper_.state()), std::memory_order_relaxed);
    statLength_.store(looper_.loopLength(), std::memory_order_relaxed);
    statRecorded_.store(looper_.recordedLength(), std::memory_order_relaxed);
    statPosition_.store(looper_.heardPosition(), std::memory_order_relaxed);
    statLayers_.store(looper_.closedLayers(), std::memory_order_relaxed);
    statLayerOpen_.store(looper_.layerOpen() ? 1 : 0, std::memory_order_relaxed);
    statCalibrating_.store(probe_.active() ? 1 : 0, std::memory_order_relaxed);
    statNow_.store(now_, std::memory_order_relaxed);
}

}  // namespace rounds
