// Rounds audio core: the narrow C API between the Swift app and the C++ engine.
//
// Threading contract:
//   - rounds_engine_process() is the only function called on the real-time
//     audio thread. It never allocates, locks or blocks.
//   - Every other function is called from non-real-time threads (UI, MIDI).
//     Commands reach the audio thread through a lock-free queue.

#ifndef ROUNDS_CORE_H
#define ROUNDS_CORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Engine

typedef struct RoundsEngine RoundsEngine;

// Loop slots. All loops run on one sample clock. The first loop recorded sets
// the tempo (its length is taken as 1, 2, 4 or 8 bars of 4/4, whichever is
// 80-160 BPM); later loops snap to 1, 2, 4, 8 ... bars so they stay in step.
#define ROUNDS_MAX_LOOPS 8

// Parts (Verse, Chorus ...): each part has the 8 slots, and each slot points
// at one of the engine's loops (loop IDs 0 ..< ROUNDS_POOL_LOOPS; part p's
// own loops are p * 8 + slot). A shared loop is one loop in several parts.
// Commands, notes and loop status take a slot of the current part; gains,
// sharing and session transfers take a loop ID.
#define ROUNDS_MAX_PARTS 8
#define ROUNDS_MAX_SONG_STEMS 6
#define ROUNDS_POOL_LOOPS 64  // ROUNDS_MAX_LOOPS * ROUNDS_MAX_PARTS (a plain number so Swift imports it)

typedef enum {
    ROUNDS_LOOP_EMPTY = 0,
    ROUNDS_LOOP_RECORDING = 1,
    ROUNDS_LOOP_OVERDUBBING = 2,
    ROUNDS_LOOP_PLAYING = 3,
    ROUNDS_LOOP_STOPPED = 4,
} RoundsLoopState;

typedef enum {
    // Record -> Play -> Overdub -> Play ... ; from Stopped: Play.
    ROUNDS_CMD_TAP = 0,
    // Discard the layer being overdubbed, or else remove the last layer.
    ROUNDS_CMD_UNDO = 1,
    ROUNDS_CMD_STOP = 2,
    ROUNDS_CMD_CLEAR = 3,
    // Footswitch double-tap: the tap that began this gesture already fired,
    // so revert what it did (e.g. discard the layer it opened), then undo.
    ROUNDS_CMD_UNDO_REVERTING_TAP = 4,
    // Footswitch hold: revert what the press's tap did, then stop.
    ROUNDS_CMD_STOP_REVERTING_TAP = 5,
    // Play button (send to loop -1): stop every loop, or start every stopped
    // loop together from the top of the longest.
    ROUNDS_CMD_START_STOP_ALL = 6,
} RoundsCommand;

typedef enum {
    ROUNDS_EVENT_STATE_CHANGED = 0,   // value = new RoundsLoopState
    ROUNDS_EVENT_LOOP_LENGTH = 1,     // value = loop length in samples
    ROUNDS_EVENT_LAYER_COUNT = 2,     // value = closed layers (not counting the base)
    ROUNDS_EVENT_CALIBRATION_DONE = 3,// value = measured round trip in samples (-1 = failed)
    ROUNDS_EVENT_QUEUE_FULL = 4,      // a command was dropped
    ROUNDS_EVENT_QUEUED = 5,          // value = RoundsQueued: waiting for the next bar
    // Parts (loop = -1).
    ROUNDS_EVENT_PART_CHANGED = 6,    // value = the part now playing
    ROUNDS_EVENT_PART_QUEUED = 7,     // value = part waiting for the end of the cycle, -1 = taken back
    ROUNDS_EVENT_PART_CREATED = 8,    // value = new part, -1 = all 8 parts are used
} RoundsEventType;

// With quantize on and something playing, record, play and stop wait for the
// next bar (a tap up to 1/8 bar late, at most 250 ms, counts for the bar just
// gone: a recording then starts from that bar line). Overdub in/out and undo
// act at once. A second tap, undo or stop takes a queued action back.
typedef enum {
    ROUNDS_QUEUED_NONE = 0,
    ROUNDS_QUEUED_TAP = 1,    // record (empty loop) or play (stopped loop)
    ROUNDS_QUEUED_STOP = 2,
} RoundsQueued;

