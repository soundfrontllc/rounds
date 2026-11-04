#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "instruments.h"
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
//
// Parts: each of the 8 parts has 8 slots pointing into a pool of 64 loops
// (part p's own loops are p * 8 + slot; a shared loop sits in several parts).
// Every loop keeps time all the time; only the playing part's loops are heard.
// Overdub layers come from one pool shared by all loops.
class Engine {
public:
    static constexpr int kLoops = ROUNDS_MAX_LOOPS;   // slots in a part
    static constexpr int kParts = ROUNDS_MAX_PARTS;
    static constexpr int kPool = ROUNDS_POOL_LOOPS;
    static_assert(kPool == kLoops * kParts, "a loop for every slot of every part");
    static constexpr int kLayers = 24;                // shared overdub layers

    Engine(double sampleRate, double maxLoopSeconds, int maxBlockFrames);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Real-time.
    void process(const float* input, float* outL, float* outR, int frames);
    void processTimed(const float* input, float* outL, float* outR, int frames, uint64_t hostTime,
                      double hostTicksPerSample);

    // Any non-real-time thread.
    bool send(int loop, RoundsCommand cmd, int64_t sampleTime);
    bool sendAtHostTime(int loop, RoundsCommand cmd, uint64_t hostTime);
    // A key or pad (see rounds_engine_note). Played one block after it was
    // pressed, recorded at the moment it was pressed.
    bool note(int loop, int instrument, int note, int velocity, uint64_t hostTime);
    bool setInstrument(int loop, int instrument);
    void setLatency(int inputLatency, int outputLatency);
    void setMonitorGain(float gain) { monitorGain_.store(gain); }
    void setLoopGain(int loopID, float gain);
    void setLoopMute(int loopID, bool muted);
    void setLoopPan(int loopID, float pan);
    void setQuantize(bool on) { quantize_.store(on); }
    void setClickGain(float gain) { clickGain_.store(std::max(0.0f, gain)); }
    // Reinterprets the base loop as `bars` bars (1, 2, 4 or 8); the audio is untouched.
    void setBars(int bars) { requestedBars_.store(bars); }
    static int chooseBars(int64_t baseLength, double sampleRate);
    void startCalibration() { calibrateRequested_.store(true); }
    void getStatus(RoundsStatus* out);
    void getLoopStatus(int slot, RoundsLoopStatus* out);
    void getPoolLoopStatus(int loopID, RoundsLoopStatus* out);
    void getPartStatus(int part, RoundsPartStatus* out);
    int partLoop(int part, int slot) const;
    bool switchPart(int part);
    bool newPart();
    bool addEmptyPart(int part);
    bool setShared(int loopID, bool shared);
    bool reset();
    bool setPartLoop(int part, int slot, int loopID);
    // Song (see rounds_engine_load_song). count 0 unloads.
    bool loadSong(const float* const* stems, int count, int64_t length, int64_t barLength, int64_t downbeat);
    void setSongGain(int stem, float gain);
    void setSongPan(int stem, float pan);
    bool setSongBar(int bar);
    bool setSongLane(int slot, int stem);
    static constexpr int kSongStems = ROUNDS_MAX_SONG_STEMS;
    bool pollEvent(RoundsEvent* out) { return events_.pop(*out); }

    // Session save/load: one loop at a time through a buffer the audio thread
    // copies a slice of per block (see rounds_core.h).
    bool beginExport(int loop);
    bool beginImport(int loop, const float* samples, int64_t length);
    int transferStatus();
    int64_t takeExport(float* dest, int64_t capacity);
    int64_t maxLoopSamples() const { return loops_[0]->maxLoopSamples(); }
    bool beginImportNotes(int loop, const RoundsNote* notes, int count, int64_t length, int instrument);
    int takeExportNotes(RoundsNote* dest, int capacity, int64_t* length, int* instrument);
    int maxNotes() const { return loops_[0]->maxNotes(); }

