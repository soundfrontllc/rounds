#pragma once

#include <cstdint>
#include <vector>

namespace rounds {

// One audio loop with an undoable stack of overdub layers.
//
// Timing model (everything is in samples on one clock):
//   - The looper's clock `c` advances one per processed sample.
//   - Input processed at clock c is the sound that happened at real time
//     s = c - inputLatency.
//   - Output written at clock c is heard at real time c + outputLatency.
//   - A command applied at clock c happened at real time c.
// So the sound played at real time s is stored at the index that was being
// heard at real time s, and recorded loops line up with what the player heard.
//
// All memory is allocated in the constructor. Every other method is
// real-time safe and must only be called from the audio thread.
class Looper {
public:
    enum class State { Empty, Recording, Overdubbing, Playing, Stopped };

    struct Config {
        int64_t maxLoopSamples = 48000 * 60;
        int crossfadeSamples = 256;    // seam crossfade and layer punch in/out ramps
        int maxUndoLayers = 8;         // older layers are merged into the base
        int flattenSamplesPerBlock = 16384;
        int64_t minLoopSamples = 4800;
        int historySamples = 24000;    // recent input kept so a recording can start slightly in the past
    };

    explicit Looper(const Config& config);

    void setLatency(int inputLatency, int outputLatency);
    // Closed loops snap to a power-of-two number of bars (1, 2, 4, 8 ...).
    // 0 = no tempo yet: this loop sets it, and its length is only rounded to a
    // multiple of 8 samples.
    void setBarLength(int64_t barLength) { barLength_ = barLength; }
    // Moves the loop's phase; used to restart all loops together from the top.
    void shiftOrigin(int64_t delta) { origin_ += delta; }
    int64_t origin() const { return origin_; }
    int inputLatency() const { return inputLatency_; }
    int outputLatency() const { return outputLatency_; }

    // Commands take effect at the current clock (between samples).
    void tap();
    // Like tap(), but a recording started from Empty begins at real time
    // `recordFrom` (at most historySamples in the past) instead of now.
    void tapRecordingFrom(int64_t recordFrom);
    // The engine queued this tap for the next bar: a double-tap or hold must not
    // revert the tap before it.
    void forgetTap() { lastTap_ = TapEffect::None; }
    void revertTap() { revertLastTap(); }
    void undo();
    void undoRevertingTap();   // double-tap: revert the last tap's effect, then undo
    void stop();
    void stopRevertingTap();   // hold: revert the last tap's effect, then stop
    void clear();

    // Adds the loop's playback (times `gain`) into `out`, and records `in`.
    void process(const float* in, float* out, int frames, float gain);

    State state() const { return state_; }
    int64_t loopLength() const { return loopLength_; }
    int64_t recordedLength() const;
    int64_t heardPosition() const;   // loop index being heard right now
    int closedLayers() const;
    bool layerOpen() const { return openLayer_ >= 0; }
    int64_t clock() const { return clock_; }
    int64_t maxLoopSamples() const { return config_.maxLoopSamples; }

    // Session save/load, a slice at a time from the audio thread.
    // sampleAt() is what the loop plays at `index` (base plus every layer).
    float sampleAt(int64_t index) const { return readLoop(index); }
    // Empties the loop and writes `count` samples of a saved loop at `offset`.
    void loadSlice(const float* samples, int64_t offset, int64_t count);
    // Ends a load: the loop has `length` samples, is stopped, and index 0 is
    // heard at real time `origin`.
    void finishLoad(int64_t length, int64_t origin);

private:
    enum class TapEffect { None, StartedRecording, FinishedRecording, OpenedLayer, ClosedLayer, Restarted };

    struct Layer {
        std::vector<float> buffer;
        bool inUse = false;
        int64_t startS = 0;       // real time the layer starts recording
        int64_t closeS = INT64_MAX;  // real time it was closed (tail continues for crossfade)
        int64_t openIndex = 0;    // loop index of its first sample
        int64_t written = 0;      // samples written so far
        bool writing = true;      // false once the punch-out tail is done
    };

    void startRecording(int64_t from);
    bool finishRecording();    // false if the loop would be too short
    int64_t snapLength(int64_t recorded, int64_t minimum) const;
    int openLayer();
    void closeLayer();
    void discardLayer(int slot);
    void removeLastClosedLayer();
    void revertLastTap();
    void finishPendingWrites();
    void writeInput(float sample, int64_t s);
    float readLoop(int64_t index) const;
    bool layerCovers(const Layer& layer, int64_t index) const;
    void stepFlatten();
    int freeSlot() const;

    Config config_;
    int inputLatency_ = 0;
    int outputLatency_ = 0;

    State state_ = State::Empty;
    TapEffect lastTap_ = TapEffect::None;
    int64_t clock_ = 0;

    // Base recording. The buffer has crossfadeSamples of room past the max
    // loop length to catch the audio that continues past the loop end.
    std::vector<float> base_;
    int64_t baseStartS_ = 0;
    int64_t baseWritten_ = 0;
    bool baseWriting_ = false;
    int64_t loopLength_ = 0;
    int64_t origin_ = 0;      // real time at which loop index 0 is heard (mod length)
    int64_t barLength_ = 0;
    std::vector<float> history_;  // input by real time, modulo its size

    std::vector<Layer> layers_;
    std::vector<int> order_;  // slots of layers in the stack, oldest first
    int openLayer_ = -1;      // slot of the layer being overdubbed, -1 if none

    int flattenSlot_ = -1;    // oldest layer being merged into the base
    int64_t flattenPos_ = 0;
};

}  // namespace rounds
