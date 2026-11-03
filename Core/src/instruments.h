#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "midi_notes.h"
#include "rounds_core.h"

namespace rounds {

// The drum kits' sounds, synthesized once at startup and shared read-only by
// every player. Notes follow General MIDI (kick C1 = 36, Ableton naming).
class DrumKits {
public:
    static constexpr int kKits = 2;
    static constexpr int kSounds = 8;  // Kick, Snare, Clap, Closed hat, Open hat, Low tom, Shaker, Rim

    explicit DrumKits(double sampleRate);
    static int soundForNote(int note);  // -1 if the note plays nothing
    const std::vector<float>& sample(int kit, int sound) const {
        return samples_[static_cast<size_t>(kit)][static_cast<size_t>(sound)];
    }

private:
    std::array<std::array<std::vector<float>, kSounds>, kKits> samples_;
};

bool isDrumKit(int instrument);

// One instrument: a fixed pool of voices playing a synth preset or a drum kit.
// All memory is allocated in the constructor; everything else is real-time safe.
class Player {
public:
    Player(double sampleRate, const DrumKits* kits);

    // Releases what was playing and switches sound.
    void setInstrument(int instrument);
    int instrument() const { return instrument_; }

    // fromLoop marks notes played back by a MIDI loop, so stopping or undoing
    // the loop releases them without cutting off keys still being held.
    void noteOn(int note, int velocity, bool fromLoop);
    void noteOff(int note, bool fromLoop);
    void releaseLoopNotes();
    void releaseAll();

    // Adds `frames` samples times `gain` into `out`, applying loop notes at
    // their offsets (events sorted by offset).
    void render(float* out, int frames, float gain, const NoteEvent* events, int count);

    struct Preset;  // synth settings, one per instrument (instruments.cpp)

private:
    struct Voice {
        bool active = false;
        bool held = false;        // key down (or loop note not yet ended)
        bool fromLoop = false;
        int note = -1;
        float velocity = 0.0f;
        uint64_t age = 0;
        // Synth
        float freq = 0.0f;        // Hz, gliding toward target
        float target = 0.0f;
        float phase[3] = {0, 0, 0};
        float env = 0.0f;
        int stage = 0;            // 0 attack, 1 decay/sustain, 2 release
        float filterEnv = 0.0f;
        float ic1 = 0.0f, ic2 = 0.0f;
        float g = 0.0f, k = 2.0f; // filter coefficients, refreshed every few samples
        float modEnv = 0.0f;
        // Drums
        int sound = -1;
        int64_t pos = 0;
        float gain = 0.0f;
        float fade = 1.0f;        // choke fade-out
        bool choking = false;
    };

    const Preset& preset() const;
    Voice* allocate();
    void startSynth(Voice& v, int note, int velocity, bool fromLoop);
    void release(Voice& v);
    void renderVoice(Voice& v, float* out, int frames, float gain);
    void renderDrum(Voice& v, float* out, int frames, float gain);

    double sampleRate_;
    const DrumKits* kits_;
    int instrument_ = ROUNDS_INSTRUMENT_SUB_BASS;
    std::array<Voice, 16> voices_{};
    uint64_t counter_ = 0;
    // Mono presets: keys held, newest last, for last-note priority.
    std::array<int, 16> heldNotes_{};
    int heldCount_ = 0;
};

}  // namespace rounds
