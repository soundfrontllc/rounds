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
bool validID(int id) { return id >= 0 && id < Engine::kPool; }
bool validPart(int part) { return part >= 0 && part < Engine::kParts; }

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
    const Looper::Config config = looperConfig(sampleRate, maxLoopSeconds);
    layerPool_ = std::make_unique<LayerPool>(kLayers, config.maxLoopSamples);
    for (int i = 0; i < kPool; i++) {
        loops_.push_back(std::make_unique<Looper>(config, layerPool_.get()));
        loopGain_[static_cast<size_t>(i)].store(1.0f);
        loopMute_[static_cast<size_t>(i)].store(false);
        loopPan_[static_cast<size_t>(i)].store(0.0f);
    }
    for (int i = 0; i < kPool + 2; i++) players_.push_back(std::make_unique<Player>(sampleRate, &kits_));
    resetParts();
    for (int i = 0; i < kParts * kLoops; i++) statPartLoops_[static_cast<size_t>(i)].store(i);
    for (auto& gain : songGain_) gain.store(1.0f);
    for (auto& pan : songPan_) pan.store(0.0f);
    for (auto& peak : stemPeak_) peak.store(0.0f);
    for (auto& stem : statLaneStem_) stem.store(-1);
    laneStem_.fill(-1);
    players_[kPadsPlayer]->setInstrument(ROUNDS_INSTRUMENT_DRY_KIT);
    transferNotes_.resize(static_cast<size_t>(loops_[0]->maxNotes()));
    zeros_.assign(static_cast<size_t>(maxBlockFrames), 0.0f);
    scratch_.assign(static_cast<size_t>(maxBlockFrames), 0.0f);
    loopsL_.assign(static_cast<size_t>(maxBlockFrames), 0.0f);
    loopsR_.assign(static_cast<size_t>(maxBlockFrames), 0.0f);
    transferBuffer_.assign(static_cast<size_t>(loops_[0]->maxLoopSamples()), 0.0f);
}

