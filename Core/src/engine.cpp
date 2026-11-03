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
    config.notePreroll = static_cast<int64_t>(sampleRate * 0.1);
    return config;
}

void storeMax(std::atomic<float>& target, float value) {
    float current = target.load(std::memory_order_relaxed);
    while (value > current && !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
}

bool validLoop(int loop) { return loop >= 0 && loop < Engine::kLoops; }

int64_t wrap(int64_t value, int64_t length) {
    const int64_t r = value % length;
    return r < 0 ? r + length : r;
}
}  // namespace

int Engine::chooseBars(int64_t baseLength, double sampleRate) {
    // Beats per minute if the loop is `bars` bars of 4/4.
    for (int bars = 1; bars <= 8; bars *= 2) {
        const double bpm = 4.0 * bars * 60.0 * sampleRate / static_cast<double>(baseLength);
        if (bpm >= 80.0 || bars == 8) return bars;
    }
    return 8;
}

Engine::Engine(double sampleRate, double maxLoopSeconds, int maxBlockFrames)
    : sampleRate_(sampleRate), maxBlock_(maxBlockFrames), kits_(sampleRate), probe_(sampleRate) {
    for (int i = 0; i < kLoops; i++) {
        loops_.push_back(std::make_unique<Looper>(looperConfig(sampleRate, maxLoopSeconds)));
        loopGain_[static_cast<size_t>(i)].store(1.0f);
    }
    for (int i = 0; i < kLoops + 2; i++) players_.push_back(std::make_unique<Player>(sampleRate, &kits_));
    players_[kPadsPlayer]->setInstrument(ROUNDS_INSTRUMENT_DRY_KIT);
    transferNotes_.resize(static_cast<size_t>(loops_[0]->maxNotes()));
    zeros_.assign(static_cast<size_t>(maxBlockFrames), 0.0f);
    scratch_.assign(static_cast<size_t>(maxBlockFrames), 0.0f);
    transferBuffer_.assign(static_cast<size_t>(loops_[0]->maxLoopSamples()), 0.0f);
}

// ---------------------------------------------------------------------------
// Non-real-time side

bool Engine::push(const Command& command) {
    std::lock_guard<std::mutex> lock(producerMutex_);
    return commands_.push(command);
}

bool Engine::send(int loop, RoundsCommand cmd, int64_t sampleTime) {
    if (!validLoop(loop) && !(cmd == ROUNDS_CMD_START_STOP_ALL && loop == -1)) return false;
    return push(Command{loop, cmd, sampleTime});
}

bool Engine::sendAtHostTime(int loop, RoundsCommand cmd, uint64_t hostTime) {
    const int64_t at = hostToSample(hostTime);
    // One block of fixed delay: the moment has usually already been rendered,
    // and a constant delay feels tighter than a jittery one.
    return send(loop, cmd, at < 0 ? -1 : at + statBlock_.load(std::memory_order_relaxed));
}

bool Engine::note(int loop, int instrument, int note, int velocity, uint64_t hostTime) {
    if (loop < -1 || loop >= kLoops || instrument < 0 || instrument >= ROUNDS_INSTRUMENT_COUNT) return false;
    if (note < 0 || note > 127) return false;
    Command command{loop, kCmdNote, -1};
    const int64_t at = hostToSample(hostTime);
    if (at >= 0) {
        command.time = at + statBlock_.load(std::memory_order_relaxed);
        command.when = at;
    }
    command.note = static_cast<int16_t>(note);
    command.velocity = static_cast<int16_t>(std::clamp(velocity, 0, 127));
    command.instrument = static_cast<int16_t>(instrument);
    return push(command);
}

bool Engine::setInstrument(int loop, int instrument) {
    if (!validLoop(loop) || instrument < 0 || instrument >= ROUNDS_INSTRUMENT_COUNT) return false;
    Command command{loop, kCmdInstrument, -1};
    command.instrument = static_cast<int16_t>(instrument);
    return push(command);
}

// Engine sample time of a host time, or -1 before the audio has run.
int64_t Engine::hostToSample(uint64_t hostTime) {
    if (hostTime == 0) return -1;
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

    if (ticksPerSample <= 0.0) return -1;
    const double deltaTicks = static_cast<double>(hostTime) - static_cast<double>(mapHost);
    return mapSample + static_cast<int64_t>(std::llround(deltaTicks / ticksPerSample));
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
    out->bars = statBars_.load();
    out->barLength = statBarLength_.load();
    out->cycleLength = statCycle_.load();
    out->cyclePosition = statCyclePosition_.load();
    out->running = statRunning_.load();
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
    out->queued = s.queued.load();
    out->kind = s.kind.load();
    out->instrument = s.instrument.load();
    out->notes = s.notes.load();
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
    transferMidi_ = false;
    transfer_.store(kImportRequested, std::memory_order_release);
    return true;
}

