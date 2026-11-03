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

bool validLoop(int loop) { return loop >= 0 && loop < Engine::kLoops; }
}  // namespace

Engine::Engine(double sampleRate, double maxLoopSeconds, int maxBlockFrames)
    : sampleRate_(sampleRate), maxBlock_(maxBlockFrames), probe_(sampleRate) {
    for (int i = 0; i < kLoops; i++) {
        loops_.push_back(std::make_unique<Looper>(looperConfig(sampleRate, maxLoopSeconds)));
        loopGain_[static_cast<size_t>(i)].store(1.0f);
    }
    zeros_.assign(static_cast<size_t>(maxBlockFrames), 0.0f);
    scratch_.assign(static_cast<size_t>(maxBlockFrames), 0.0f);
    transferBuffer_.assign(static_cast<size_t>(loops_[0]->maxLoopSamples()), 0.0f);
}

// ---------------------------------------------------------------------------
// Non-real-time side

bool Engine::send(int loop, RoundsCommand cmd, int64_t sampleTime) {
    if (!validLoop(loop)) return false;
    std::lock_guard<std::mutex> lock(producerMutex_);
    return commands_.push(Command{loop, cmd, sampleTime});
}

bool Engine::sendAtHostTime(int loop, RoundsCommand cmd, uint64_t hostTime) {
    if (hostTime == 0) return send(loop, cmd, -1);
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

    if (ticksPerSample <= 0.0) return send(loop, cmd, -1);
    const double deltaTicks = static_cast<double>(hostTime) - static_cast<double>(mapHost);
    const int64_t sampleAt = mapSample + static_cast<int64_t>(std::llround(deltaTicks / ticksPerSample));
    // One block of fixed delay: the moment has usually already been rendered,
    // and a constant delay feels tighter than a jittery one.
    return send(loop, cmd, sampleAt + statBlock_.load(std::memory_order_relaxed));
}

void Engine::setLatency(int inputLatency, int outputLatency) {
    inputLatency_.store(inputLatency);
    outputLatency_.store(outputLatency);
}

void Engine::setLoopGain(int loop, float gain) {
    if (validLoop(loop)) loopGain_[static_cast<size_t>(loop)].store(std::max(0.0f, gain));
}

void Engine::getStatus(RoundsStatus* out) {
    out->baseLength = statBase_.load();
    out->baseLoop = statBaseLoop_.load();
    out->inputPeak = statInputPeak_.exchange(0.0f);
    out->outputPeak = statOutputPeak_.exchange(0.0f);
    out->inputLatency = inputLatency_.load();
    out->outputLatency = outputLatency_.load();
    out->calibrating = statCalibrating_.load();
    out->sampleTime = statNow_.load();
    out->sampleRate = sampleRate_;
}

void Engine::getLoopStatus(int loop, RoundsLoopStatus* out) {
    *out = RoundsLoopStatus{};
    if (!validLoop(loop)) return;
    LoopStats& s = loopStats_[static_cast<size_t>(loop)];
    out->state = s.state.load();
    out->loopLength = s.length.load();
    out->recordedLength = s.recorded.load();
    out->playPosition = s.position.load();
    out->layers = s.layers.load();
    out->layerOpen = s.layerOpen.load();
    out->peak = s.peak.exchange(0.0f);
    out->gain = loopGain_[static_cast<size_t>(loop)].load();
}

// ---------------------------------------------------------------------------
// Session transfer (non-real-time side)

bool Engine::beginExport(int loop) {
    if (!validLoop(loop) || transfer_.load(std::memory_order_acquire) != kIdle) return false;
    transferLoop_ = loop;
    transfer_.store(kExportRequested, std::memory_order_release);
    return true;
}

bool Engine::beginImport(int loop, const float* samples, int64_t length) {
    if (!validLoop(loop) || length <= 0 || length > maxLoopSamples()) return false;
    if (transfer_.load(std::memory_order_acquire) != kIdle) return false;
    std::memcpy(transferBuffer_.data(), samples, sizeof(float) * static_cast<size_t>(length));
    transferLoop_ = loop;
    transferLength_ = length;
    transfer_.store(kImportRequested, std::memory_order_release);
    return true;
}

// 0 = idle, 1 = in progress, 2 = done, -1 = failed. A finished import or a
// failure returns to idle here; a finished export in takeExport().
int Engine::transferStatus() {
    switch (transfer_.load(std::memory_order_acquire)) {
        case kIdle: return 0;
        case kExportDone: return 2;
        case kImportDone: transfer_.store(kIdle, std::memory_order_release); return 2;
        case kFailed: transfer_.store(kIdle, std::memory_order_release); return -1;
        default: return 1;
    }
}