// Built-in instruments. Each loop slot owns one; a MIDI loop plays through it.
typedef enum {
    ROUNDS_INSTRUMENT_SUB_BASS = 0,
    ROUNDS_INSTRUMENT_FINGER_BASS = 1,
    ROUNDS_INSTRUMENT_SYNTH_BASS = 2,
    ROUNDS_INSTRUMENT_E_PIANO = 3,
    ROUNDS_INSTRUMENT_WARM_PAD = 4,
    ROUNDS_INSTRUMENT_MONO_LEAD = 5,
    ROUNDS_INSTRUMENT_DRY_KIT = 6,      // drum kits: General MIDI notes (kick = 36)
    ROUNDS_INSTRUMENT_ELECTRO_KIT = 7,
    ROUNDS_INSTRUMENT_COUNT = 8,
} RoundsInstrument;

typedef enum {
    ROUNDS_KIND_AUDIO = 0,
    ROUNDS_KIND_MIDI = 1,
} RoundsLoopKind;

// A recorded note: starts `start` samples into the loop, lasts `length` samples.
typedef struct {
    int64_t start;
    int64_t length;
    int32_t note;
    int32_t velocity;
} RoundsNote;

typedef struct {
    int32_t type;          // RoundsEventType
    int32_t loop;          // loop slot, or -1 for engine-wide events
    int64_t value;
    int64_t sampleTime;    // engine sample clock when it happened
} RoundsEvent;

typedef struct {
    int32_t state;          // RoundsLoopState
    int64_t loopLength;     // samples, 0 while empty or recording
    int64_t recordedLength; // samples recorded so far (while recording)
    int64_t playPosition;   // index heard right now, 0..loopLength
    int32_t layers;         // closed overdub layers
    int32_t layerOpen;      // 1 while an overdub layer is being written
    float peak;             // loop's output since last status read, linear
    float gain;
    int32_t queued;         // RoundsQueued
    int32_t kind;           // RoundsLoopKind (audio until notes are recorded)
    int32_t instrument;     // RoundsInstrument the slot plays
    int32_t notes;          // recorded notes (MIDI loops)
    int32_t loopID;         // the engine loop this slot plays
    int32_t shared;         // 1: "New part" keeps this loop instead of copying it
    int32_t songStem;       // the song stem in this lane (peak is then the stem's), or -1
} RoundsLoopStatus;

typedef struct {
    int32_t exists;
    int32_t loops;          // slots with a loop that isn't empty
    int64_t cycleLength;    // longest loop in the part, samples
} RoundsPartStatus;

typedef struct {
    int64_t baseLength;     // samples; 0 until the first loop is recorded
    int32_t baseLoop;       // slot that set the base length, -1 if none (or in another part)
    int32_t baseLoopID;     // loop ID that set it, -1 if none
    int32_t bars;           // bars in the base loop (4/4), 0 = no tempo yet
    int64_t barLength;      // samples; tempo = 240 * sampleRate / barLength BPM
    int64_t cycleLength;    // longest loop, samples
    int64_t cyclePosition;  // what is heard now, from the bar grid's start, 0..cycleLength
    int32_t running;        // 1 while any loop plays, overdubs or records
    float inputPeak;        // since last status read, linear 0..1+
    float outputPeak;
    int32_t inputLatency;   // samples, as used for compensation
    int32_t outputLatency;
    int32_t calibrating;
    int64_t sampleTime;     // engine sample clock
    double sampleRate;
    int32_t part;           // the part playing
    int32_t nextPart;       // part waiting for the end of the cycle, -1 if none
    int64_t partSwitchIn;   // samples until it is heard, -1 if none
    int32_t songLoaded;     // a whole song (imported stems) is loaded; it sets the tempo
    int32_t songPlaying;
    int64_t songLength;     // samples
    int64_t songPosition;   // song sample heard now (while playing), else where Play starts it
    int32_t songBar;        // bar heard now (from 1), or the bar Play starts from
} RoundsStatus;

