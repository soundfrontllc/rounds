#include "looper.h"

#include <algorithm>
#include <cstring>

namespace rounds {

namespace {
int64_t wrap(int64_t value, int64_t length) {
    int64_t r = value % length;
    return r < 0 ? r + length : r;
}
}  // namespace

Looper::Looper(const Config& config) : config_(config) {
    base_.assign(static_cast<size_t>(config_.maxLoopSamples + config_.crossfadeSamples), 0.0f);
    // Two spare slots: one for a new layer while the oldest is being merged.
    layers_.resize(static_cast<size_t>(config_.maxUndoLayers + 2));
    for (auto& layer : layers_) layer.buffer.assign(static_cast<size_t>(config_.maxLoopSamples), 0.0f);
    order_.reserve(layers_.size());
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

void Looper::tap() {
    switch (state_) {
        case State::Empty:
            startRecording();
            lastTap_ = TapEffect::StartedRecording;
            break;
        case State::Recording:
            if (!finishRecording()) return;  // too short: keep recording
            openLayer();
            state_ = State::Overdubbing;
            lastTap_ = TapEffect::FinishedRecordingAndOpenedLayer;
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
            finishPendingWrites();
            origin_ = clock_;
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
        case TapEffect::FinishedRecordingAndOpenedLayer:
        case TapEffect::OpenedLayer:
            if (state_ == State::Overdubbing && openLayer_ >= 0) {
                discardLayer(openLayer_);
                state_ = State::Playing;
            }
            break;
        case TapEffect::Restarted:
            if (state_ == State::Playing) state_ = State::Stopped;
            break;
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
    for (auto& layer : layers_) layer.inUse = false;
    order_.clear();
    openLayer_ = -1;
    flattenSlot_ = -1;
}

// ---------------------------------------------------------------------------
// Recording and layers

void Looper::startRecording() {
    clear();
    state_ = State::Recording;
    baseStartS_ = clock_;
    baseWritten_ = 0;
    baseWriting_ = true;
    origin_ = clock_;
}

bool Looper::finishRecording() {
    const int64_t length = clock_ - baseStartS_;
    const int64_t minimum =
        std::max<int64_t>(config_.minLoopSamples, inputLatency_ + outputLatency_ + 2 * config_.crossfadeSamples);
    if (length < minimum) return false;
    loopLength_ = std::min(length, config_.maxLoopSamples);
    // Input keeps arriving for inputLatency samples, plus crossfadeSamples of
    // overflow past the end for the seam crossfade; writeInput() finishes it.
    return true;
}

int Looper::freeSlot() const {
    for (size_t i = 0; i < layers_.size(); i++) {
        if (!layers_[i].inUse) return static_cast<int>(i);
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
        // Only possible if layers are opened faster than they can be merged.
        while (flattenSlot_ >= 0) stepFlatten();
        slot = freeSlot();
        if (slot < 0) return -1;
    }
    Layer& layer = layers_[static_cast<size_t>(slot)];
    layer.inUse = true;
    layer.startS = clock_;
    layer.closeS = INT64_MAX;
    layer.openIndex = wrap(clock_ - origin_, loopLength_);
    layer.written = 0;
    layer.writing = true;
    order_.push_back(slot);
    openLayer_ = slot;
    return slot;
}

void Looper::closeLayer() {
    if (openLayer_ < 0) return;
    layers_[static_cast<size_t>(openLayer_)].closeS = clock_;
    openLayer_ = -1;
}

void Looper::discardLayer(int slot) {
    if (slot < 0) return;
    if (slot == flattenSlot_) flattenSlot_ = -1;  // cannot happen for the open layer, but stay safe
    layers_[static_cast<size_t>(slot)].inUse = false;
    order_.erase(std::remove(order_.begin(), order_.end(), slot), order_.end());
    if (slot == openLayer_) openLayer_ = -1;
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
    for (auto& layer : layers_) {
        if (layer.inUse) layer.writing = false;
    }
}

// ---------------------------------------------------------------------------
// Audio

void Looper::writeInput(float sample, int64_t s) {
    const int fade = config_.crossfadeSamples;

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
        Layer& layer = layers_[static_cast<size_t>(slot)];
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
    float sum = base_[static_cast<size_t>(index)];
    for (size_t i = 0; i < order_.size(); i++) {
        const int slot = order_[i];
        if (slot == flattenSlot_ && index < flattenPos_) continue;  // already merged into base
        const Layer& layer = layers_[static_cast<size_t>(slot)];
        if (layerCovers(layer, index)) sum += layer.buffer[static_cast<size_t>(index)];
    }
    return sum;
}

void Looper::stepFlatten() {
    if (flattenSlot_ < 0) return;
    Layer& layer = layers_[static_cast<size_t>(flattenSlot_)];
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

    const bool audible = state_ == State::Playing || state_ == State::Overdubbing;
    for (int i = 0; i < frames; i++) {
        const int64_t c = clock_ + i;
        writeInput(in[i], c - inputLatency_);
        if (audible && loopLength_ > 0) {
            const int64_t index = wrap(c + outputLatency_ - origin_, loopLength_);
            out[i] += readLoop(index) * gain;
        }
    }
    clock_ += frames;
}

}  // namespace rounds