Engine::~Engine() {
    delete song_;
    delete incomingSong_.load();
    delete retiredSong_.load();
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

void Engine::setLoopGain(int loopID, float gain) {
    if (validID(loopID)) loopGain_[static_cast<size_t>(loopID)].store(std::max(0.0f, gain));
}

void Engine::setLoopMute(int loopID, bool muted) {
    if (validID(loopID)) loopMute_[static_cast<size_t>(loopID)].store(muted);
}

void Engine::setLoopPan(int loopID, float pan) {
    if (validID(loopID)) loopPan_[static_cast<size_t>(loopID)].store(std::clamp(pan, -1.0f, 1.0f));
}

// A copied loop keeps its volume, mute and pan.
void Engine::copyMix(int from, int to) {
    const size_t f = static_cast<size_t>(from), t = static_cast<size_t>(to);
    loopGain_[t].store(loopGain_[f].load(std::memory_order_relaxed), std::memory_order_relaxed);
    loopMute_[t].store(loopMute_[f].load(std::memory_order_relaxed), std::memory_order_relaxed);
    loopPan_[t].store(loopPan_[f].load(std::memory_order_relaxed), std::memory_order_relaxed);
}

bool Engine::loadSong(const float* const* stems, int count, int64_t length, int64_t barLength, int64_t downbeat) {
    if (count < 0 || count > kSongStems) return false;
    if (count > 0 && (!stems || length <= 0 || barLength <= 0 || downbeat < 0 || downbeat >= length)) return false;
    auto song = std::make_unique<Song>();
    if (count > 0) {
        song->count = count;
        song->length = length;
        song->barLength = barLength;
        song->downbeat = downbeat;
        for (int i = 0; i < count; i++) {
            if (!stems[i]) return false;
            song->stems[static_cast<size_t>(i)].assign(stems[i], stems[i] + length);
        }
    }
    std::lock_guard<std::mutex> lock(songMutex_);
    freeRetiredSong();
    // A song the audio thread hasn't taken yet is simply replaced.
    delete incomingSong_.exchange(song.release(), std::memory_order_acq_rel);
    return true;
}

void Engine::freeRetiredSong() { delete retiredSong_.exchange(nullptr, std::memory_order_acq_rel); }

void Engine::setSongPan(int stem, float pan) {
    if (stem >= 0 && stem < kSongStems) songPan_[static_cast<size_t>(stem)].store(std::clamp(pan, -1.0f, 1.0f));
}

void Engine::setSongGain(int stem, float gain) {
    if (stem >= 0 && stem < kSongStems) songGain_[static_cast<size_t>(stem)].store(std::max(0.0f, gain));
}

bool Engine::setSongLane(int slot, int stem) {
    if (!validLoop(slot) || stem < -1 || stem >= kSongStems) return false;
    Command command{slot, kCmdSongLane, -1};
    command.note = static_cast<int16_t>(stem);
    return push(command);
}

bool Engine::setSongBar(int bar) {
    if (bar < 1) return false;
    return push(Command{bar, kCmdSongBar, -1});
}

bool Engine::switchPart(int part) {
    if (!validPart(part)) return false;
    return push(Command{part, kCmdSwitchPart, -1});
}

bool Engine::newPart() { return push(Command{-1, kCmdNewPart, -1}); }

bool Engine::addEmptyPart(int part) {
    if (!validPart(part)) return false;
    return push(Command{part, kCmdEmptyPart, -1});
}

bool Engine::setShared(int loopID, bool shared) {
    if (!validID(loopID)) return false;
    Command command{loopID, kCmdShared, -1};
    command.velocity = shared ? 1 : 0;
    return push(command);
}

bool Engine::reset() { return push(Command{-1, kCmdReset, -1}); }

bool Engine::setPartLoop(int part, int slot, int loopID) {
    if (!validPart(part) || !validLoop(slot) || !validID(loopID)) return false;
    Command command{part, kCmdPartLoop, -1};
    command.note = static_cast<int16_t>(slot);
    command.instrument = static_cast<int16_t>(loopID);
    return push(command);
}

int Engine::partLoop(int part, int slot) const {
    if (!validPart(part) || !validLoop(slot)) return -1;
    return statPartLoops_[static_cast<size_t>(part * kLoops + slot)].load(std::memory_order_relaxed);
}

void Engine::getPartStatus(int part, RoundsPartStatus* out) {
    *out = RoundsPartStatus{};
    if (!validPart(part)) return;
    const PartStats& p = partStats_[static_cast<size_t>(part)];
    out->exists = p.exists.load();
    out->loops = p.loops.load();
    out->cycleLength = p.cycle.load();
}

void Engine::getStatus(RoundsStatus* out) {
    out->baseLength = statBase_.load();
    out->baseLoop = statBaseLoop_.load();
    out->baseLoopID = statBaseID_.load();
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
    out->part = statPart_.load();
    out->nextPart = statNextPart_.load();
    out->partSwitchIn = statSwitchIn_.load();
    out->songLoaded = statSongLoaded_.load();
    out->songPlaying = statSongPlaying_.load();
    out->songLength = statSongLength_.load();
    out->songPosition = statSongPosition_.load();
    out->songBar = statSongBar_.load();
    std::lock_guard<std::mutex> lock(songMutex_);
    freeRetiredSong();
}

void Engine::getLoopStatus(int slot, RoundsLoopStatus* out) {
    *out = RoundsLoopStatus{};
    if (!validLoop(slot)) return;
    getPoolLoopStatus(partLoop(statPart_.load(std::memory_order_relaxed), slot), out);
    const int stem = statLaneStem_[static_cast<size_t>(slot)].load(std::memory_order_relaxed);
    if (stem >= 0) {
        out->songStem = stem;
        out->peak = stemPeak_[static_cast<size_t>(stem)].exchange(0.0f);
    }
}

void Engine::getPoolLoopStatus(int loop, RoundsLoopStatus* out) {
    *out = RoundsLoopStatus{};
    if (!validID(loop)) return;
    LoopStats& s = loopStats_[static_cast<size_t>(loop)];
    out->state = s.state.load();
    out->loopLength = s.length.load();
    out->recordedLength = s.recorded.load();
    out->playPosition = s.position.load();
    out->layers = s.layers.load();
    out->layerOpen = s.layerOpen.load();
    out->peak = s.peak.exchange(0.0f);
    out->gain = loopGain_[static_cast<size_t>(loop)].load();
    out->muted = loopMute_[static_cast<size_t>(loop)].load() ? 1 : 0;
    out->pan = loopPan_[static_cast<size_t>(loop)].load();
    out->queued = s.queued.load();
    out->kind = s.kind.load();
    out->instrument = s.instrument.load();
    out->notes = s.notes.load();
    out->loopID = loop;
    out->shared = s.shared.load();
    out->songStem = -1;
}

// ---------------------------------------------------------------------------
// Session transfer (non-real-time side)

bool Engine::beginExport(int loop) {
    if (!validID(loop) || transfer_.load(std::memory_order_acquire) != kIdle) return false;
    transferLoop_ = loop;
    transfer_.store(kExportRequested, std::memory_order_release);
    return true;
}

bool Engine::beginImport(int loop, const float* samples, int64_t length) {
    if (!validID(loop) || length <= 0 || length > maxLoopSamples()) return false;
    if (transfer_.load(std::memory_order_acquire) != kIdle) return false;
    std::memcpy(transferBuffer_.data(), samples, sizeof(float) * static_cast<size_t>(length));
    transferLoop_ = loop;
    transferLength_ = length;
    transferMidi_ = false;
    transfer_.store(kImportRequested, std::memory_order_release);
    return true;
}

bool Engine::beginImportNotes(int loop, const RoundsNote* notes, int count, int64_t length, int instrument) {
    if (!validID(loop) || length <= 1 || length > maxLoopSamples() || count < 0 || count > maxNotes()) return false;
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

bool Engine::importing(int loop) const {
    const int state = transfer_.load(std::memory_order_relaxed);
    return (state == kImportRequested || state == kImporting) && loop == transferLoop_;
}

// Anything recording or playing in the playing part, or the song playing.
bool Engine::running() const {
    if (songPlaying_) return true;
    for (int slot = 0; slot < kLoops; slot++) {
        const Looper::State state = loops_[static_cast<size_t>(slotLoop(slot))]->state();
        if (state != Looper::State::Empty && state != Looper::State::Stopped) return true;
    }
    return false;
}

bool Engine::inPart(int part, int id) const {
    for (int slot : partLoops_[static_cast<size_t>(part)]) {
        if (slot == id) return true;
    }
    return false;
}

// Heard right now: in the playing part, and playing or overdubbing.
bool Engine::audible(int id) const {
    const Looper::State state = loops_[static_cast<size_t>(id)]->state();
    return (state == Looper::State::Playing || state == Looper::State::Overdubbing) && inPart(part_, id);
}

int Engine::longestInPart() const {
    int longest = -1;
    for (int slot = 0; slot < kLoops; slot++) {
        const int id = slotLoop(slot);
        if (loops_[static_cast<size_t>(id)]->loopLength() <= 0) continue;
        if (longest < 0 || loops_[static_cast<size_t>(id)]->loopLength() > loops_[static_cast<size_t>(longest)]->loopLength()) {
            longest = id;
        }
    }
    return longest;
}

bool Engine::quantizing() const {
    return barLength_ > 0 && quantize_.load(std::memory_order_relaxed) && running();
}

// ---------------------------------------------------------------------------
// Parts

void Engine::resetParts() {
    for (int p = 0; p < kParts; p++) {
        for (int slot = 0; slot < kLoops; slot++) partLoops_[static_cast<size_t>(p)][static_cast<size_t>(slot)] = p * kLoops + slot;
        partExists_[static_cast<size_t>(p)] = p == 0;
    }
    part_ = 0;
    nextPart_ = -1;
    for (Copy& c : copies_) c = Copy{};
}

// A loop for a new part's slot that no part uses: its own, unless a loaded
// session gave that one to another part.
int Engine::freeLoopFor(int part, int slot) const {
    auto used = [this](int id) {
        for (int p = 0; p < kParts; p++) {
            if (partExists_[static_cast<size_t>(p)] && inPart(p, id)) return true;
        }
        return false;
    };
    const int own = part * kLoops + slot;
    if (!used(own)) return own;
    for (int id = 0; id < kPool; id++) {
        if (!used(id)) return id;
    }
    return -1;
}

void Engine::createPart() {
    for (const Copy& c : copies_) {
        if (c.to >= 0) return;  // the last new part is still being copied
    }
    int part = -1;
    for (int p = 0; p < kParts && part < 0; p++) {
        if (!partExists_[static_cast<size_t>(p)]) part = p;
    }
    if (part < 0) {
        emit(ROUNDS_EVENT_PART_CREATED, -1, -1);
        return;
    }
    auto& slots = partLoops_[static_cast<size_t>(part)];
    slots.fill(-1);
    partExists_[static_cast<size_t>(part)] = true;
    for (int slot = 0; slot < kLoops; slot++) {
        const int from = slotLoop(slot);
        if (shared_[static_cast<size_t>(from)]) {
            slots[static_cast<size_t>(slot)] = from;
            continue;
        }
        // 64 loops for 8 parts of 8: there is always one.
        const int to = freeLoopFor(part, slot);
        slots[static_cast<size_t>(slot)] = to;
        Looper& dest = *loops_[static_cast<size_t>(to)];
        cancelScheduled(to);
        dest.clear();
        players_[static_cast<size_t>(to)]->setInstrument(players_[static_cast<size_t>(from)]->instrument());
        copyMix(from, to);
        shared_[static_cast<size_t>(to)] = false;
        const Looper& source = *loops_[static_cast<size_t>(from)];
        const Looper::State state = source.state();
        if (state == Looper::State::Empty || state == Looper::State::Recording) continue;
        if (source.kind() == Looper::Kind::Midi) {
            dest.finishCopy(source);
        } else {
            copies_[static_cast<size_t>(slot)] = Copy{from, to, source.loopLength(), 0};
        }
    }
    emit(ROUNDS_EVENT_PART_CREATED, -1, part);
    requestSwitch(part);
}

void Engine::createEmptyPart(int part) {
    if (partExists_[static_cast<size_t>(part)]) return;
    auto& slots = partLoops_[static_cast<size_t>(part)];
    slots.fill(-1);
    partExists_[static_cast<size_t>(part)] = true;
    for (int slot = 0; slot < kLoops; slot++) {
        const int id = freeLoopFor(part, slot);
        slots[static_cast<size_t>(slot)] = id;
        cancelScheduled(id);
        loops_[static_cast<size_t>(id)]->clear();
        loopGain_[static_cast<size_t>(id)].store(1.0f, std::memory_order_relaxed);
        loopMute_[static_cast<size_t>(id)].store(false, std::memory_order_relaxed);
        loopPan_[static_cast<size_t>(id)].store(0.0f, std::memory_order_relaxed);
        shared_[static_cast<size_t>(id)] = false;
    }
    emit(ROUNDS_EVENT_PART_CREATED, -1, part);
}

// Copies a slice of the new part's loops after each block.
void Engine::stepCopies() {
    int64_t budget = kCopySlice;
    for (Copy& c : copies_) {
        if (c.to < 0 || budget <= 0) continue;
        const Looper& source = *loops_[static_cast<size_t>(c.from)];
        Looper& dest = *loops_[static_cast<size_t>(c.to)];
        if (source.loopLength() != c.length || source.state() == Looper::State::Recording) {
            dest.clear();  // re-recorded or cleared mid-copy: leave the slot empty
            c = Copy{};
            continue;
        }
        const int64_t count = std::min(budget, c.length - c.pos);
        dest.copySlice(source, c.pos, count);
        c.pos += count;
        budget -= count;
        if (c.pos >= c.length) {
            dest.finishCopy(source);
            c = Copy{};
        }
    }
}

void Engine::requestSwitch(int part) {
    if (!validPart(part) || !partExists_[static_cast<size_t>(part)]) return;
    if (part == part_ || part == nextPart_) {
        // The playing part, or the one already waiting: take the switch back.
        if (nextPart_ >= 0) emit(ROUNDS_EVENT_PART_QUEUED, -1, -1);
        nextPart_ = -1;
        return;
    }
    nextPart_ = part;
    scheduleSwitch();
    if (!switchNow_) emit(ROUNDS_EVENT_PART_QUEUED, -1, part);
}

// The switch is heard where the longest loop of the playing part comes round.
void Engine::scheduleSwitch() {
    const int64_t now = clock();
    const int longest = longestInPart();
    switchNow_ = !running() || longest < 0;
    if (switchNow_) {
        switchAt_ = now;
        switchOrigin_ = now;
        return;
    }
    const Looper& looper = *loops_[static_cast<size_t>(longest)];
    const int64_t cycle = looper.loopLength();
    const int64_t heard = now + outputLatency_.load(std::memory_order_relaxed);
    const int64_t into = wrap(heard - looper.origin(), cycle);
    switchOrigin_ = into == 0 ? heard : heard + cycle - into;
    switchAt_ = switchOrigin_ - (heard - now);
}

void Engine::fireSwitch() {
    for (const Copy& c : copies_) {
        if (c.to < 0) continue;
        // Still copying (a long loop, made just before the cycle ended): try
        // again at the end of the next cycle, or the next block if stopped.
        if (!switchNow_) scheduleSwitch();
        return;
    }
    const int from = part_;
    const int to = nextPart_;
    for (int id : partLoops_[static_cast<size_t>(from)]) {
        if (inPart(to, id)) continue;
        Looper& looper = *loops_[static_cast<size_t>(id)];
        cancelScheduled(id);
        // Leaving the part closes a recording or overdub, keeping what was played.
        if (looper.state() == Looper::State::Recording || looper.state() == Looper::State::Overdubbing) {
            looper.setBarLength(barLength_);
            looper.tap();
            if (looper.state() == Looper::State::Recording) looper.clear();  // too short to keep
        }
    }
    for (int id : partLoops_[static_cast<size_t>(to)]) {
        if (inPart(from, id)) continue;
        Looper& looper = *loops_[static_cast<size_t>(id)];
        if (switchNow_) {
            if (looper.state() == Looper::State::Playing) looper.stop();
        } else {
            looper.shiftOrigin(switchOrigin_ - looper.origin());  // from the top
        }
    }
    part_ = to;
    nextPart_ = -1;
    resyncPublished();
    emit(ROUNDS_EVENT_PART_CHANGED, -1, to);
    updateBase();
    releaseStaleNotes();
    publish();
}

// A part's slots are reported as they are, without an event per slot.
void Engine::resyncPublished() {
    for (int slot = 0; slot < kLoops; slot++) {
        const int id = slotLoop(slot);
        const Looper& looper = *loops_[static_cast<size_t>(id)];
        Published& last = published_[static_cast<size_t>(slot)];
        last.state = static_cast<int>(looper.state());
        last.length = looper.loopLength();
        last.layers = looper.closedLayers();
        last.queued = scheduled_[static_cast<size_t>(id)].action;
    }
}

// Keeps a few shared layers free by merging the oldest one anywhere into its
// loop's base (a slice per block), so an overdub never finds the pool empty.
void Engine::keepLayersFree() {
    if (layerPool_->freeCount() >= 3) return;
    int oldest = -1;
    uint64_t age = 0;
    for (int id = 0; id < kPool; id++) {
        const Looper& looper = *loops_[static_cast<size_t>(id)];
        if (looper.merging()) return;  // one at a time
        const uint64_t a = looper.oldestLayerAge();
        if (a != 0 && (oldest < 0 || a < age)) {
            oldest = id;
            age = a;
        }
    }
    if (oldest >= 0) loops_[static_cast<size_t>(oldest)]->mergeOldestLayer();
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
    const int32_t cmd = command.cmd;
    // Slot commands act on the loop in that slot of the playing part.
    const int loop = validLoop(command.loop) ? slotLoop(command.loop) : -1;
    if (cmd == kCmdNote) {
        // Notes on a stem lane are played, not recorded.
        const bool stemLane = validLoop(command.loop) && laneStem_[static_cast<size_t>(command.loop)] >= 0;
        applyNote(command, stemLane ? -1 : loop);
    } else if (cmd == kCmdInstrument) {
        players_[static_cast<size_t>(loop)]->setInstrument(command.instrument);
    } else if (cmd == ROUNDS_CMD_START_STOP_ALL) {
        startStopAll();
    } else if (cmd == kCmdSwitchPart) {
        requestSwitch(command.loop);
    } else if (cmd == kCmdNewPart) {
        createPart();
    } else if (cmd == kCmdEmptyPart) {
        createEmptyPart(command.loop);
    } else if (cmd == kCmdSongBar) {
        if (song_ && song_->length > 0) {
            const int last = std::max(1, songBarAt(song_->length - 1));
            const int bar = std::min(command.loop, last);
            if (songPlaying_) {
                // Jump on the coming bar line, so the grid carries on.
                const int64_t now = clock();
                const int64_t since = wrap(now - gridOrigin_, barLength_);
                songJumpAt_ = since == 0 ? now : now + barLength_ - since;
                songJumpBar_ = bar;
            } else {
                songCue_ = bar;
            }
        }
    } else if (cmd == kCmdSongLane) {
        const bool usable = song_ && song_->length > 0 && command.note < song_->count;
        if (command.note < 0 || usable) {
            // A stem is in one lane at most.
            for (int& stem : laneStem_) {
                if (stem == command.note) stem = -1;
            }
            laneStem_[static_cast<size_t>(command.loop)] = command.note;
        }
    } else if (cmd == kCmdShared) {
        shared_[static_cast<size_t>(command.loop)] = command.velocity != 0;
    } else if (cmd == kCmdReset) {
        for (int id = 0; id < kPool; id++) {
            cancelScheduled(id);
            loops_[static_cast<size_t>(id)]->clear();
            shared_[static_cast<size_t>(id)] = false;
            loopMute_[static_cast<size_t>(id)].store(false, std::memory_order_relaxed);
            loopPan_[static_cast<size_t>(id)].store(0.0f, std::memory_order_relaxed);
        }
        resetParts();
        resyncPublished();
        emit(ROUNDS_EVENT_PART_CHANGED, -1, 0);
    } else if (cmd == kCmdPartLoop) {
        const size_t part = static_cast<size_t>(command.loop);
        if (!partExists_[part]) {
            // A part made by a session load starts as its own loops.
            for (int slot = 0; slot < kLoops; slot++) partLoops_[part][static_cast<size_t>(slot)] = command.loop * kLoops + slot;
            partExists_[part] = true;
        }
        partLoops_[part][static_cast<size_t>(command.note)] = command.instrument;
        if (static_cast<int>(part) == part_) resyncPublished();
    } else if (validLoop(command.loop) && laneStem_[static_cast<size_t>(command.loop)] >= 0) {
        // A stem lane only plays the song; clearing it takes the stem out.
        if (cmd == ROUNDS_CMD_CLEAR) laneStem_[static_cast<size_t>(command.loop)] = -1;
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
            case ROUNDS_CMD_HALVE:
            case ROUNDS_CMD_DOUBLE:
                // At least a bar (or the shortest loop before there is a tempo).
                looper.resize(cmd == ROUNDS_CMD_DOUBLE, barLength_ > 0 ? barLength_ : 4800);
                releaseStaleNotes();
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

// `id`: the loop in the note's slot, -1 for none.
void Engine::applyNote(const Command& command, int id) {
    const int note = command.note;
    const int velocity = command.velocity;
    const int64_t when = command.when;
    Player& preview = *players_[isDrumKit(command.instrument) ? kPadsPlayer : kKeysPlayer];
    const bool slot = id >= 0 && !importing(id);
    if (velocity == 0) {
        // Released wherever it may have been played: the loop can have
        // changed kind since the key went down.
        preview.noteOff(note, false);
        if (slot) {
            players_[static_cast<size_t>(id)]->noteOff(note, false);
            loops_[static_cast<size_t>(id)]->noteReleased(note, when);
        }
        return;
    }
    if (slot) {
        Looper& looper = *loops_[static_cast<size_t>(id)];
        Player& player = *players_[static_cast<size_t>(id)];
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
    for (int i = 0; i < kPool; i++) {
        const Looper& looper = *loops_[static_cast<size_t>(i)];
        const bool audible = looper.kind() == Looper::Kind::Midi && this->audible(i);
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
    const bool song = song_ && song_->length > 0;
    if (running()) {
        if (songPlaying_) {
            if (!quantizing()) {
                stopSong();
            } else {
                // On the bar line, like the loops (a press just after one counts for it).
                const int64_t now = clock();
                const int64_t since = wrap(now - gridOrigin_, barLength_);
                const int64_t late = std::min<int64_t>(barLength_ / 8, static_cast<int64_t>(sampleRate_ / 4));
                if (since <= late) {
                    stopSong();
                } else {
                    songStopAt_ = now + barLength_ - since;
                }
            }
        }
        for (int slot = 0; slot < kLoops; slot++) {
            const int i = slotLoop(slot);
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
    for (int slot = 0; slot < kLoops; slot++) {
        const int i = slotLoop(slot);
        cancelScheduled(i);
        const Looper& looper = *loops_[static_cast<size_t>(i)];
        if (looper.state() == Looper::State::Stopped &&
            (longest < 0 || looper.loopLength() > loops_[static_cast<size_t>(longest)]->loopLength())) {
            longest = i;
        }
    }
    // Everything starts from its top as heard: what is written now is heard
    // outputLatency later, so start there and no first beat is cut short.
    const int64_t top = clock() + outputLatency_.load(std::memory_order_relaxed);
    if (longest >= 0) {
        shiftAll(top - loops_[static_cast<size_t>(longest)]->origin());
        for (int slot = 0; slot < kLoops; slot++) {
            Looper& looper = *loops_[static_cast<size_t>(slotLoop(slot))];
            if (looper.state() == Looper::State::Stopped) looper.tap();
        }
    }
    if (song) {
        // The song from its start bar, and the bar grid with it.
        startSong(top);
        gridOrigin_ = top;
    }
}

// ---------------------------------------------------------------------------
// Song

// At the start of a block: picks up a song another thread loaded, handing the
// old one back to be freed (once the last one has been).
void Engine::takeSong() {
    if (retiredSong_.load(std::memory_order_acquire) != nullptr) return;
    Song* incoming = incomingSong_.exchange(nullptr, std::memory_order_acq_rel);
    if (!incoming) return;
    retiredSong_.store(song_, std::memory_order_release);
    song_ = incoming;  // length 0: no song
    songPlaying_ = false;
    songCue_ = 1;
    songStopAt_ = -1;
    songJumpAt_ = -1;
    emptyLanes();
    updateBase();
}

void Engine::emptyLanes() { laneStem_.fill(-1); }

void Engine::startSong(int64_t at) {
    songOrigin_ = at - song_->barStart(songCue_);
    songPlaying_ = true;
    songStopAt_ = -1;
    songJumpAt_ = -1;
}

// Stops, remembering the bar it was in so Play carries on from there.
void Engine::stopSong() {
    if (songPlaying_ && song_) songCue_ = songBarAt(clock() - songOrigin_);
    songPlaying_ = false;
    songStopAt_ = -1;
    songJumpAt_ = -1;
}

int Engine::songBarAt(int64_t index) const {
    if (!song_ || song_->barLength <= 0) return 1;
    const int64_t bars = (index - song_->downbeat) / song_->barLength;
    return static_cast<int>(std::max<int64_t>(0, bars)) + 1;
}

// Song sample i is heard at real time songOrigin_ + i, like a loop's index.
// Into the panned mix, like the loops.
void Engine::renderSong(float* left, float* right, int frames, int64_t start) {
    if (!songPlaying_ || !song_ || song_->length <= 0) return;
    const Song& song = *song_;
    const int64_t latency = outputLatency_.load(std::memory_order_relaxed);
    // Only stems in a lane are heard.
    std::array<float, kSongStems> gains{};
    for (int stem : laneStem_) {
        if (stem >= 0 && stem < song.count) gains[static_cast<size_t>(stem)] = songGain_[static_cast<size_t>(stem)].load(std::memory_order_relaxed);
    }
    std::array<float, kSongStems> peaks{}, sideL{}, sideR{};
    for (int s = 0; s < song.count; s++) {
        const float pan = songPan_[static_cast<size_t>(s)].load(std::memory_order_relaxed);
        sideL[static_cast<size_t>(s)] = std::min(1.0f, 1.0f - pan);
        sideR[static_cast<size_t>(s)] = std::min(1.0f, 1.0f + pan);
    }
    for (int i = 0; i < frames; i++) {
        const int64_t index = start + i + latency - songOrigin_;
        if (index >= song.length) {
            // The end: back to the top for the next Play.
            songPlaying_ = false;
            songCue_ = 1;
            break;
        }
        if (index < 0) continue;
        for (int s = 0; s < song.count; s++) {
            const float v = song.stems[static_cast<size_t>(s)][static_cast<size_t>(index)] * gains[static_cast<size_t>(s)];
            peaks[static_cast<size_t>(s)] = std::max(peaks[static_cast<size_t>(s)], std::fabs(v));
            left[i] += v * sideL[static_cast<size_t>(s)];
            right[i] += v * sideR[static_cast<size_t>(s)];
        }
    }
    for (int s = 0; s < song.count; s++) {
        std::atomic<float>& peak = stemPeak_[static_cast<size_t>(s)];
        if (peaks[static_cast<size_t>(s)] > peak.load(std::memory_order_relaxed)) peak.store(peaks[static_cast<size_t>(s)], std::memory_order_relaxed);
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
    if (song_ && song_->length > 0) {
        // The song's bars are the grid; loops snap to them.
        if (barLength_ != song_->barLength) {
            barLength_ = song_->barLength;
            bars_ = 1;
            baseLength_ = barLength_;
            baseLoop_ = -1;
            for (auto& looper : loops_) looper->setBarLength(barLength_);
        }
        return;
    }
    bool anyLoop = false;
    for (int i = 0; i < kPool; i++) {
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
    if ((song_ && song_->length > 0) || baseLength_ <= 0 || bars < 1 || bars > 8 || (bars & (bars - 1)) != 0 || baseLength_ % bars != 0) return;
    bars_ = bars;
    barLength_ = baseLength_ / bars;
    for (auto& looper : loops_) looper->setBarLength(barLength_);
}

void Engine::publish() {
    // Looper::State and RoundsLoopState share their order.
    static_assert(static_cast<int>(Looper::State::Stopped) == ROUNDS_LOOP_STOPPED, "state order");
    for (int i = 0; i < kLoops; i++) {
        const int id = slotLoop(i);
        const Looper& looper = *loops_[static_cast<size_t>(id)];
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
        const int queued = scheduled_[static_cast<size_t>(id)].action;
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

// `out` is the centred mix; the loops go into loopsL_ / loopsR_ at `offset`.
void Engine::processChunk(const float* input, float* out, int frames, int64_t start, int offset) {
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
    heard_.fill(false);
    for (int slot = 0; slot < kLoops; slot++) heard_[static_cast<size_t>(slotLoop(slot))] = true;
    for (int l = 0; l < kPool; l++) {
        Looper& looper = *loops_[static_cast<size_t>(l)];
        Player& player = *players_[static_cast<size_t>(l)];
        const float gain = loopMute_[static_cast<size_t>(l)].load(std::memory_order_relaxed)
            ? 0.0f : loopGain_[static_cast<size_t>(l)].load(std::memory_order_relaxed);
        if (!heard_[static_cast<size_t>(l)]) {
            // Another part's loop keeps time; its notes ring out after a switch.
            looper.process(input, nullptr, frames, gain);
            player.render(out, frames, gain, nullptr, 0);
            continue;
        }
        std::memset(scratch_.data(), 0, sizeof(float) * static_cast<size_t>(frames));
        const int events = looper.collectNotes(frames, noteEvents_.data(), static_cast<int>(noteEvents_.size()));
        looper.process(input, scratch_.data(), frames, gain);
        player.render(scratch_.data(), frames, gain, noteEvents_.data(), events);
        // Balance: centred, both sides at full level; panned, the other side fades.
        const float pan = loopPan_[static_cast<size_t>(l)].load(std::memory_order_relaxed);
        const float left = std::min(1.0f, 1.0f - pan), right = std::min(1.0f, 1.0f + pan);
        float* L = loopsL_.data() + offset;
        float* R = loopsR_.data() + offset;
        float peak = chunkPeak_[static_cast<size_t>(l)];
        for (int i = 0; i < frames; i++) {
            const float v = scratch_[static_cast<size_t>(i)];
            L[i] += v * left;
            R[i] += v * right;
            peak = std::max(peak, std::fabs(v));
        }
        chunkPeak_[static_cast<size_t>(l)] = peak;
    }
    players_[kKeysPlayer]->render(out, frames, 1.0f, nullptr, 0);
    players_[kPadsPlayer]->render(out, frames, 1.0f, nullptr, 0);
    renderSong(loopsL_.data() + offset, loopsR_.data() + offset, frames, start);
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
    takeSong();

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
    std::memset(loopsL_.data(), 0, sizeof(float) * static_cast<size_t>(frames));
    std::memset(loopsR_.data(), 0, sizeof(float) * static_cast<size_t>(frames));
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
        for (int l = 0; l < kPool; l++) {
            if (scheduled_[static_cast<size_t>(l)].action != kNone && scheduled_[static_cast<size_t>(l)].time <= at) {
                fireScheduled(l);
            }
        }
        if (nextPart_ >= 0 && switchAt_ <= at) fireSwitch();
        if (songStopAt_ >= 0 && songStopAt_ <= at) stopSong();
        if (songJumpAt_ >= 0 && songJumpAt_ <= at) {
            songOrigin_ = songJumpAt_ - song_->barStart(songJumpBar_);
            songJumpAt_ = -1;
        }
        int64_t chunk = frames - pos;
        if (pendingCount_ > 0) chunk = std::min<int64_t>(chunk, pending_[0].time - at);
        for (const Scheduled& s : scheduled_) {
            if (s.action != kNone) chunk = std::min<int64_t>(chunk, s.time - at);
        }
        if (nextPart_ >= 0 && switchAt_ > at) chunk = std::min<int64_t>(chunk, switchAt_ - at);
        if (songStopAt_ > at) chunk = std::min<int64_t>(chunk, songStopAt_ - at);
        if (songJumpAt_ > at) chunk = std::min<int64_t>(chunk, songJumpAt_ - at);
        processChunk(input + pos, outL + pos, static_cast<int>(chunk), at, pos);
        pos += static_cast<int>(chunk);
    }

    // The centred mix plus the panned loops.
    float inPeak = 0.0f;
    float outPeak = 0.0f;
    for (int i = 0; i < frames; i++) {
        const float centre = outL[i];
        outL[i] = centre + loopsL_[static_cast<size_t>(i)];
        if (outR) outR[i] = centre + loopsR_[static_cast<size_t>(i)];
        inPeak = std::max(inPeak, std::fabs(input[i]));
        outPeak = std::max(outPeak, std::max(std::fabs(outL[i]), std::fabs(centre + loopsR_[static_cast<size_t>(i)])));
    }

    now_ += frames;
    stepTransfer();
    stepCopies();
    keepLayersFree();
    updateBase();  // a loop can close itself when it runs out of room
    releaseStaleNotes();
    publish();
    storeMax(statInputPeak_, inPeak);
    storeMax(statOutputPeak_, outPeak);
    for (int l = 0; l < kPool; l++) {
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
        s.shared.store(shared_[static_cast<size_t>(l)] ? 1 : 0, std::memory_order_relaxed);
    }
    for (int p = 0; p < kParts; p++) {
        int loops = 0;
        int64_t partCycle = 0;
        for (int slot = 0; slot < kLoops; slot++) {
            const int id = partLoops_[static_cast<size_t>(p)][static_cast<size_t>(slot)];
            statPartLoops_[static_cast<size_t>(p * kLoops + slot)].store(id, std::memory_order_relaxed);
            const Looper& looper = *loops_[static_cast<size_t>(id)];
            if (looper.state() != Looper::State::Empty) loops++;
            partCycle = std::max(partCycle, looper.loopLength());
        }
        PartStats& ps = partStats_[static_cast<size_t>(p)];
        ps.exists.store(partExists_[static_cast<size_t>(p)] ? 1 : 0, std::memory_order_relaxed);
        ps.loops.store(loops, std::memory_order_relaxed);
        ps.cycle.store(partCycle, std::memory_order_relaxed);
    }
    const bool hasSong = song_ && song_->length > 0;
    statSongLoaded_.store(hasSong ? 1 : 0, std::memory_order_relaxed);
    statSongPlaying_.store(songPlaying_ ? 1 : 0, std::memory_order_relaxed);
    statSongLength_.store(hasSong ? song_->length : 0, std::memory_order_relaxed);
    if (hasSong) {
        const int64_t heard = std::clamp<int64_t>(now_ + out - songOrigin_, 0, song_->length);
        statSongPosition_.store(songPlaying_ ? heard : song_->barStart(songCue_), std::memory_order_relaxed);
        statSongBar_.store(songPlaying_ ? songBarAt(heard) : songCue_, std::memory_order_relaxed);
    }
    for (int slot = 0; slot < kLoops; slot++) {
        statLaneStem_[static_cast<size_t>(slot)].store(laneStem_[static_cast<size_t>(slot)], std::memory_order_relaxed);
    }
    statPart_.store(part_, std::memory_order_relaxed);
    statNextPart_.store(nextPart_, std::memory_order_relaxed);
    statSwitchIn_.store(nextPart_ >= 0 ? std::max<int64_t>(0, switchOrigin_ - (now_ + out)) : -1, std::memory_order_relaxed);
    const int64_t cycle = partStats_[static_cast<size_t>(part_)].cycle.load(std::memory_order_relaxed);
    statBars_.store(bars_, std::memory_order_relaxed);
    statBarLength_.store(barLength_, std::memory_order_relaxed);
    statCycle_.store(cycle, std::memory_order_relaxed);
    statCyclePosition_.store(cycle > 0 ? wrap(now_ + out - gridOrigin_, cycle) : 0, std::memory_order_relaxed);
    statRunning_.store(running() ? 1 : 0, std::memory_order_relaxed);
    statBase_.store(baseLength_, std::memory_order_relaxed);
    // The base loop's slot in the playing part (-1 if it is in another part).
    int baseSlot = -1;
    for (int slot = 0; slot < kLoops; slot++) {
        if (baseLoop_ >= 0 && slotLoop(slot) == baseLoop_) baseSlot = slot;
    }
    statBaseLoop_.store(baseSlot, std::memory_order_relaxed);
    statBaseID_.store(baseLoop_, std::memory_order_relaxed);
    statCalibrating_.store(probe_.active() ? 1 : 0, std::memory_order_relaxed);
    statNow_.store(now_, std::memory_order_relaxed);
}

}  // namespace rounds