// maxLoopSeconds bounds memory per loop slot: all loop and layer buffers are
// allocated here, never on the audio thread.
RoundsEngine* rounds_engine_create(double sampleRate, double maxLoopSeconds, int32_t maxBlockFrames);
void rounds_engine_destroy(RoundsEngine* engine);

// Real-time. `input` is mono, `outL`/`outR` receive the mix. frames <= maxBlockFrames.
void rounds_engine_process(RoundsEngine* engine, const float* input, float* outL, float* outR, int32_t frames);

// Like process, but also records which host time the first frame of this
// block corresponds to, so commands stamped with host times land on the right
// sample. hostTicksPerSample converts host clock ticks to samples.
void rounds_engine_process_timed(RoundsEngine* engine, const float* input, float* outL, float* outR,
                                 int32_t frames, uint64_t hostTime, double hostTicksPerSample);

// Queue a command for loop slot `loop`. sampleTime < 0 means "as soon as
// possible". Returns 0 if the queue was full or the slot is out of range.
int32_t rounds_engine_send(RoundsEngine* engine, int32_t loop, RoundsCommand cmd, int64_t sampleTime);

// Queue a command stamped with a host time (e.g. a CoreMIDI timestamp). The
// command is applied a fixed one block after that moment, so its timing has
// constant latency instead of jitter. hostTime 0 means "now".
int32_t rounds_engine_send_at_host_time(RoundsEngine* engine, int32_t loop, RoundsCommand cmd, uint64_t hostTime);

// Latency used to line recorded audio up with what was heard. Samples.
void rounds_engine_set_latency(RoundsEngine* engine, int32_t inputLatency, int32_t outputLatency);
// Gain for passing the input straight to the output (0 = off).
void rounds_engine_set_monitor_gain(RoundsEngine* engine, float gain);
// Volume of a loop ID (it goes with the loop into every part that shares it).
void rounds_engine_set_loop_gain(RoundsEngine* engine, int32_t loopID, float gain);
// Quantize record/play/stop to the next bar (off by default in the engine).
void rounds_engine_set_quantize(RoundsEngine* engine, int32_t on);
// Click on every beat while anything plays (0 = off, the default).
void rounds_engine_set_click_gain(RoundsEngine* engine, float gain);
// Takes the base loop as `bars` bars (1, 2, 4 or 8), e.g. to halve or double
// the tempo. Ignored without a tempo. The audio isn't changed.
void rounds_engine_set_bars(RoundsEngine* engine, int32_t bars);

// Notes (keys and drum pads). `loop` is the slot whose instrument plays the
// note; -1 just plays `instrument` without recording (keys over an audio loop,
// or pads with no drum loop). A note into a loop that is recording or
// overdubbing is recorded. An empty loop that is recording becomes a MIDI loop
// on its first note, playing `instrument`; a MIDI loop always plays its own.
// velocity 0 = note off. hostTime is the CoreMIDI timestamp (0 = now).
int32_t rounds_engine_note(RoundsEngine* engine, int32_t loop, int32_t instrument, int32_t note, int32_t velocity,
                           uint64_t hostTime);
// The slot's instrument (switches a MIDI loop's sound).
void rounds_engine_set_instrument(RoundsEngine* engine, int32_t loop, int32_t instrument);

// Parts. A switch waits for the end of the current cycle (the longest loop in
// the playing part coming round); with nothing playing it happens at once and
// leaves the part's loops stopped. Asking for the playing or the waiting part
// takes a waiting switch back. Loops only in the old part close any recording
// or overdub at the switch and fall silent, keeping their state; loops only in
// the new part start from their top; shared loops play straight through.
void rounds_engine_switch_part(RoundsEngine* engine, int32_t part);
// Copies the playing part into the first unused part (shared loops are kept,
// the others copied with their overdubs merged) and switches to it like
// switch_part. Emits ROUNDS_EVENT_PART_CREATED.
void rounds_engine_new_part(RoundsEngine* engine);
void rounds_engine_set_shared(RoundsEngine* engine, int32_t loopID, int32_t shared);
// Makes `part` (if it doesn't exist yet) with 8 empty loops no other part
// uses, without switching to it; e.g. for imported stems. Emits PART_CREATED.
void rounds_engine_add_empty_part(RoundsEngine* engine, int32_t part);
void rounds_engine_get_part_status(RoundsEngine* engine, int32_t part, RoundsPartStatus* out);
// The loop ID in a part's slot (as of the last processed block).
int32_t rounds_engine_part_loop(RoundsEngine* engine, int32_t part, int32_t slot);
// Session load: empties every loop and leaves only part 1, with its own loops;
// then set_part_loop makes the saved parts (creating them) before loading loops.
void rounds_engine_reset(RoundsEngine* engine);
void rounds_engine_set_part_loop(RoundsEngine* engine, int32_t part, int32_t slot, int32_t loopID);

