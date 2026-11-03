#include "looper.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace rounds {

namespace {
int64_t wrap(int64_t value, int64_t length) {
    int64_t r = value % length;
    return r < 0 ? r + length : r;
}
}  // namespace

LayerPool::LayerPool(int count, int64_t samples) {
    layers.resize(static_cast<size_t>(count));
    for (auto& layer : layers) layer.buffer.assign(static_cast<size_t>(samples), 0.0f);
}

int LayerPool::freeCount() const {
    int count = 0;
    for (const auto& layer : layers) count += layer.inUse ? 0 : 1;
    return count;
}

Looper::Looper(const Config& config, LayerPool* pool) : config_(config), pool_(pool) {
    base_.assign(static_cast<size_t>(config_.maxLoopSamples + config_.crossfadeSamples), 0.0f);
    if (!pool_) {
        // Two spare slots: one for a new layer while the oldest is being merged.
        ownPool_ = std::make_unique<LayerPool>(config_.maxUndoLayers + 2, config_.maxLoopSamples);
        pool_ = ownPool_.get();
    }
    order_.reserve(static_cast<size_t>(config_.maxUndoLayers + 2));
    history_.assign(static_cast<size_t>(std::max(1, config_.historySamples)), 0.0f);
    notes_.resize(static_cast<size_t>(std::max(1, config_.maxNotes)));
}

void Looper::setLatency(int inputLatency, int outputLatency) {
    inputLatency_ = std::max(0, inputLatency);
    outputLatency_ = std::max(0, outputLatency);
}

int64_t Looper::recordedLength() const {
    if (state_ == State::Recording) return clock_ - baseStartS_;
    return loopLength_;
}

int64_t Looper::heardPosition() const {
    if (loopLength_ == 0) return 0;
    return wrap(clock_ - origin_, loopLength_);
}

int Looper::closedLayers() const {
    int count = static_cast<int>(order_.size());
    if (openLayer_ >= 0) count--;
    return count;
}

// ---------------------------------------------------------------------------
// Commands

void Looper::tap() { tapRecordingFrom(clock_); }

void Looper::tapRecordingFrom(int64_t recordFrom) {
    switch (state_) {
        case State::Empty:
            startRecording(std::min(recordFrom, clock_));
            lastTap_ = TapEffect::StartedRecording;
            break;
        case State::Recording:
            if (!finishRecording()) return;  // too short: keep recording
            state_ = State::Playing;
            lastTap_ = TapEffect::FinishedRecording;
            break;
        case State::Overdubbing:
            closeLayer();
            state_ = State::Playing;
            lastTap_ = TapEffect::ClosedLayer;
            break;
        case State::Playing:
            openLayer();
            state_ = State::Overdubbing;
            lastTap_ = TapEffect::OpenedLayer;
            break;
        case State::Stopped:
            // Rejoin in phase; the engine moves the origin when nothing else plays.
            finishPendingWrites();
            state_ = State::Playing;
            lastTap_ = TapEffect::Restarted;
            break;
    }
}

void Looper::revertLastTap() {
    switch (lastTap_) {
        case TapEffect::StartedRecording:
            if (state_ == State::Recording) clear();
            break;
        case TapEffect::OpenedLayer:
            if (state_ == State::Overdubbing && openLayer_ >= 0) {
                discardLayer(openLayer_);
                state_ = State::Playing;
            }
            break;
        case TapEffect::Restarted:
            if (state_ == State::Playing) state_ = State::Stopped;
            break;
        // Closing the loop or a layer keeps what was played: nothing to revert.
        case TapEffect::FinishedRecording:
        case TapEffect::ClosedLayer:
        case TapEffect::None:
            break;
    }
    lastTap_ = TapEffect::None;
}

void Looper::undo() {
    lastTap_ = TapEffect::None;
    if (state_ == State::Recording) {
        clear();
        return;
    }
    if (openLayer_ >= 0) {
        discardLayer(openLayer_);
        if (state_ == State::Overdubbing) state_ = State::Playing;
        return;
    }
    removeLastClosedLayer();
}

void Looper::undoRevertingTap() {
    revertLastTap();
    if (state_ == State::Empty) return;
    removeLastClosedLayer();
}

void Looper::stop() {
    lastTap_ = TapEffect::None;
    switch (state_) {
        case State::Empty:
        case State::Stopped:
            return;
        case State::Recording:
            if (!finishRecording()) {
                clear();
                return;
            }
            break;
        case State::Overdubbing:
            closeLayer();
            break;
        case State::Playing:
            break;
    }
    state_ = State::Stopped;
}