int64_t Engine::takeExport(float* dest, int64_t capacity) {
    if (transfer_.load(std::memory_order_acquire) != kExportDone) return -1;
    const int64_t length = transferLength_;
    if (length > capacity) return -1;
    std::memcpy(dest, transferBuffer_.data(), sizeof(float) * static_cast<size_t>(length));
    transfer_.store(kIdle, std::memory_order_release);
    return length;
}

// ---------------------------------------------------------------------------
// Real-time side

void Engine::emit(RoundsEventType type, int loop, int64_t value) {
    events_.push(RoundsEvent{type, loop, value, now_});  // dropped if the UI isn't reading
}

bool Engine::anyAudible() const {
    for (const auto& looper : loops_) {
        if (looper->state() == Looper::State::Playing || looper->state() == Looper::State::Overdubbing) return true;
    }
    return false;
}

bool Engine::importing(int loop) const {
    const int state = transfer_.load(std::memory_order_relaxed);
    return (state == kImportRequested || state == kImporting) && loop == transferLoop_;
}

void Engine::apply(int loop, int32_t cmd) {
    if (importing(loop)) return;  // the loop is being loaded from a session
    Looper& looper = *loops_[static_cast<size_t>(loop)];
    looper.setBaseLength(baseLength_);
    switch (cmd) {
        case ROUNDS_CMD_TAP:
            if (looper.state() == Looper::State::Stopped && !anyAudible()) {
                // Nothing playing: start this loop from the top, and move every
                // loop's phase with it so they stay in step with each other.
                const int64_t delta = looper.clock() - looper.origin();
                for (auto& other : loops_) other->shiftOrigin(delta);
            }
            looper.tap();
            break;
        case ROUNDS_CMD_UNDO: looper.undo(); break;
        case ROUNDS_CMD_STOP: looper.stop(); break;
        case ROUNDS_CMD_CLEAR: looper.clear(); break;
        case ROUNDS_CMD_UNDO_REVERTING_TAP: looper.undoRevertingTap(); break;
        case ROUNDS_CMD_STOP_REVERTING_TAP: looper.stopRevertingTap(); break;
        default: break;
    }
    updateBase();
    publish();
}

// The first loop to get a length sets the base; it resets once every loop is empty.
void Engine::updateBase() {
    bool anyLoop = false;
    for (int i = 0; i < kLoops; i++) {
        const Looper& looper = *loops_[static_cast<size_t>(i)];
        if (looper.state() != Looper::State::Empty) anyLoop = true;
        if (baseLength_ == 0 && looper.loopLength() > 0) {
            baseLength_ = looper.loopLength();
            baseLoop_ = i;
        }
    }
    if (!anyLoop) {
        baseLength_ = 0;
        baseLoop_ = -1;
    }
}

void Engine::publish() {
    // Looper::State and RoundsLoopState share their order.
    static_assert(static_cast<int>(Looper::State::Stopped) == ROUNDS_LOOP_STOPPED, "state order");
    for (int i = 0; i < kLoops; i++) {
        const Looper& looper = *loops_[static_cast<size_t>(i)];
        Published& last = published_[static_cast<size_t>(i)];
        const int state = static_cast<int>(looper.state());
        if (state != last.state) {
            last.state = state;
            emit(ROUNDS_EVENT_STATE_CHANGED, i, state);
        }
        if (looper.loopLength() != last.length) {
            last.length = looper.loopLength();
            emit(ROUNDS_EVENT_LOOP_LENGTH, i, last.length);
        }
        if (looper.closedLayers() != last.layers) {
            last.layers = looper.closedLayers();
            emit(ROUNDS_EVENT_LAYER_COUNT, i, last.layers);
        }
    }
}

