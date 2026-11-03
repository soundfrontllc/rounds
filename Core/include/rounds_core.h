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
// the base length; later loops snap to 1/8x ... 8x of it so they stay in step.
#define ROUNDS_MAX_LOOPS 8

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
} RoundsCommand;

typedef enum {
    ROUNDS_EVENT_STATE_CHANGED = 0,   // value = new RoundsLoopState
    ROUNDS_EVENT_LOOP_LENGTH = 1,     // value = loop length in samples
    ROUNDS_EVENT_LAYER_COUNT = 2,     // value = closed layers (not counting the base)
    ROUNDS_EVENT_CALIBRATION_DONE = 3,// value = measured round trip in samples (-1 = failed)
    ROUNDS_EVENT_QUEUE_FULL = 4,      // a command was dropped
} RoundsEventType;

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
} RoundsLoopStatus;

typedef struct {
    int64_t baseLength;     // samples; 0 until the first loop is recorded
    int32_t baseLoop;       // slot that set the base length, -1 if none
    float inputPeak;        // since last status read, linear 0..1+
    float outputPeak;
    int32_t inputLatency;   // samples, as used for compensation
    int32_t outputLatency;
    int32_t calibrating;
    int64_t sampleTime;     // engine sample clock
    double sampleRate;
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
void rounds_engine_set_loop_gain(RoundsEngine* engine, int32_t loop, float gain);

// Play clicks and measure how long they take to come back in. Needs the
// output physically looped back to the input (a cable), or a mic near the speaker.
void rounds_engine_start_calibration(RoundsEngine* engine);

void rounds_engine_get_status(RoundsEngine* engine, RoundsStatus* out);
void rounds_engine_get_loop_status(RoundsEngine* engine, int32_t loop, RoundsLoopStatus* out);
// Returns 1 and fills `out` if an event was waiting.
int32_t rounds_engine_poll_event(RoundsEngine* engine, RoundsEvent* out);

// Session save/load. One loop at a time; the audio thread copies a slice per
// block, so a save or load never glitches playback. Poll transfer_status
// until it is no longer 1 (it needs the audio running).
//
// Save: begin_export(loop), wait for 2, then take_export(dest, capacity)
// returns the loop's length in samples (-1 on failure). Saved loops are
// rotated to line up with the base loop's start, so loops saved together
// play back in step.
// Load: begin_import(loop, samples, length) copies the samples right away;
// the loop then fills in and ends up stopped. Load the base loop first: the
// first loop to finish loading into an empty engine becomes the base.
// Every loaded loop starts at index 0 together, so tapping one starts them in step.
int64_t rounds_engine_max_loop_samples(RoundsEngine* engine);
int32_t rounds_engine_begin_export(RoundsEngine* engine, int32_t loop);
int32_t rounds_engine_begin_import(RoundsEngine* engine, int32_t loop, const float* samples, int64_t length);
// 0 = idle, 1 = in progress, 2 = done, -1 = failed (loop empty, recording, or
// re-recorded mid-save). Reading 2 after an import, or -1, frees the engine
// for the next transfer.
int32_t rounds_engine_transfer_status(RoundsEngine* engine);
int64_t rounds_engine_take_export(RoundsEngine* engine, float* dest, int64_t capacity);

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