void Looper::stopRevertingTap() {
    revertLastTap();
    stop();
}

void Looper::clear() {
    state_ = State::Empty;
    lastTap_ = TapEffect::None;
    baseWriting_ = false;
    baseWritten_ = 0;
    loopLength_ = 0;
    for (int slot : order_) layer(slot).inUse = false;
    order_.clear();
    openLayer_ = -1;
    flattenSlot_ = -1;
    kind_ = Kind::Audio;
    noteCount_ = 0;
}

// ---------------------------------------------------------------------------
// Recording and layers

void Looper::startRecording(int64_t from) {
    clear();
    state_ = State::Recording;
    baseStartS_ = from;
    baseWritten_ = 0;
    baseWriting_ = true;
    origin_ = from;
    // Input up to real time `heard` has already gone by; copy what was played
    // since `from` out of the history (silence for anything older than it holds).
    const int64_t heard = clock_ - inputLatency_;
    const int64_t size = static_cast<int64_t>(history_.size());
    for (int64_t s = from; s < heard; s++) {
        base_[static_cast<size_t>(baseWritten_++)] = s >= heard - size ? history_[static_cast<size_t>(wrap(s, size))] : 0.0f;
    }
    replayRecent(from, -1);
}

bool Looper::finishRecording() {
    const int64_t length = clock_ - baseStartS_;
    const int64_t minimum =
        std::max<int64_t>(config_.minLoopSamples, inputLatency_ + outputLatency_ + 2 * config_.crossfadeSamples);
    if (length < minimum) return false;
    loopLength_ = snapLength(std::min(length, config_.maxLoopSamples), minimum);
    if (kind_ == Kind::Midi) {
        // Notes past the snapped end were the next time round: drop them.
        int kept = 0;
        for (int i = 0; i < noteCount_; i++) {
            Note n = notes_[static_cast<size_t>(i)];
            if (n.index >= loopLength_) continue;
            if (n.length >= 0) n.length = std::min(n.length, loopLength_ - 1);
            notes_[static_cast<size_t>(kept++)] = n;
        }
        noteCount_ = kept;
    }
    // Input keeps arriving for inputLatency samples, plus crossfadeSamples of
    // overflow past the end for the seam crossfade; writeInput() finishes it.
    return true;
}

// If the snapped length is longer than what was played, recording carries on
// to the new end (writeInput) and readLoop plays silence until it gets there.
int64_t Looper::snapLength(int64_t recorded, int64_t minimum) const {
    if (barLength_ <= 0) return recorded - recorded % 8;
    int64_t best = -1;
    for (int64_t candidate = barLength_; candidate <= config_.maxLoopSamples; candidate *= 2) {
        if (candidate < minimum) continue;
        if (best < 0 || std::llabs(recorded - candidate) < std::llabs(recorded - best)) best = candidate;
    }
    return best > 0 ? best : recorded;
}

int Looper::freeSlot() const {
    for (size_t i = 0; i < pool_->layers.size(); i++) {
        if (!pool_->layers[i].inUse) return static_cast<int>(i);
    }
    return -1;
}

int Looper::openLayer() {
    if (closedLayers() >= config_.maxUndoLayers && flattenSlot_ < 0 && !order_.empty()) {
        flattenSlot_ = order_.front();
        flattenPos_ = 0;
    }
    int slot = freeSlot();
    if (slot < 0) {
        // Layers opened faster than they can be merged (or a shared pool ran
        // dry): merge this loop's oldest at once.
        if (flattenSlot_ < 0 && !order_.empty()) {
            flattenSlot_ = order_.front();
            flattenPos_ = 0;
        }
        while (flattenSlot_ >= 0) stepFlatten();
        slot = freeSlot();
        if (slot < 0) return -1;
    }
    Layer& l = layer(slot);
    l.inUse = true;
    l.age = ++pool_->counter;
    l.startS = clock_;
    l.closeS = INT64_MAX;
    l.openIndex = wrap(clock_ - origin_, loopLength_);
    l.written = 0;
    l.writing = true;
    order_.push_back(slot);
    openLayer_ = slot;
    if (kind_ == Kind::Midi) replayRecent(clock_, slot);
    return slot;
}

void Looper::closeLayer() {
    if (openLayer_ < 0) return;
    layer(openLayer_).closeS = clock_;
    openLayer_ = -1;
}