    double sampleRate() const { return sampleRate_; }

private:
    struct Command {
        int32_t loop;
        int32_t cmd;
        int64_t time;
        int64_t when = -1;       // notes: real time the key was pressed
        int16_t note = 0;
        int16_t velocity = 0;
        int16_t instrument = 0;
    };
    // Internal commands, beside RoundsCommand. Notes and instruments name a
    // slot; parts a part (in `loop`); sharing a loop ID.
    static constexpr int32_t kCmdNote = 100;
    static constexpr int32_t kCmdInstrument = 101;
    static constexpr int32_t kCmdSwitchPart = 102;
    static constexpr int32_t kCmdNewPart = 103;
    static constexpr int32_t kCmdShared = 104;
    static constexpr int32_t kCmdReset = 105;
    static constexpr int32_t kCmdPartLoop = 106;   // loop = part, note = slot, instrument = loop ID
    static constexpr int32_t kCmdEmptyPart = 107;  // loop = part
    static constexpr int32_t kCmdSongBar = 108;    // loop = bar
    static constexpr int32_t kCmdSongLane = 109;   // loop = slot, note = stem or -1
    static constexpr int kKeysPlayer = kPool;      // keys over an audio loop
    static constexpr int kPadsPlayer = kPool + 1;  // pads with no drum loop
    static constexpr int64_t kCopySlice = 65536;   // samples copied per block for a new part

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

    // A whole song. Built by other threads, handed to the audio thread through
    // incomingSong_ and back through retiredSong_ to be freed off it. A song
    // of length 0 means "unload".
    struct Song {
        std::array<std::vector<float>, ROUNDS_MAX_SONG_STEMS> stems;
        int count = 0;
        int64_t length = 0;
        int64_t barLength = 0;
        int64_t downbeat = 0;
        int64_t barStart(int bar) const { return downbeat + static_cast<int64_t>(bar - 1) * barLength; }
    };

    // A loop being copied into a new part, a slice per block.
    struct Copy {
        int from = -1;
        int to = -1;
        int64_t length = 0;
        int64_t pos = 0;
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
        std::atomic<int> kind{0};
        std::atomic<int> instrument{0};
        std::atomic<int> notes{0};
        std::atomic<int> shared{0};
    };
    struct PartStats {
        std::atomic<int> exists{0};
        std::atomic<int> loops{0};
        std::atomic<int64_t> cycle{0};
    };

    bool push(const Command& command);
    int64_t hostToSample(uint64_t hostTime);
    void apply(const Command& command);
    void applyNote(const Command& command, int id);
    int slotLoop(int slot) const { return partLoops_[static_cast<size_t>(part_)][static_cast<size_t>(slot)]; }
    bool inPart(int part, int id) const;
    bool audible(int id) const;
    int longestInPart() const;
    void requestSwitch(int part);
    void scheduleSwitch();
    void fireSwitch();
    void createPart();
    void createEmptyPart(int part);
    int freeLoopFor(int part, int slot) const;
    void resetParts();
    void stepCopies();
    void keepLayersFree();
    void resyncPublished();
    void takeSong();
    void startSong(int64_t at);
    void stopSong();
    void emptyLanes();
    void renderSong(float* left, float* right, int frames, int64_t start);
    int songBarAt(int64_t index) const;
    int64_t nextBarLine(int64_t now) const;
    void freeRetiredSong();
    void releaseStaleNotes();
    // Below, `loop` is a loop ID.
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
    bool running() const;
    void renderClick(float* out, int frames, int64_t start);
    void publish();
    void emit(RoundsEventType type, int loop, int64_t value);
    void processChunk(const float* input, float* out, int frames, int64_t start, int offset);
    void stepTransfer();
    bool importing(int loop) const;

    enum Transfer { kIdle, kExportRequested, kExporting, kExportDone, kImportRequested, kImporting, kImportDone, kFailed };
    static constexpr int64_t kTransferSlice = 32768;

