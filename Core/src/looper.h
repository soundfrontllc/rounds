#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "midi_notes.h"
#include "rounds_core.h"

namespace rounds {

// Overdub layer buffers, shared by every loop of an engine (or owned by one
// loop on its own). A loop holds a layer from the overdub that opens it until
// the layer is undone or merged into the loop's base.
struct LayerPool {
    struct Layer {
        std::vector<float> buffer;
        bool inUse = false;
        uint64_t age = 0;         // when it was opened, pool-wide
        int64_t startS = 0;       // real time the layer starts recording
        int64_t closeS = INT64_MAX;  // real time it was closed (tail continues for crossfade)
        int64_t openIndex = 0;    // loop index of its first sample
        int64_t written = 0;      // samples written so far
        bool writing = true;      // false once the punch-out tail is done
    };
    LayerPool(int layers, int64_t samples);
    int freeCount() const;
    std::vector<Layer> layers;
    uint64_t counter = 0;
};

// One loop with an undoable stack of overdub layers. A loop records audio
// until a note arrives while it records; it is then a MIDI loop, keeping
// notes instead (tagged with their layer, so undo works the same way).
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
    enum class Kind { Audio, Midi };

    struct Config {
        int64_t maxLoopSamples = 48000 * 60;
        int crossfadeSamples = 256;    // seam crossfade and layer punch in/out ramps
        int maxUndoLayers = 8;         // older layers are merged into the base
        int flattenSamplesPerBlock = 16384;
        int64_t minLoopSamples = 4800;
        int historySamples = 24000;    // recent input kept so a recording can start slightly in the past
        int maxNotes = 4096;
        int64_t notePreroll = 4410;    // a note this early for a recording or overdub still counts
    };

    // Without a pool the loop gets its own, with room for its undo layers.
    explicit Looper(const Config& config, LayerPool* pool = nullptr);

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
    // out == nullptr: the loop is in a part that isn't playing; it only keeps time.
    void process(const float* in, float* out, int frames, float gain);

    State state() const { return state_; }
    int64_t loopLength() const { return loopLength_; }
    int64_t recordedLength() const;
    int64_t heardPosition() const;   // loop index being heard right now
    int closedLayers() const;
    bool layerOpen() const { return openLayer_ >= 0; }
    int64_t clock() const { return clock_; }
    int64_t maxLoopSamples() const { return config_.maxLoopSamples; }
    Kind kind() const { return kind_; }
    int noteCount() const { return noteCount_; }
    int maxNotes() const { return config_.maxNotes; }

    // Shared pool upkeep: when the pool runs low the engine merges the
    // globally oldest closed layer into its loop's base, a slice per block.
    // Age of this loop's oldest closed layer that could be merged now (0 = none).
    uint64_t oldestLayerAge() const;
    void mergeOldestLayer();
    bool merging() const { return flattenSlot_ >= 0; }

    // Copies another loop (a new part's copy of a loop): overdubs merged, no
    // undo history. copySlice() a range of samples at a time, then finishCopy().
    void copySlice(const Looper& source, int64_t offset, int64_t count);
    void finishCopy(const Looper& source);

    // A key pressed or released at real time `s`. Recorded while recording or
    // overdubbing (a recording becomes MIDI on its first note); notes pressed
    // just before a recording or overdub starts are picked up when it does.
    void notePressed(int note, int velocity, int64_t s);
    void noteReleased(int note, int64_t s);
    // The loop's notes heard in the next `frames` samples, sorted by offset.
    int collectNotes(int frames, NoteEvent* out, int capacity) const;

    // Session save/load, a slice at a time from the audio thread.
    // sampleAt() is what the loop plays at `index` (base plus every layer).
    float sampleAt(int64_t index) const { return readLoop(index); }
    // Empties the loop and writes `count` samples of a saved loop at `offset`.
    void loadSlice(const float* samples, int64_t offset, int64_t count);
    // Ends a load: the loop has `length` samples, is stopped, and index 0 is
    // heard at real time `origin`.
    void finishLoad(int64_t length, int64_t origin);
    // MIDI: saved note k starts at loop index (start + rotation) % length.
    int exportNotes(RoundsNote* dest, int capacity, int64_t rotation) const;
    void loadNotes(const RoundsNote* notes, int count, int64_t length, int64_t origin);

private:
    enum class TapEffect { None, StartedRecording, FinishedRecording, OpenedLayer, ClosedLayer, Restarted };

    using Layer = LayerPool::Layer;
    Layer& layer(int slot) { return pool_->layers[static_cast<size_t>(slot)]; }
    const Layer& layer(int slot) const { return pool_->layers[static_cast<size_t>(slot)]; }

    // index: loop index (from the recording's start while recording);
    // length -1 while the key is held; layer -1 = base.
    struct Note {
        int64_t index;
        int64_t length;
        int64_t pressS;
        int note;
        int velocity;
        int layer;
    };
    // A key pressed while nothing was recording, kept briefly in case a
    // recording or overdub starts right after it.
    struct Recent {
        int64_t pressS = 0;
        int64_t releaseS = -1;
        int note = 0;
        int velocity = 0;
        bool used = true;
    };

    void addNote(int64_t pressS, int64_t releaseS, int note, int velocity, int layer);
    void removeNotes(int layer);
    void replayRecent(int64_t from, int layer);
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

    std::unique_ptr<LayerPool> ownPool_;
    LayerPool* pool_;
    std::vector<int> order_;  // pool slots of this loop's layers, oldest first
    int openLayer_ = -1;      // slot of the layer being overdubbed, -1 if none

    int flattenSlot_ = -1;    // oldest layer being merged into the base
    int64_t flattenPos_ = 0;

    Kind kind_ = Kind::Audio;
    std::vector<Note> notes_;
    int noteCount_ = 0;
    std::array<Recent, 16> recent_{};
    int recentNext_ = 0;
};

}  // namespace rounds