void Looper::discardLayer(int slot) {
    if (slot < 0) return;
    if (slot == flattenSlot_) flattenSlot_ = -1;  // cannot happen for the open layer, but stay safe
    layer(slot).inUse = false;
    order_.erase(std::remove(order_.begin(), order_.end(), slot), order_.end());
    if (slot == openLayer_) openLayer_ = -1;
    removeNotes(slot);
}

void Looper::removeLastClosedLayer() {
    for (auto it = order_.rbegin(); it != order_.rend(); ++it) {
        if (*it == openLayer_) continue;
        if (*it == flattenSlot_) return;  // already being merged; no longer undoable
        discardLayer(*it);
        return;
    }
}

// A restart can only happen from Stopped; anything still finishing its
// punch-out tail is cut short so it doesn't write against the new origin.
void Looper::finishPendingWrites() {
    if (baseWriting_ && loopLength_ > 0) baseWriting_ = false;
    for (int slot : order_) layer(slot).writing = false;
}

// ---------------------------------------------------------------------------
// Shared layer pool

uint64_t Looper::oldestLayerAge() const {
    if (flattenSlot_ >= 0 || order_.empty() || order_.front() == openLayer_) return 0;
    return layer(order_.front()).age;
}

void Looper::mergeOldestLayer() {
    if (oldestLayerAge() == 0) return;
    flattenSlot_ = order_.front();
    flattenPos_ = 0;
}

// ---------------------------------------------------------------------------
// Copying (a new part)

void Looper::copySlice(const Looper& source, int64_t offset, int64_t count) {
    if (offset == 0) clear();
    count = std::min(count, config_.maxLoopSamples - offset);
    for (int64_t k = 0; k < count; k++) {
        base_[static_cast<size_t>(offset + k)] = source.readLoop(offset + k);
    }
}

void Looper::finishCopy(const Looper& source) {
    clear();  // keeps base_
    loopLength_ = std::min(source.loopLength_, config_.maxLoopSamples);
    origin_ = source.origin_;
    barLength_ = source.barLength_;
    kind_ = source.kind_;
    if (kind_ == Kind::Midi) {
        for (int i = 0; i < source.noteCount_ && noteCount_ < config_.maxNotes; i++) {
            Note n = source.notes_[static_cast<size_t>(i)];
            if (n.length < 0) n.length = std::max<int64_t>(1, loopLength_ / 16);  // still held
            n.layer = -1;
            notes_[static_cast<size_t>(noteCount_++)] = n;
        }
    }
    state_ = source.state_ == State::Stopped ? State::Stopped : State::Playing;
}

// ---------------------------------------------------------------------------
// Session load

void Looper::loadSlice(const float* samples, int64_t offset, int64_t count) {
    if (offset == 0) clear();
    count = std::min(count, config_.maxLoopSamples - offset);
    if (count > 0) std::memcpy(&base_[static_cast<size_t>(offset)], samples, sizeof(float) * static_cast<size_t>(count));
}

void Looper::finishLoad(int64_t length, int64_t origin) {
    clear();  // keeps base_, drops any layers
    loopLength_ = std::min(length, config_.maxLoopSamples);
    origin_ = origin;
    state_ = State::Stopped;
}

// ---------------------------------------------------------------------------
// MIDI

// Loop index of a note pressed at real time `pressS`. While recording, a note
// slightly before the start lands on it.
void Looper::addNote(int64_t pressS, int64_t releaseS, int note, int velocity, int layer) {
    if (noteCount_ >= config_.maxNotes) return;
    int64_t index = pressS - origin_;
    index = loopLength_ > 0 ? wrap(index, loopLength_) : std::max<int64_t>(0, index);
    int64_t length = releaseS >= 0 ? std::max<int64_t>(1, releaseS - pressS) : -1;
    if (loopLength_ > 0 && length >= 0) length = std::min(length, loopLength_ - 1);
    notes_[static_cast<size_t>(noteCount_++)] = Note{index, length, pressS, note, velocity, layer};
}

void Looper::removeNotes(int layer) {
    int kept = 0;
    for (int i = 0; i < noteCount_; i++) {
        if (notes_[static_cast<size_t>(i)].layer != layer) notes_[static_cast<size_t>(kept++)] = notes_[static_cast<size_t>(i)];
    }
    noteCount_ = kept;
}