// Copies one slice of the current save or load. Runs after the block is
// rendered, so a loaded loop never plays half-written.
void Engine::stepTransfer() {
    const int state = transfer_.load(std::memory_order_acquire);
    if (state == kIdle || state == kExportDone || state == kImportDone || state == kFailed) return;
    Looper& looper = *loops_[static_cast<size_t>(transferLoop_)];

    if (state == kExportRequested) {
        const auto s = looper.state();
        if (looper.loopLength() <= 0 || s == Looper::State::Recording) {
            transfer_.store(kFailed, std::memory_order_release);
            return;
        }
        transferLength_ = looper.loopLength();
        transferPos_ = 0;
        // Saved sample k is what played at real time ref + k, with ref the
        // base loop's top, so every saved loop lines up with the others.
        const int64_t ref = baseLoop_ >= 0 ? loops_[static_cast<size_t>(baseLoop_)]->origin() : looper.origin();
        transferRotation_ = ((ref - looper.origin()) % transferLength_ + transferLength_) % transferLength_;
        transfer_.store(kExporting, std::memory_order_relaxed);
    } else if (state == kImportRequested) {
        transferPos_ = 0;
        transfer_.store(kImporting, std::memory_order_relaxed);
    }

    const int64_t end = std::min(transferLength_, transferPos_ + kTransferSlice);
    if (transfer_.load(std::memory_order_relaxed) == kExporting) {
        // Re-recorded or cleared mid-save: the copy would mix two takes.
        if (looper.loopLength() != transferLength_ || looper.state() == Looper::State::Recording) {
            transfer_.store(kFailed, std::memory_order_release);
            return;
        }
        for (int64_t k = transferPos_; k < end; k++) {
            transferBuffer_[static_cast<size_t>(k)] = looper.sampleAt((k + transferRotation_) % transferLength_);
        }
        transferPos_ = end;
        if (transferPos_ >= transferLength_) transfer_.store(kExportDone, std::memory_order_release);
    } else {
        looper.loadSlice(transferBuffer_.data() + transferPos_, transferPos_, end - transferPos_);
        transferPos_ = end;
        if (transferPos_ >= transferLength_) {
            // Every loaded loop shares origin 0, so a session's loops come
            // back in the phase they were saved in.
            looper.finishLoad(transferLength_, 0);
            updateBase();
            publish();
            transfer_.store(kImportDone, std::memory_order_release);
        }
    }
}

void Engine::processChunk(const float* input, float* out, int frames) {
    if (frames <= 0) return;
    if (probe_.active()) {
        // While calibrating the loopers hear silence and stay quiet.
        for (auto& looper : loops_) looper->process(zeros_.data(), out, frames, 0.0f);
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
            emit(ROUNDS_EVENT_CALIBRATION_DONE, -1, roundTrip);
        }
        return;
    }
    for (int l = 0; l < kLoops; l++) {
        std::memset(scratch_.data(), 0, sizeof(float) * static_cast<size_t>(frames));
        loops_[static_cast<size_t>(l)]->process(input, scratch_.data(), frames,
                                                 loopGain_[static_cast<size_t>(l)].load(std::memory_order_relaxed));
        float peak = chunkPeak_[static_cast<size_t>(l)];
        for (int i = 0; i < frames; i++) {
            out[i] += scratch_[static_cast<size_t>(i)];
            peak = std::max(peak, std::fabs(scratch_[static_cast<size_t>(i)]));
        }
        chunkPeak_[static_cast<size_t>(l)] = peak;
    }
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
        for (auto& looper : loops_) looper->setLatency(in, out);
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
    chunkPeak_.fill(0.0f);

    // Render in chunks split exactly at command times.
    int pos = 0;
    while (pos < frames) {
        while (pendingCount_ > 0 && pending_[0].time <= now_ + pos) {
            const Command command = pending_[0];
            for (int i = 1; i < pendingCount_; i++) pending_[i - 1] = pending_[i];
            pendingCount_--;
            apply(command.loop, command.cmd);
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
    stepTransfer();
    updateBase();  // a loop can close itself when it runs out of room
    publish();
    storeMax(statInputPeak_, inPeak);
    storeMax(statOutputPeak_, outPeak);
    for (int l = 0; l < kLoops; l++) {
        const Looper& looper = *loops_[static_cast<size_t>(l)];
        LoopStats& s = loopStats_[static_cast<size_t>(l)];
        storeMax(s.peak, chunkPeak_[static_cast<size_t>(l)]);
        s.state.store(static_cast<int>(looper.state()), std::memory_order_relaxed);
        s.length.store(looper.loopLength(), std::memory_order_relaxed);
        s.recorded.store(looper.recordedLength(), std::memory_order_relaxed);
        s.position.store(looper.heardPosition(), std::memory_order_relaxed);
        s.layers.store(looper.closedLayers(), std::memory_order_relaxed);
        s.layerOpen.store(looper.layerOpen() ? 1 : 0, std::memory_order_relaxed);
    }
    statBase_.store(baseLength_, std::memory_order_relaxed);
    statBaseLoop_.store(baseLoop_, std::memory_order_relaxed);
    statCalibrating_.store(probe_.active() ? 1 : 0, std::memory_order_relaxed);
    statNow_.store(now_, std::memory_order_relaxed);
}

}  // namespace rounds
