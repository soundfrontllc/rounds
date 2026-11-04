#include "rounds_core.h"

#include "engine.h"
#include "footswitch.h"

struct RoundsEngine {
    rounds::Engine engine;
    RoundsEngine(double sampleRate, double maxLoopSeconds, int maxBlock) : engine(sampleRate, maxLoopSeconds, maxBlock) {}
};

struct RoundsFootswitch {
    rounds::Footswitch footswitch;
    explicit RoundsFootswitch(const RoundsFootswitchConfig& config) : footswitch(config) {}
};

extern "C" {

RoundsEngine* rounds_engine_create(double sampleRate, double maxLoopSeconds, int32_t maxBlockFrames) {
    return new RoundsEngine(sampleRate, maxLoopSeconds, maxBlockFrames);
}

void rounds_engine_destroy(RoundsEngine* engine) { delete engine; }

void rounds_engine_process(RoundsEngine* engine, const float* input, float* outL, float* outR, int32_t frames) {
    engine->engine.process(input, outL, outR, frames);
}

void rounds_engine_process_timed(RoundsEngine* engine, const float* input, float* outL, float* outR, int32_t frames,
                                 uint64_t hostTime, double hostTicksPerSample) {
    engine->engine.processTimed(input, outL, outR, frames, hostTime, hostTicksPerSample);
}

int32_t rounds_engine_send(RoundsEngine* engine, int32_t loop, RoundsCommand cmd, int64_t sampleTime) {
    return engine->engine.send(loop, cmd, sampleTime) ? 1 : 0;
}

int32_t rounds_engine_send_at_host_time(RoundsEngine* engine, int32_t loop, RoundsCommand cmd, uint64_t hostTime) {
    return engine->engine.sendAtHostTime(loop, cmd, hostTime) ? 1 : 0;
}

int32_t rounds_engine_note(RoundsEngine* engine, int32_t loop, int32_t instrument, int32_t note, int32_t velocity,
                           uint64_t hostTime) {
    return engine->engine.note(loop, instrument, note, velocity, hostTime) ? 1 : 0;
}

void rounds_engine_set_instrument(RoundsEngine* engine, int32_t loop, int32_t instrument) {
    engine->engine.setInstrument(loop, instrument);
}

void rounds_engine_set_latency(RoundsEngine* engine, int32_t inputLatency, int32_t outputLatency) {
    engine->engine.setLatency(inputLatency, outputLatency);
}

void rounds_engine_set_monitor_gain(RoundsEngine* engine, float gain) { engine->engine.setMonitorGain(gain); }

void rounds_engine_set_loop_gain(RoundsEngine* engine, int32_t loop, float gain) {
    engine->engine.setLoopGain(loop, gain);
}

void rounds_engine_set_quantize(RoundsEngine* engine, int32_t on) { engine->engine.setQuantize(on != 0); }

void rounds_engine_set_click_gain(RoundsEngine* engine, float gain) { engine->engine.setClickGain(gain); }

void rounds_engine_set_bars(RoundsEngine* engine, int32_t bars) { engine->engine.setBars(bars); }

void rounds_engine_start_calibration(RoundsEngine* engine) { engine->engine.startCalibration(); }

void rounds_engine_get_status(RoundsEngine* engine, RoundsStatus* out) { engine->engine.getStatus(out); }

void rounds_engine_get_loop_status(RoundsEngine* engine, int32_t loop, RoundsLoopStatus* out) {
    engine->engine.getLoopStatus(loop, out);
}

void rounds_engine_get_pool_loop_status(RoundsEngine* engine, int32_t loopID, RoundsLoopStatus* out) {
    engine->engine.getPoolLoopStatus(loopID, out);
}

int32_t rounds_engine_load_song(RoundsEngine* engine, const float* const* stems, int32_t count, int64_t length,
                                int64_t barLength, int64_t downbeat) {
    return engine->engine.loadSong(stems, count, length, barLength, downbeat) ? 1 : 0;
}

void rounds_engine_unload_song(RoundsEngine* engine) { engine->engine.loadSong(nullptr, 0, 0, 0, 0); }

void rounds_engine_set_song_gain(RoundsEngine* engine, int32_t stem, float gain) {
    engine->engine.setSongGain(stem, gain);
}

void rounds_engine_set_song_bar(RoundsEngine* engine, int32_t bar) { engine->engine.setSongBar(bar); }

void rounds_engine_set_song_lane(RoundsEngine* engine, int32_t slot, int32_t stem) {
    engine->engine.setSongLane(slot, stem);
}

void rounds_engine_switch_part(RoundsEngine* engine, int32_t part) { engine->engine.switchPart(part); }

void rounds_engine_new_part(RoundsEngine* engine) { engine->engine.newPart(); }

void rounds_engine_add_empty_part(RoundsEngine* engine, int32_t part) { engine->engine.addEmptyPart(part); }

void rounds_engine_set_shared(RoundsEngine* engine, int32_t loopID, int32_t shared) {
    engine->engine.setShared(loopID, shared != 0);
}

void rounds_engine_get_part_status(RoundsEngine* engine, int32_t part, RoundsPartStatus* out) {
    engine->engine.getPartStatus(part, out);
}

int32_t rounds_engine_part_loop(RoundsEngine* engine, int32_t part, int32_t slot) {
    return engine->engine.partLoop(part, slot);
}

void rounds_engine_reset(RoundsEngine* engine) { engine->engine.reset(); }

void rounds_engine_set_part_loop(RoundsEngine* engine, int32_t part, int32_t slot, int32_t loopID) {
    engine->engine.setPartLoop(part, slot, loopID);
}

int32_t rounds_engine_poll_event(RoundsEngine* engine, RoundsEvent* out) {
    return engine->engine.pollEvent(out) ? 1 : 0;
}

int64_t rounds_engine_max_loop_samples(RoundsEngine* engine) { return engine->engine.maxLoopSamples(); }

int32_t rounds_engine_begin_export(RoundsEngine* engine, int32_t loop) {
    return engine->engine.beginExport(loop) ? 1 : 0;
}

int32_t rounds_engine_begin_import(RoundsEngine* engine, int32_t loop, const float* samples, int64_t length) {
    return engine->engine.beginImport(loop, samples, length) ? 1 : 0;
}

int32_t rounds_engine_transfer_status(RoundsEngine* engine) { return engine->engine.transferStatus(); }

int32_t rounds_engine_take_export_notes(RoundsEngine* engine, RoundsNote* dest, int32_t capacity, int64_t* length,
                                        int32_t* instrument) {
    int inst = 0;
    const int count = engine->engine.takeExportNotes(dest, capacity, length, &inst);
    *instrument = inst;
    return count;
}

int32_t rounds_engine_begin_import_notes(RoundsEngine* engine, int32_t loop, const RoundsNote* notes, int32_t count,
                                         int64_t length, int32_t instrument) {
    return engine->engine.beginImportNotes(loop, notes, count, length, instrument) ? 1 : 0;
}

int32_t rounds_engine_max_notes(RoundsEngine* engine) { return engine->engine.maxNotes(); }

int64_t rounds_engine_take_export(RoundsEngine* engine, float* dest, int64_t capacity) {
    return engine->engine.takeExport(dest, capacity);
}

RoundsFootswitchConfig rounds_footswitch_default_config(void) {
    RoundsFootswitchConfig config;
    config.doubleTapSeconds = 0.30;
    config.holdSeconds = 0.60;
    config.longHoldSeconds = 2.00;
    return config;
}

RoundsFootswitch* rounds_footswitch_create(RoundsFootswitchConfig config) { return new RoundsFootswitch(config); }

void rounds_footswitch_destroy(RoundsFootswitch* fs) { delete fs; }

int32_t rounds_footswitch_press(RoundsFootswitch* fs, double time) { return fs->footswitch.press(time); }

int32_t rounds_footswitch_release(RoundsFootswitch* fs, double time) { return fs->footswitch.release(time); }

int32_t rounds_footswitch_tick(RoundsFootswitch* fs, double time) { return fs->footswitch.tick(time); }

int32_t rounds_gesture_command(int32_t gesture) {
    switch (gesture) {
        case ROUNDS_GESTURE_TAP: return ROUNDS_CMD_TAP;
        case ROUNDS_GESTURE_DOUBLE_TAP: return ROUNDS_CMD_UNDO_REVERTING_TAP;
        case ROUNDS_GESTURE_HOLD: return ROUNDS_CMD_STOP_REVERTING_TAP;
        case ROUNDS_GESTURE_LONG_HOLD: return ROUNDS_CMD_CLEAR;
        default: return -1;
    }
}

}  // extern "C"