void Looper::replayRecent(int64_t from, int layer) {
    for (Recent& r : recent_) {
        if (r.used || r.pressS < from - config_.notePreroll || r.pressS > clock_) continue;
        r.used = true;
        kind_ = Kind::Midi;
        addNote(r.pressS, r.releaseS, r.note, r.velocity, layer);
    }
}

void Looper::notePressed(int note, int velocity, int64_t s) {
    if (state_ == State::Recording) {
        kind_ = Kind::Midi;
        addNote(s, -1, note, velocity, -1);
    } else if (state_ == State::Overdubbing && kind_ == Kind::Midi && openLayer_ >= 0) {
        addNote(s, -1, note, velocity, openLayer_);
    } else {
        recent_[static_cast<size_t>(recentNext_)] = Recent{s, -1, note, velocity, false};
        recentNext_ = (recentNext_ + 1) % static_cast<int>(recent_.size());
    }
}

void Looper::noteReleased(int note, int64_t s) {
    for (int i = noteCount_ - 1; i >= 0; i--) {
        Note& n = notes_[static_cast<size_t>(i)];
        if (n.note != note || n.length >= 0) continue;
        n.length = std::max<int64_t>(1, s - n.pressS);
        if (loopLength_ > 0) n.length = std::min(n.length, loopLength_ - 1);
        return;
    }
    for (Recent& r : recent_) {
        if (!r.used && r.note == note && r.releaseS < 0) r.releaseS = s;
    }
}