bool Engine::beginImportNotes(int loop, const RoundsNote* notes, int count, int64_t length, int instrument) {
    if (!validLoop(loop) || length <= 1 || length > maxLoopSamples() || count < 0 || count > maxNotes()) return false;
    if (instrument < 0 || instrument >= ROUNDS_INSTRUMENT_COUNT) return false;
    if (transfer_.load(std::memory_order_acquire) != kIdle) return false;
    std::copy(notes, notes + count, transferNotes_.begin());
    transferNoteCount_ = count;
    transferLoop_ = loop;
    transferLength_ = length;
    transferInstrument_ = instrument;
    transferMidi_ = true;
    transfer_.store(kImportRequested, std::memory_order_release);
    return true;
}

int Engine::takeExportNotes(RoundsNote* dest, int capacity, int64_t* length, int* instrument) {
    if (transfer_.load(std::memory_order_acquire) != kExportDone) return -1;
    const int count = transferNoteCount_;
    if (!transferMidi_ || count > capacity) {
        transfer_.store(kIdle, std::memory_order_release);
        return -1;
    }
    std::copy(transferNotes_.begin(), transferNotes_.begin() + count, dest);
    *length = transferLength_;
    *instrument = transferInstrument_;
    transfer_.store(kIdle, std::memory_order_release);
    return count;
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
    if (transferMidi_ || length > capacity) {
        transfer_.store(kIdle, std::memory_order_release);
        return -1;
    }
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

bool Engine::running() const {
    for (const auto& looper : loops_) {
        if (looper->state() != Looper::State::Empty && looper->state() != Looper::State::Stopped) return true;
    }
    return false;
}

bool Engine::quantizing() const {
    return barLength_ > 0 && quantize_.load(std::memory_order_relaxed) && running();
}

// Moves every loop's phase, and the bar grid with them.
void Engine::shiftAll(int64_t delta) {
    for (auto& looper : loops_) looper->shiftOrigin(delta);
    gridOrigin_ += delta;
}

bool Engine::cancelScheduled(int loop) {
    Scheduled& s = scheduled_[static_cast<size_t>(loop)];
    if (s.action == kNone) return false;
    s.action = kNone;
    return true;
}

void Engine::apply(const Command& command) {
    const int loop = command.loop;
    const int32_t cmd = command.cmd;
    if (cmd == kCmdNote) {
        applyNote(command);
    } else if (cmd == kCmdInstrument) {
        players_[static_cast<size_t>(loop)]->setInstrument(command.instrument);
    } else if (cmd == ROUNDS_CMD_START_STOP_ALL) {
        startStopAll();
    } else {
        if (importing(loop)) return;  // the loop is being loaded from a session
        Looper& looper = *loops_[static_cast<size_t>(loop)];
        switch (cmd) {
            case ROUNDS_CMD_TAP: tap(loop); break;
            case ROUNDS_CMD_UNDO:
                // Undo takes back a queued action first.
                if (!cancelScheduled(loop)) looper.undo();
                break;
            case ROUNDS_CMD_STOP:
                if (scheduled_[static_cast<size_t>(loop)].action == kQueuedTap) {
                    cancelScheduled(loop);  // stopping a queued record/play just cancels it
                } else {
                    stop(loop);
                }
                break;
            case ROUNDS_CMD_CLEAR:
                cancelScheduled(loop);
                looper.clear();
                break;
            case ROUNDS_CMD_UNDO_REVERTING_TAP:
                // A queued tap never happened, so there is nothing to revert.
                if (scheduled_[static_cast<size_t>(loop)].action == kQueuedTap) cancelScheduled(loop);
                looper.undoRevertingTap();
                break;
            case ROUNDS_CMD_STOP_REVERTING_TAP:
                if (scheduled_[static_cast<size_t>(loop)].action == kQueuedTap) {
                    cancelScheduled(loop);
                } else {
                    looper.revertTap();
                }
                stop(loop);
                break;
            default: break;
        }
    }
    updateBase();
    releaseStaleNotes();
    publish();
}

void Engine::applyNote(const Command& command) {
    const int note = command.note;
    const int velocity = command.velocity;
    const int64_t when = command.when;
    Player& preview = *players_[isDrumKit(command.instrument) ? kPadsPlayer : kKeysPlayer];
    const bool slot = validLoop(command.loop) && !importing(command.loop);
    if (velocity == 0) {
        // Released wherever it may have been played: the loop can have
        // changed kind since the key went down.
        preview.noteOff(note, false);
        if (slot) {
            players_[static_cast<size_t>(command.loop)]->noteOff(note, false);
            loops_[static_cast<size_t>(command.loop)]->noteReleased(note, when);
        }
        return;
    }
    if (slot) {
        Looper& looper = *loops_[static_cast<size_t>(command.loop)];
        Player& player = *players_[static_cast<size_t>(command.loop)];
        const Looper::State state = looper.state();
        const bool midi = looper.kind() == Looper::Kind::Midi && state != Looper::State::Empty;
        const bool canBecomeMidi = state == Looper::State::Empty || (state == Looper::State::Recording && !midi);
        if (midi || canBecomeMidi) {
            if (!midi) player.setInstrument(command.instrument);
            player.noteOn(note, velocity, false);
            looper.notePressed(note, velocity, when);
            return;
        }
    }
    // An audio loop, or no loop: just play it.
    preview.setInstrument(command.instrument);
    preview.noteOn(note, velocity, false);
}

// A MIDI loop that stops or loses notes (undo, clear) lets go of what it was playing.
void Engine::releaseStaleNotes() {
    for (int i = 0; i < kLoops; i++) {
        const Looper& looper = *loops_[static_cast<size_t>(i)];
        const bool audible = looper.kind() == Looper::Kind::Midi &&
                             (looper.state() == Looper::State::Playing || looper.state() == Looper::State::Overdubbing);
        const int notes = looper.noteCount();
        if ((midiAudible_[static_cast<size_t>(i)] && !audible) || notes < midiNotes_[static_cast<size_t>(i)]) {
            players_[static_cast<size_t>(i)]->releaseLoopNotes();
        }
        midiAudible_[static_cast<size_t>(i)] = audible;
        midiNotes_[static_cast<size_t>(i)] = notes;
    }
}

void Engine::tap(int loop) {
    Looper& looper = *loops_[static_cast<size_t>(loop)];
    looper.setBarLength(barLength_);
    if (cancelScheduled(loop)) {
        looper.forgetTap();  // a second tap takes back a queued record, play or stop
        return;
    }
    const Looper::State state = looper.state();
    const bool starts = state == Looper::State::Empty || state == Looper::State::Stopped;
    if (!starts || barLength_ <= 0) {
        looper.tap();
        return;
    }
    const int64_t now = clock();
    if (!running()) {
        // Nothing playing: start now, and move the bar grid (and every loop's
        // phase with it) so this is the top of a bar, or of this loop.
        shiftAll(state == Looper::State::Stopped ? now - looper.origin() : wrap(now - gridOrigin_, barLength_));
        looper.tap();
        return;
    }
    if (!quantize_.load(std::memory_order_relaxed)) {
        looper.tap();
        return;
    }
    // A tap just after a bar line counts for that bar (players tend to land a
    // little late); otherwise it waits for the next one.
    const int64_t since = wrap(now - gridOrigin_, barLength_);
    const int64_t late = std::min<int64_t>(barLength_ / 8, static_cast<int64_t>(sampleRate_ / 4));
    if (since <= late) {
        looper.tapRecordingFrom(now - since);
    } else {
        scheduled_[static_cast<size_t>(loop)] = Scheduled{kQueuedTap, now + barLength_ - since};
        looper.forgetTap();
    }
}

void Engine::stop(int loop) {
    Looper& looper = *loops_[static_cast<size_t>(loop)];
    cancelScheduled(loop);
    const Looper::State state = looper.state();
    const bool audible = state == Looper::State::Playing || state == Looper::State::Overdubbing;
    if (!audible || !quantizing()) {
        looper.stop();
        return;
    }
    const int64_t now = clock();
    const int64_t since = wrap(now - gridOrigin_, barLength_);
    const int64_t late = std::min<int64_t>(barLength_ / 8, static_cast<int64_t>(sampleRate_ / 4));
    if (since <= late) {
        looper.stop();
    } else {
        scheduled_[static_cast<size_t>(loop)] = Scheduled{kQueuedStop, now + barLength_ - since};
    }
}

// Play button: stop everything (on the next bar), or start every stopped loop
// together from the top of the longest one.
void Engine::startStopAll() {
    if (running()) {
        for (int i = 0; i < kLoops; i++) {
            const Looper::State state = loops_[static_cast<size_t>(i)]->state();
            if (state == Looper::State::Empty || state == Looper::State::Stopped) {
                cancelScheduled(i);
            } else {
                stop(i);
            }
        }
        return;
    }
    int longest = -1;
    for (int i = 0; i < kLoops; i++) {
        cancelScheduled(i);
        const Looper& looper = *loops_[static_cast<size_t>(i)];
        if (looper.state() == Looper::State::Stopped &&
            (longest < 0 || looper.loopLength() > loops_[static_cast<size_t>(longest)]->loopLength())) {
            longest = i;
        }
    }
    if (longest < 0) return;
    shiftAll(clock() - loops_[static_cast<size_t>(longest)]->origin());
    for (auto& looper : loops_) {
        if (looper->state() == Looper::State::Stopped) looper->tap();
    }
}

void Engine::fireScheduled(int loop) {
    Scheduled& s = scheduled_[static_cast<size_t>(loop)];
    Looper& looper = *loops_[static_cast<size_t>(loop)];
    looper.setBarLength(barLength_);
    if (s.action == kQueuedTap) looper.tap();
    if (s.action == kQueuedStop) looper.stop();
    s.action = kNone;
    updateBase();
    releaseStaleNotes();
    publish();
}

// The first loop to get a length sets the tempo; it resets once every loop is empty.
void Engine::updateBase() {
    bool anyLoop = false;
    for (int i = 0; i < kLoops; i++) {
        const Looper& looper = *loops_[static_cast<size_t>(i)];
        if (looper.state() != Looper::State::Empty) anyLoop = true;
        if (baseLength_ == 0 && looper.loopLength() > 0) {
            baseLength_ = looper.loopLength();
            baseLoop_ = i;
            gridOrigin_ = looper.origin();
            applyBars(chooseBars(baseLength_, sampleRate_));
        }
    }
    if (!anyLoop && baseLength_ != 0) {
        baseLength_ = 0;
        baseLoop_ = -1;
        bars_ = 0;
        barLength_ = 0;
        for (auto& looper : loops_) looper->setBarLength(0);
    }
}

void Engine::applyBars(int bars) {
    if (baseLength_ <= 0 || bars < 1 || bars > 8 || (bars & (bars - 1)) != 0 || baseLength_ % bars != 0) return;
    bars_ = bars;
    barLength_ = baseLength_ / bars;
    for (auto& looper : loops_) looper->setBarLength(barLength_);
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
        const int queued = scheduled_[static_cast<size_t>(i)].action;
        if (queued != last.queued) {
            last.queued = queued;
            emit(ROUNDS_EVENT_QUEUED, i, queued);
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
        transferMidi_ = looper.kind() == Looper::Kind::Midi;
        if (transferMidi_) {
            // Notes are few: copy them all at once.
            transferNoteCount_ = looper.exportNotes(transferNotes_.data(), static_cast<int>(transferNotes_.size()), transferRotation_);
            transferInstrument_ = players_[static_cast<size_t>(transferLoop_)]->instrument();
            transfer_.store(kExportDone, std::memory_order_release);
            return;
        }
        transfer_.store(kExporting, std::memory_order_relaxed);
    } else if (state == kImportRequested) {
        if (transferMidi_) {
            looper.loadNotes(transferNotes_.data(), transferNoteCount_, transferLength_, 0);
            players_[static_cast<size_t>(transferLoop_)]->setInstrument(transferInstrument_);
            updateBase();
            releaseStaleNotes();
            publish();
            transfer_.store(kImportDone, std::memory_order_release);
            return;
        }
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

// A short blip on every beat while anything plays, higher on the downbeat.
// Timed by what is heard: a beat leaves outputLatency early so it sounds on
// the grid, like the loops do.
void Engine::renderClick(float* out, int frames, int64_t start) {
    const float gain = clickGain_.load(std::memory_order_relaxed);
    if (barLength_ <= 0) return;
    const bool on = gain > 0.0f && running();
    const int64_t latency = outputLatency_.load(std::memory_order_relaxed);
    for (int i = 0; i < frames; i++) {
        if (on) {
            const int64_t inBar = wrap(start + i + latency - gridOrigin_, barLength_);
            for (int beat = 0; beat < 4; beat++) {
                if (inBar != barLength_ * beat / 4) continue;
                const float freq = beat == 0 ? 1760.0f : 1320.0f;
                clickStep_ = freq / static_cast<float>(sampleRate_);
                clickPhase_ = 0.0f;
                clickLevel_ = beat == 0 ? 1.0f : 0.6f;
                clickDecay_ = std::exp(std::log(0.001f) / static_cast<float>(sampleRate_ * 0.03));
            }
        }
        if (clickLevel_ < 1e-4f) {
            clickLevel_ = 0.0f;
            continue;
        }
        out[i] += std::sin(6.2831853f * clickPhase_) * clickLevel_ * gain;
        clickPhase_ += clickStep_;
        if (clickPhase_ >= 1.0f) clickPhase_ -= 1.0f;
        clickLevel_ *= clickDecay_;
    }
}

void Engine::processChunk(const float* input, float* out, int frames, int64_t start) {
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
        Looper& looper = *loops_[static_cast<size_t>(l)];
        const float gain = loopGain_[static_cast<size_t>(l)].load(std::memory_order_relaxed);
        const int events = looper.collectNotes(frames, noteEvents_.data(), static_cast<int>(noteEvents_.size()));
        looper.process(input, scratch_.data(), frames, gain);
        players_[static_cast<size_t>(l)]->render(scratch_.data(), frames, gain, noteEvents_.data(), events);
        float peak = chunkPeak_[static_cast<size_t>(l)];
        for (int i = 0; i < frames; i++) {
            out[i] += scratch_[static_cast<size_t>(i)];
            peak = std::max(peak, std::fabs(scratch_[static_cast<size_t>(i)]));
        }
        chunkPeak_[static_cast<size_t>(l)] = peak;
    }
    players_[kKeysPlayer]->render(out, frames, 1.0f, nullptr, 0);
    players_[kPadsPlayer]->render(out, frames, 1.0f, nullptr, 0);
    renderClick(out, frames, start);
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
    const int bars = requestedBars_.exchange(0, std::memory_order_relaxed);
    if (bars > 0) applyBars(bars);

    // Move queued commands into the time-ordered pending list.
    Command incoming;
    while (pendingCount_ < 64 && commands_.pop(incoming)) {
        if (incoming.time < 0) incoming.time = now_;
        if (incoming.when < 0) incoming.when = incoming.time;
        int i = pendingCount_++;
        while (i > 0 && pending_[i - 1].time > incoming.time) {
            pending_[i] = pending_[i - 1];
            i--;
        }
        pending_[i] = incoming;
    }

    std::memset(outL, 0, sizeof(float) * static_cast<size_t>(frames));
    chunkPeak_.fill(0.0f);

    // Render in chunks split exactly at command times and queued bar lines.
    int pos = 0;
    while (pos < frames) {
        const int64_t at = now_ + pos;
        while (pendingCount_ > 0 && pending_[0].time <= at) {
            const Command command = pending_[0];
            for (int i = 1; i < pendingCount_; i++) pending_[i - 1] = pending_[i];
            pendingCount_--;
            apply(command);
        }
        for (int l = 0; l < kLoops; l++) {
            if (scheduled_[static_cast<size_t>(l)].action != kNone && scheduled_[static_cast<size_t>(l)].time <= at) {
                fireScheduled(l);
            }
        }
        int64_t chunk = frames - pos;
        if (pendingCount_ > 0) chunk = std::min<int64_t>(chunk, pending_[0].time - at);
        for (const Scheduled& s : scheduled_) {
            if (s.action != kNone) chunk = std::min<int64_t>(chunk, s.time - at);
        }
        processChunk(input + pos, outL + pos, static_cast<int>(chunk), at);
        pos += static_cast<int>(chunk);
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
    releaseStaleNotes();
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
        s.queued.store(scheduled_[static_cast<size_t>(l)].action, std::memory_order_relaxed);
        s.kind.store(looper.kind() == Looper::Kind::Midi ? ROUNDS_KIND_MIDI : ROUNDS_KIND_AUDIO, std::memory_order_relaxed);
        s.instrument.store(players_[static_cast<size_t>(l)]->instrument(), std::memory_order_relaxed);
        s.notes.store(looper.noteCount(), std::memory_order_relaxed);
    }
    int64_t cycle = 0;
    for (const auto& looper : loops_) cycle = std::max(cycle, looper->loopLength());
    statBars_.store(bars_, std::memory_order_relaxed);
    statBarLength_.store(barLength_, std::memory_order_relaxed);
    statCycle_.store(cycle, std::memory_order_relaxed);
    statCyclePosition_.store(cycle > 0 ? wrap(now_ + out - gridOrigin_, cycle) : 0, std::memory_order_relaxed);
    statRunning_.store(running() ? 1 : 0, std::memory_order_relaxed);
    statBase_.store(baseLength_, std::memory_order_relaxed);
    statBaseLoop_.store(baseLoop_, std::memory_order_relaxed);
    statCalibrating_.store(probe_.active() ? 1 : 0, std::memory_order_relaxed);
    statNow_.store(now_, std::memory_order_relaxed);
}

}  // namespace rounds