// A whole song: imported stems (mono, at the engine's rate, up to
// ROUNDS_MAX_SONG_STEMS) playing in time with the loops, across parts.
// While loaded it sets the tempo and bar grid: `barLength` samples a bar, bar 1
// at song sample `downbeat` (stretch the song to the session's bar length
// first when loops already set one). Copies the stems; returns 0 if they are
// unusable. Play (START_STOP_ALL) starts it with the loops from its start bar;
// stop lands on the next bar like the loops. Loading replaces any song.
int32_t rounds_engine_load_song(RoundsEngine* engine, const float* const* stems, int32_t count, int64_t length,
                                int64_t barLength, int64_t downbeat);
void rounds_engine_unload_song(RoundsEngine* engine);
void rounds_engine_set_song_gain(RoundsEngine* engine, int32_t stem, float gain);
// Where Play starts the song (bar from 1); while it plays, it jumps there at the next bar line.
void rounds_engine_set_song_bar(RoundsEngine* engine, int32_t bar);
// Puts song stem `stem` in lane `slot` (in every part), or empties the lane
// (stem -1). Only stems in a lane are heard. A stem lane only plays: tap,
// stop and undo there do nothing, keys and pads aren't recorded into it, and
// CLEAR takes the stem out. Loading or unloading a song empties every lane.
// Put a stem only in a lane that is empty in every part.
void rounds_engine_set_song_lane(RoundsEngine* engine, int32_t slot, int32_t stem);

// Play clicks and measure how long they take to come back in. Needs the
// output physically looped back to the input (a cable), or a mic near the speaker.
void rounds_engine_start_calibration(RoundsEngine* engine);

void rounds_engine_get_status(RoundsEngine* engine, RoundsStatus* out);
// A slot of the playing part.
void rounds_engine_get_loop_status(RoundsEngine* engine, int32_t loop, RoundsLoopStatus* out);
void rounds_engine_get_pool_loop_status(RoundsEngine* engine, int32_t loopID, RoundsLoopStatus* out);
// Returns 1 and fills `out` if an event was waiting.
int32_t rounds_engine_poll_event(RoundsEngine* engine, RoundsEvent* out);

// Session save/load, by loop ID. One loop at a time; the audio thread copies a slice per
// block, so a save or load never glitches playback. Poll transfer_status
// until it is no longer 1 (it needs the audio running).
//
// Save: begin_export(loopID), wait for 2, then take_export(dest, capacity)
// returns the loop's length in samples (-1 on failure). Saved loops are
// rotated to line up with the base loop's start, so loops saved together
// play back in step.
// Load: begin_import(loopID, samples, length) copies the samples right away;
// the loop then fills in and ends up stopped. Load the base loop first: the
// first loop to finish loading into an empty engine becomes the base.
// Every loaded loop starts at index 0 together, so tapping one starts them in step.
int64_t rounds_engine_max_loop_samples(RoundsEngine* engine);
int32_t rounds_engine_begin_export(RoundsEngine* engine, int32_t loopID);
int32_t rounds_engine_begin_import(RoundsEngine* engine, int32_t loopID, const float* samples, int64_t length);
// 0 = idle, 1 = in progress, 2 = done, -1 = failed (loop empty, recording, or
// re-recorded mid-save). Reading 2 after an import, or -1, frees the engine
// for the next transfer.
int32_t rounds_engine_transfer_status(RoundsEngine* engine);
int64_t rounds_engine_take_export(RoundsEngine* engine, float* dest, int64_t capacity);
// MIDI loops: after begin_export reaches 2, fills `dest` with the loop's notes
// (starts rotated like audio) and returns their count; *length gets the loop
// length and *instrument its instrument. -1 if the export isn't a MIDI loop.
int32_t rounds_engine_take_export_notes(RoundsEngine* engine, RoundsNote* dest, int32_t capacity, int64_t* length,
                                        int32_t* instrument);