int Looper::collectNotes(int frames, NoteEvent* out, int capacity) const {
    if (kind_ != Kind::Midi || loopLength_ <= 0) return 0;
    if (state_ != State::Playing && state_ != State::Overdubbing) return 0;
    const int64_t start = wrap(clock_ + outputLatency_ - origin_, loopLength_);
    int count = 0;
    for (int i = 0; i < noteCount_ && count + 2 <= capacity; i++) {
        const Note& n = notes_[static_cast<size_t>(i)];
        const int64_t on = wrap(n.index - start, loopLength_);
        if (on < frames) out[count++] = NoteEvent{static_cast<int>(on), n.note, n.velocity};
        if (n.length >= 0) {
            const int64_t off = wrap(n.index + n.length - start, loopLength_);
            if (off < frames) out[count++] = NoteEvent{static_cast<int>(off), n.note, 0};
        }
    }
    // By offset; at the same offset a note ends before one starts.
    for (int i = 1; i < count; i++) {
        const NoteEvent e = out[i];
        int j = i;
        while (j > 0 && (out[j - 1].offset > e.offset ||
                         (out[j - 1].offset == e.offset && out[j - 1].velocity > 0 && e.velocity == 0))) {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = e;
    }
    return count;
}

int Looper::exportNotes(RoundsNote* dest, int capacity, int64_t rotation) const {
    if (kind_ != Kind::Midi || loopLength_ <= 0) return 0;
    int count = 0;
    for (int i = 0; i < noteCount_ && count < capacity; i++) {
        const Note& n = notes_[static_cast<size_t>(i)];
        // A key still held when saving gets a sixteenth of the loop.
        const int64_t length = n.length >= 0 ? n.length : std::max<int64_t>(1, loopLength_ / 16);
        dest[count++] = RoundsNote{wrap(n.index - rotation, loopLength_), length, n.note, n.velocity};
    }
    return count;
}

void Looper::loadNotes(const RoundsNote* notes, int count, int64_t length, int64_t origin) {
    clear();
    loopLength_ = std::min(length, config_.maxLoopSamples);
    if (loopLength_ <= 1) {
        loopLength_ = 0;
        return;
    }
    kind_ = Kind::Midi;
    origin_ = origin;
    for (int i = 0; i < count && noteCount_ < config_.maxNotes; i++) {
        const RoundsNote& n = notes[i];
        const int64_t noteLength = std::clamp<int64_t>(n.length, 1, loopLength_ - 1);
        notes_[static_cast<size_t>(noteCount_++)] =
            Note{wrap(n.start, loopLength_), noteLength, 0, std::clamp(n.note, 0, 127), std::clamp(n.velocity, 1, 127), -1};
    }
    state_ = State::Stopped;
}

// ---------------------------------------------------------------------------
// Audio

void Looper::writeInput(float sample, int64_t s) {
    const int fade = config_.crossfadeSamples;
    history_[static_cast<size_t>(wrap(s, static_cast<int64_t>(history_.size())))] = sample;
    if (kind_ == Kind::Midi) return;

    if (baseWriting_ && s >= baseStartS_) {
        const int64_t end = loopLength_ > 0 ? loopLength_ + fade : config_.maxLoopSamples + fade;
        if (baseWritten_ < end) base_[static_cast<size_t>(baseWritten_++)] = sample;
        if (loopLength_ > 0 && baseWritten_ >= end) {
            // Seam: fade the start in while fading out the audio that
            // continued past the loop end.
            for (int k = 0; k < fade; k++) {
                const float g = static_cast<float>(k) / static_cast<float>(fade);
                float& head = base_[static_cast<size_t>(k)];
                head = head * g + base_[static_cast<size_t>(loopLength_ + k)] * (1.0f - g);
            }
            baseWriting_ = false;
        }
    }

    if (loopLength_ == 0) return;

    for (int slot : order_) {
        Layer& layer = this->layer(slot);
        if (!layer.writing || s < layer.startS) continue;
        float gain = std::min(1.0f, static_cast<float>(layer.written) / static_cast<float>(fade));
        if (s >= layer.closeS) {
            const int64_t into = s - layer.closeS;
            if (into >= fade) {
                layer.writing = false;
                continue;
            }
            gain = std::min(gain, 1.0f - static_cast<float>(into) / static_cast<float>(fade));
        }
        const size_t index = static_cast<size_t>(wrap(layer.openIndex + layer.written, loopLength_));
        if (layer.written < loopLength_) {
            layer.buffer[index] = sample * gain;
        } else {
            layer.buffer[index] += sample * gain;
        }
        layer.written++;
    }
}

bool Looper::layerCovers(const Layer& layer, int64_t index) const {
    const int64_t covered = std::min(layer.written, loopLength_);
    return wrap(index - layer.openIndex, loopLength_) < covered;
}

float Looper::readLoop(int64_t index) const {
    // Not recorded yet (a loop that snapped longer than was played): silence,
    // not whatever an earlier take left in the buffer.
    float sum = baseWriting_ && index >= baseWritten_ ? 0.0f : base_[static_cast<size_t>(index)];
    for (size_t i = 0; i < order_.size(); i++) {
        const int slot = order_[i];
        if (slot == flattenSlot_ && index < flattenPos_) continue;  // already merged into base
        const Layer& layer = this->layer(slot);
        if (layerCovers(layer, index)) sum += layer.buffer[static_cast<size_t>(index)];
    }
    return sum;
}

void Looper::stepFlatten() {
    if (flattenSlot_ < 0) return;
    Layer& layer = this->layer(flattenSlot_);
    if (kind_ == Kind::Midi) {
        // Notes merge at once: they just join the base.
        for (int i = 0; i < noteCount_; i++) {
            if (notes_[static_cast<size_t>(i)].layer == flattenSlot_) notes_[static_cast<size_t>(i)].layer = -1;
        }
        flattenPos_ = loopLength_;
    }
    const int64_t end = std::min(loopLength_, flattenPos_ + config_.flattenSamplesPerBlock);
    for (int64_t k = flattenPos_; k < end; k++) {
        if (layerCovers(layer, k)) base_[static_cast<size_t>(k)] += layer.buffer[static_cast<size_t>(k)];
    }
    flattenPos_ = end;
    if (flattenPos_ >= loopLength_) {
        const int slot = flattenSlot_;
        flattenSlot_ = -1;
        layer.inUse = false;
        order_.erase(std::remove(order_.begin(), order_.end(), slot), order_.end());
    }
}

void Looper::process(const float* in, float* out, int frames, float gain) {
    if (state_ == State::Recording && clock_ - baseStartS_ >= config_.maxLoopSamples - inputLatency_) {
        // Out of room: close the loop and play it.
        if (finishRecording()) {
            state_ = State::Playing;
            lastTap_ = TapEffect::None;
        } else {
            clear();
        }
    }

    stepFlatten();

    const bool audible = out && (state_ == State::Playing || state_ == State::Overdubbing);
    for (int i = 0; i < frames; i++) {
        const int64_t c = clock_ + i;
        writeInput(in[i], c - inputLatency_);
        if (audible && loopLength_ > 0 && kind_ == Kind::Audio) {
            const int64_t index = wrap(c + outputLatency_ - origin_, loopLength_);
            out[i] += readLoop(index) * gain;
        }
    }
    clock_ += frames;
}

}  // namespace rounds