    double sampleRate_;
    int maxBlock_;
    std::unique_ptr<LayerPool> layerPool_;
    std::vector<std::unique_ptr<Looper>> loops_;    // the pool, by loop ID
    DrumKits kits_;
    std::vector<std::unique_ptr<Player>> players_;  // one per loop ID, then keys and pads
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
    std::vector<float> loopsL_, loopsR_;  // the loops, panned (the rest of the mix is centred)
    std::array<float, kPool> chunkPeak_{};
    std::array<Published, kLoops> published_{};   // per slot of the playing part
    std::array<bool, kPool> heard_{};             // in the playing part (this chunk)
    int64_t baseLength_ = 0;
    int baseLoop_ = -1;
    int bars_ = 0;              // bars in the base loop, 0 = no tempo yet
    int64_t barLength_ = 0;     // samples
    int64_t gridOrigin_ = 0;    // real time a bar starts at (mod barLength_)
    std::array<Scheduled, kPool> scheduled_{};
    std::array<NoteEvent, 256> noteEvents_{};
    std::array<bool, kPool> midiAudible_{};   // to release a loop's notes when it stops
    std::array<int, kPool> midiNotes_{};      // or loses notes (undo, clear)
    std::array<bool, kPool> shared_{};
    // Parts.
    std::array<std::array<int, kLoops>, kParts> partLoops_{};
    std::array<bool, kParts> partExists_{};
    int part_ = 0;
    int nextPart_ = -1;
    int64_t switchAt_ = 0;       // clock time of the switch
    int64_t switchOrigin_ = 0;   // real time the new part's loops start (switchAt_ + output latency)
    bool switchNow_ = false;     // nothing was playing: switch without starting anything
    std::array<Copy, kLoops> copies_{};
    // Song (audio thread).
    Song* song_ = nullptr;
    bool songPlaying_ = false;
    int64_t songOrigin_ = 0;    // real time song sample 0 is heard
    int songCue_ = 1;           // bar Play starts it from
    int64_t songStopAt_ = -1;   // clock time of a stop waiting for the bar line
    int64_t songJumpAt_ = -1;   // clock time of a jump to songJumpBar_
    int songJumpBar_ = 1;
    std::atomic<Song*> incomingSong_{nullptr};
    std::atomic<Song*> retiredSong_{nullptr};
    std::mutex songMutex_;      // loaders only (never the audio thread)
    std::array<std::atomic<float>, kSongStems> songGain_;
    std::array<std::atomic<float>, kSongStems> songPan_;
    std::array<int, kLoops> laneStem_{};          // stem in each lane, -1 for none
    std::array<std::atomic<int>, kLoops> statLaneStem_;
    std::array<std::atomic<float>, kSongStems> stemPeak_;
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
    std::array<std::atomic<float>, kPool> loopGain_;
    std::array<std::atomic<bool>, kPool> loopMute_;
    std::array<std::atomic<float>, kPool> loopPan_;
    void copyMix(int from, int to);
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
    bool transferMidi_ = false;
    std::vector<RoundsNote> transferNotes_;
    int transferNoteCount_ = 0;
    int transferInstrument_ = 0;

    std::array<LoopStats, kPool> loopStats_;
    std::array<PartStats, kParts> partStats_;
    std::array<std::atomic<int>, kParts * kLoops> statPartLoops_;
    std::atomic<int> statPart_{0};
    std::atomic<int> statNextPart_{-1};
    std::atomic<int64_t> statSwitchIn_{-1};
    std::atomic<int> statSongLoaded_{0};
    std::atomic<int> statSongPlaying_{0};
    std::atomic<int64_t> statSongLength_{0};
    std::atomic<int64_t> statSongPosition_{0};
    std::atomic<int> statSongBar_{1};
    std::atomic<int64_t> statBase_{0};
    std::atomic<int> statBaseLoop_{-1};
    std::atomic<int> statBaseID_{-1};
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