// Loads a MIDI loop; finishes like begin_import (poll transfer_status).
int32_t rounds_engine_begin_import_notes(RoundsEngine* engine, int32_t loopID, const RoundsNote* notes, int32_t count,
                                         int64_t length, int32_t instrument);
int32_t rounds_engine_max_notes(RoundsEngine* engine);

// ---------------------------------------------------------------------------
// Footswitch gestures (pure logic, no audio)

typedef struct RoundsFootswitch RoundsFootswitch;

typedef enum {
    ROUNDS_GESTURE_NONE = 0,
    ROUNDS_GESTURE_TAP = 1,         // fired immediately on press
    ROUNDS_GESTURE_DOUBLE_TAP = 2,  // second press soon after the first
    ROUNDS_GESTURE_HOLD = 3,        // held past holdSeconds
    ROUNDS_GESTURE_LONG_HOLD = 4,   // held past longHoldSeconds
} RoundsGesture;

typedef struct {
    double doubleTapSeconds;  // default 0.30
    double holdSeconds;       // default 0.60
    double longHoldSeconds;   // default 2.00
} RoundsFootswitchConfig;

RoundsFootswitchConfig rounds_footswitch_default_config(void);
RoundsFootswitch* rounds_footswitch_create(RoundsFootswitchConfig config);
void rounds_footswitch_destroy(RoundsFootswitch* fs);
// Times are in seconds on any monotonic clock. Each returns the gesture that
// fired (or NONE). Call tick() regularly (every ~10 ms) so holds are detected.
int32_t rounds_footswitch_press(RoundsFootswitch* fs, double time);
int32_t rounds_footswitch_release(RoundsFootswitch* fs, double time);
int32_t rounds_footswitch_tick(RoundsFootswitch* fs, double time);

// Maps a gesture to the engine command it triggers. Returns -1 for NONE.
int32_t rounds_gesture_command(int32_t gesture);

// ---------------------------------------------------------------------------
// macOS audio I/O (Core Audio HAL). Not available on other platforms.

typedef struct RoundsAudioIO RoundsAudioIO;

typedef struct {
    uint32_t deviceID;
    char name[128];
    int32_t inputChannels;
    int32_t outputChannels;
    double sampleRate;
} RoundsAudioDevice;

int32_t rounds_audio_device_count(void);
int32_t rounds_audio_device_info(int32_t index, RoundsAudioDevice* out);
uint32_t rounds_audio_default_input_device(void);

typedef struct {
    double sampleRate;
    int32_t bufferFrames;
    int32_t inputLatency;   // device-reported, samples (includes buffer)
    int32_t outputLatency;
} RoundsAudioInfo;

// Opens `deviceID` for both input and output (an interface with both, or an
// aggregate device). inputChannel is 0-based. On failure returns NULL and
// writes a reason into `error`.
RoundsAudioIO* rounds_audio_open(uint32_t deviceID, int32_t inputChannel, int32_t bufferFrames,
                                 double maxLoopSeconds, RoundsAudioInfo* info,
                                 char* error, int32_t errorLength);
RoundsEngine* rounds_audio_engine(RoundsAudioIO* io);
int32_t rounds_audio_start(RoundsAudioIO* io);
void rounds_audio_stop(RoundsAudioIO* io);
void rounds_audio_close(RoundsAudioIO* io);

// Host clock helpers (mach_absolute_time ticks, as used by CoreMIDI timestamps).
uint64_t rounds_host_time_now(void);
double rounds_host_time_to_seconds(uint64_t hostTime);

#ifdef __cplusplus
}
#endif

#endif
