#include "instruments.h"

#include <algorithm>
#include <cmath>

namespace rounds {

namespace {
constexpr float kTwoPi = 6.2831853f;

// Deterministic noise, so the kits sound the same on every launch.
struct Noise {
    uint32_t state = 0x9E3779B9u;
    float next() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return static_cast<float>(state) / 2147483648.0f - 1.0f;
    }
};

// Topology-preserving state variable filter (Simper). Used offline for the
// kits and per voice for the synths.
struct Svf {
    float ic1 = 0.0f, ic2 = 0.0f;
    float lp = 0.0f, bp = 0.0f, hp = 0.0f;
    void run(float x, float g, float k) {
        const float a1 = 1.0f / (1.0f + g * (g + k));
        const float a2 = g * a1;
        const float a3 = g * a2;
        const float v3 = x - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        lp = v2;
        bp = v1;
        hp = x - k * v1 - v2;
    }
};

float coefficient(double sampleRate, float hz) {
    return std::tan(3.14159265f * std::min(hz, static_cast<float>(sampleRate) * 0.45f) / static_cast<float>(sampleRate));
}

float polyBlep(float t, float dt) {
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0f;
    }
    if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

void normalize(std::vector<float>& s, float peak) {
    float max = 0.0f;
    for (float v : s) max = std::max(max, std::fabs(v));
    if (max <= 0.0f) return;
    for (float& v : s) v *= peak / max;
}

// One drum sound. `t` runs in seconds; kit 0 is dry and acoustic-ish, kit 1
// electronic (808-style).
std::vector<float> makeDrum(double sr, int kit, int sound) {
    static constexpr float kLengths[2][DrumKits::kSounds] = {
        {0.5f, 0.35f, 0.4f, 0.12f, 0.6f, 0.6f, 0.2f, 0.08f},
        {1.2f, 0.4f, 0.45f, 0.1f, 0.7f, 0.8f, 0.25f, 0.25f},
    };
    const float rate = static_cast<float>(sr);
    std::vector<float> s(static_cast<size_t>(kLengths[kit][sound] * rate), 0.0f);
    Noise noise;
    noise.state += static_cast<uint32_t>(kit * 977 + sound * 131);
    Svf f1, f2;
    float phase = 0.0f, phase2 = 0.0f;
    const bool electro = kit == 1;
    for (size_t i = 0; i < s.size(); i++) {
        const float t = static_cast<float>(i) / rate;
        float x = 0.0f;
        switch (sound) {
            case 0: {  // Kick: falling sine plus a short click
                const float freq = electro ? 45.0f + 120.0f * std::exp(-t / 0.03f) : 50.0f + 110.0f * std::exp(-t / 0.04f);
                phase += freq / rate;
                const float body = std::sin(kTwoPi * phase) * std::exp(-t / (electro ? 0.45f : 0.22f));
                const float click = noise.next() * std::exp(-t / 0.003f) * (electro ? 0.1f : 0.35f);
                x = std::tanh((body + click) * (electro ? 1.8f : 1.3f));
                break;
            }
            case 1: {  // Snare: tone plus high-passed noise
                phase += 185.0f / rate;
                phase2 += 330.0f / rate;
                f1.run(noise.next(), coefficient(sr, electro ? 1800.0f : 1200.0f), 1.4f);
                const float tone = (std::sin(kTwoPi * phase) + 0.5f * std::sin(kTwoPi * phase2)) * std::exp(-t / 0.06f);
                x = tone * (electro ? 0.35f : 0.55f) + f1.hp * std::exp(-t / (electro ? 0.14f : 0.1f)) * 0.8f;
                break;
            }
            case 2: {  // Clap: three quick bursts and a tail of band-passed noise
                f1.run(noise.next(), coefficient(sr, electro ? 1100.0f : 1400.0f), 0.9f);
                float env = 0.0f;
                for (int burst = 0; burst < 3; burst++) {
                    const float start = 0.011f * static_cast<float>(burst);
                    if (t >= start) env = std::max(env, std::exp(-(t - start) / 0.006f));
                }
                if (t >= 0.033f) env = std::max(env, 0.7f * std::exp(-(t - 0.033f) / (electro ? 0.15f : 0.11f)));
                x = f1.bp * env;
                break;
            }
            case 3:    // Closed hat
            case 4: {  // Open hat
                float source;
                if (electro) {
                    // Six detuned square waves, the classic metallic hat.
                    static constexpr float kFreqs[6] = {205.3f, 304.4f, 369.6f, 522.7f, 540.0f, 800.0f};
                    source = 0.0f;
                    for (float hz : kFreqs) source += std::fmod(t * hz * 2.0f, 1.0f) < 0.5f ? 1.0f : -1.0f;
                    source = source / 6.0f + noise.next() * 0.3f;
                } else {
                    source = noise.next();
                }
                f1.run(source, coefficient(sr, 7000.0f), 1.2f);
                f2.run(f1.hp, coefficient(sr, 9000.0f), 1.0f);
                const float decay = sound == 3 ? (electro ? 0.025f : 0.03f) : (electro ? 0.28f : 0.22f);
                x = f2.hp * std::exp(-t / decay);
                break;
            }
            case 5: {  // Low tom
                const float freq = electro ? 75.0f + 90.0f * std::exp(-t / 0.12f) : 90.0f + 60.0f * std::exp(-t / 0.08f);
                phase += freq / rate;
                x = std::sin(kTwoPi * phase) * std::exp(-t / (electro ? 0.35f : 0.28f)) +
                    noise.next() * std::exp(-t / 0.01f) * 0.15f;
                break;
            }
            case 6: {  // Shaker: soft attack, band-passed noise
                f1.run(noise.next(), coefficient(sr, electro ? 6500.0f : 5000.0f), 1.0f);
                const float attack = std::min(1.0f, t / 0.015f);
                x = f1.bp * attack * std::exp(-std::max(0.0f, t - 0.015f) / 0.05f);
                break;
            }
            case 7: {  // Rim (dry) / cowbell-ish rim (electro)
                if (electro) {
                    phase += 545.0f / rate;
                    phase2 += 815.0f / rate;
                    const float sq = (std::fmod(phase, 1.0f) < 0.5f ? 1.0f : -1.0f) + (std::fmod(phase2, 1.0f) < 0.5f ? 1.0f : -1.0f);
                    f1.run(sq, coefficient(sr, 2600.0f), 1.0f);
                    x = f1.bp * std::exp(-t / 0.07f);
                } else {
                    phase += 1700.0f / rate;
                    phase2 += 420.0f / rate;
                    x = std::sin(kTwoPi * phase) * std::exp(-t / 0.008f) + 0.6f * std::sin(kTwoPi * phase2) * std::exp(-t / 0.012f) +
                        noise.next() * std::exp(-t / 0.004f) * 0.3f;
                }
                break;
            }
            default: break;
        }
        s[i] = x;
    }
    // A few samples of fade-out so the end never clicks.
    const size_t tail = std::min<size_t>(s.size(), static_cast<size_t>(rate * 0.005f));
    for (size_t i = 0; i < tail; i++) s[s.size() - 1 - i] *= static_cast<float>(i) / static_cast<float>(tail);
    normalize(s, sound == 0 ? 0.95f : 0.8f);
    return s;
}
}  // namespace

// ---------------------------------------------------------------------------
// Drum kits

DrumKits::DrumKits(double sampleRate) {
    for (int kit = 0; kit < kKits; kit++) {
        for (int sound = 0; sound < kSounds; sound++) {
            samples_[static_cast<size_t>(kit)][static_cast<size_t>(sound)] = makeDrum(sampleRate, kit, sound);
        }
    }
}

int DrumKits::soundForNote(int note) {
    switch (note) {
        case 35: case 36: return 0;           // Kick
        case 38: case 40: return 1;           // Snare
        case 39: return 2;                    // Clap
        case 42: case 44: return 3;           // Closed hat
        case 46: return 4;                    // Open hat
        case 41: case 43: case 45: return 5;  // Low tom
        case 58: case 69: case 70: return 6;  // Shaker
        case 37: return 7;                    // Rim
        default: return -1;
    }
}

bool isDrumKit(int instrument) {
    return instrument >= ROUNDS_INSTRUMENT_DRY_KIT && instrument < ROUNDS_INSTRUMENT_COUNT;
}

// ---------------------------------------------------------------------------
// Synth presets

struct Player::Preset {
    int wave;            // 0 sine, 1 saw, 2 square, 3 triangle, 4 FM electric piano
    float detuneCents;   // second oscillator, 0 = none
    float sub;           // sine an octave down
    float attack, decay, sustain, release;   // seconds, except sustain (level)
    float cutoff;        // Hz, 0 = no filter
    float envAmount;     // Hz added at the filter envelope's peak
    float filterDecay;   // seconds
    float resonance;     // 0..1
    float keyTrack;      // how far the cutoff follows the note (1 = fully)
    bool mono;
    float glide;         // seconds (mono only)
    float level;
    float drive;
};

namespace {
// Constant-initialized, so the audio thread never runs a static initializer.
constexpr Player::Preset kPresets[] = {
    // wave detune sub  att    dec   sus   rel    cutoff env    fdec  res   keytr mono   glide level drive
    {0, 0.0f, 0.0f, 0.004f, 0.4f, 0.85f, 0.08f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, true, 0.0f, 0.6f, 1.6f},       // Sub Bass
    {1, 0.0f, 0.4f, 0.002f, 1.2f, 0.0f, 0.07f, 320.0f, 2200.0f, 0.12f, 0.15f, 0.3f, true, 0.0f, 0.55f, 1.2f},  // Finger Bass
    {1, 7.0f, 0.0f, 0.002f, 0.35f, 0.6f, 0.06f, 160.0f, 2600.0f, 0.22f, 0.65f, 0.2f, true, 0.0f, 0.42f, 1.5f}, // Synth Bass
    {4, 0.0f, 0.0f, 0.002f, 4.0f, 0.0f, 0.35f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, false, 0.0f, 0.32f, 1.0f},       // E-Piano
    {1, 12.0f, 0.0f, 0.5f, 1.0f, 0.9f, 1.2f, 900.0f, 700.0f, 1.5f, 0.2f, 0.4f, false, 0.0f, 0.16f, 1.0f},     // Warm Pad
    {2, 5.0f, 0.0f, 0.008f, 0.3f, 0.8f, 0.12f, 1800.0f, 2200.0f, 0.3f, 0.35f, 0.5f, true, 0.05f, 0.24f, 1.3f}, // Mono Lead
};
static_assert(sizeof(kPresets) / sizeof(kPresets[0]) == ROUNDS_INSTRUMENT_MONO_LEAD + 1, "one preset per synth");
}  // namespace

const Player::Preset& Player::preset() const {
    return kPresets[std::clamp(instrument_, 0, static_cast<int>(ROUNDS_INSTRUMENT_MONO_LEAD))];
}

namespace {
float noteHz(int note) { return 440.0f * std::pow(2.0f, (static_cast<float>(note) - 69.0f) / 12.0f); }

// Per-sample multiplier that decays by 1/e every `seconds / 4` (about -35 dB over `seconds`).
float decayCoef(double sampleRate, float seconds) {
    if (seconds <= 0.0f) return 0.0f;
    return std::exp(-4.0f / (seconds * static_cast<float>(sampleRate)));
}
}  // namespace

// ---------------------------------------------------------------------------
// Player

Player::Player(double sampleRate, const DrumKits* kits) : sampleRate_(sampleRate), kits_(kits) {}

void Player::setInstrument(int instrument) {
    if (instrument < 0 || instrument >= ROUNDS_INSTRUMENT_COUNT || instrument == instrument_) return;
    releaseAll();
    // Synth voices still ringing out would switch sound mid-release: cut them.
    for (Voice& v : voices_) v.active = false;
    instrument_ = instrument;
}

Player::Voice* Player::allocate() {
    Voice* best = nullptr;
    for (Voice& v : voices_) {
        if (!v.active) return &v;
    }
    // Steal the oldest released voice, else the oldest.
    for (Voice& v : voices_) {
        if (!v.held && (!best || v.age < best->age)) best = &v;
    }
    if (best) return best;
    for (Voice& v : voices_) {
        if (!best || v.age < best->age) best = &v;
    }
    return best;
}

void Player::startSynth(Voice& v, int note, int velocity, bool fromLoop) {
    const Preset& p = preset();
    v.active = true;
    v.held = true;
    v.fromLoop = fromLoop;
    v.note = note;
    v.velocity = static_cast<float>(velocity) / 127.0f;
    v.age = ++counter_;
    v.target = noteHz(note);
    if (!(p.mono && v.freq > 0.0f && p.glide > 0.0f && v.env > 0.0f)) v.freq = v.target;
    v.phase[0] = v.phase[1] = v.phase[2] = 0.0f;
    v.env = 0.0f;
    v.stage = 0;
    v.filterEnv = 1.0f;
    v.modEnv = 1.0f;
    v.ic1 = v.ic2 = 0.0f;
    v.sound = -1;
}

void Player::release(Voice& v) {
    v.held = false;
    if (v.sound < 0) v.stage = 2;
}

void Player::noteOn(int note, int velocity, bool fromLoop) {
    if (velocity <= 0) {
        noteOff(note, fromLoop);
        return;
    }
    if (isDrumKit(instrument_)) {
        const int sound = DrumKits::soundForNote(note);
        if (sound < 0) return;
        if (sound == 3) {
            // A closed hat chokes a ringing open hat.
            for (Voice& v : voices_) {
                if (v.active && v.sound == 4) v.choking = true;
            }
        }
        Voice* v = allocate();
        if (!v) return;
        *v = Voice{};
        v->active = true;
        v->fromLoop = fromLoop;
        v->note = note;
        v->sound = sound;
        v->age = ++counter_;
        const float vel = static_cast<float>(velocity) / 127.0f;
        v->gain = std::pow(vel, 1.3f);
        return;
    }

    if (preset().mono) {
        // Last-note priority: a new key glides (or jumps) without retriggering
        // while another is still held.
        for (int i = 0; i < heldCount_; i++) {
            if (heldNotes_[static_cast<size_t>(i)] == note) {
                for (int j = i; j < heldCount_ - 1; j++) heldNotes_[static_cast<size_t>(j)] = heldNotes_[static_cast<size_t>(j + 1)];
                heldCount_--;
                break;
            }
        }
        if (heldCount_ < static_cast<int>(heldNotes_.size())) heldNotes_[static_cast<size_t>(heldCount_++)] = note;
        Voice& v = voices_[0];
        if (v.active && v.held) {
            v.note = note;
            v.target = noteHz(note);
            if (preset().glide <= 0.0f) v.freq = v.target;
            v.fromLoop = fromLoop;
            v.velocity = std::max(v.velocity, static_cast<float>(velocity) / 127.0f);
            return;
        }
        startSynth(v, note, velocity, fromLoop);
        return;
    }

    Voice* v = allocate();
    if (v) startSynth(*v, note, velocity, fromLoop);
}

void Player::noteOff(int note, bool fromLoop) {
    if (isDrumKit(instrument_)) return;  // one-shots ring out
    if (preset().mono) {
        for (int i = 0; i < heldCount_; i++) {
            if (heldNotes_[static_cast<size_t>(i)] == note) {
                for (int j = i; j < heldCount_ - 1; j++) heldNotes_[static_cast<size_t>(j)] = heldNotes_[static_cast<size_t>(j + 1)];
                heldCount_--;
                break;
            }
        }
        Voice& v = voices_[0];
        if (!v.active || v.note != note) return;
        if (heldCount_ > 0) {
            v.note = heldNotes_[static_cast<size_t>(heldCount_ - 1)];
            v.target = noteHz(v.note);
            if (preset().glide <= 0.0f) v.freq = v.target;
        } else {
            release(v);
        }
        return;
    }
    for (Voice& v : voices_) {
        if (v.active && v.held && v.note == note && v.fromLoop == fromLoop) release(v);
    }
}

void Player::releaseLoopNotes() {
    for (Voice& v : voices_) {
        if (v.active && v.held && v.fromLoop) release(v);
    }
    if (preset().mono && voices_[0].fromLoop && !voices_[0].held) heldCount_ = 0;
}

void Player::releaseAll() {
    for (Voice& v : voices_) {
        if (v.active && v.held) release(v);
    }
    heldCount_ = 0;
}

void Player::renderDrum(Voice& v, float* out, int frames, float gain) {
    const std::vector<float>& sample = kits_->sample(instrument_ - ROUNDS_INSTRUMENT_DRY_KIT, v.sound);
    const int64_t length = static_cast<int64_t>(sample.size());
    const float chokeCoef = decayCoef(sampleRate_, 0.02f);
    for (int i = 0; i < frames; i++) {
        if (v.pos >= length || v.fade < 1e-4f) {
            v.active = false;
            return;
        }
        out[i] += sample[static_cast<size_t>(v.pos++)] * v.gain * v.fade * gain;
        if (v.choking) v.fade *= chokeCoef;
    }
}

void Player::renderVoice(Voice& v, float* out, int frames, float gain) {
    const Preset& p = preset();
    const float sr = static_cast<float>(sampleRate_);
    const float attackStep = p.attack > 0.0f ? 1.0f / (p.attack * sr) : 1.0f;
    const float decay = decayCoef(sampleRate_, p.decay);
    const float rel = decayCoef(sampleRate_, p.release);
    const float filterDecay = decayCoef(sampleRate_, p.filterDecay);
    const float modDecay = decayCoef(sampleRate_, 1.6f);
    const float glide = p.mono && p.glide > 0.0f ? 1.0f - std::exp(-1.0f / (p.glide * sr)) : 1.0f;
    const float detune = std::pow(2.0f, p.detuneCents / 1200.0f);
    const float velGain = 0.35f + 0.65f * v.velocity;
    const float drive = std::tanh(p.drive);

    for (int i = 0; i < frames; i++) {
        // Envelopes.
        if (v.stage == 0) {
            v.env += attackStep;
            if (v.env >= 1.0f) {
                v.env = 1.0f;
                v.stage = 1;
            }
        } else if (v.stage == 1) {
            v.env = p.sustain + (v.env - p.sustain) * decay;
            if (p.sustain <= 0.0f && v.env < 1e-4f) {
                v.active = false;
                return;
            }
        } else {
            v.env *= rel;
            if (v.env < 1e-4f) {
                v.active = false;
                return;
            }
        }
        v.filterEnv *= filterDecay;
        v.modEnv *= modDecay;
        v.freq += (v.target - v.freq) * glide;

        // Oscillators.
        const float dt = v.freq / sr;
        float x = 0.0f;
        switch (p.wave) {
            case 0: x = std::sin(kTwoPi * v.phase[0]); break;
            case 1: x = 2.0f * v.phase[0] - 1.0f - polyBlep(v.phase[0], dt); break;
            case 2: {
                const float t2 = std::fmod(v.phase[0] + 0.5f, 1.0f);
                x = (v.phase[0] < 0.5f ? 1.0f : -1.0f) + polyBlep(v.phase[0], dt) - polyBlep(t2, dt);
                break;
            }
            case 3: x = 4.0f * std::fabs(v.phase[0] - 0.5f) - 1.0f; break;
            case 4: {
                // Two-operator FM: the modulator's depth fades as the note
                // sustains, plus a faint bell partial on the attack.
                const float index = v.modEnv * (1.2f + 2.0f * v.velocity);
                x = std::sin(kTwoPi * v.phase[0] + index * std::sin(kTwoPi * v.phase[1]));
                x += 0.12f * v.modEnv * v.modEnv * std::sin(kTwoPi * v.phase[2]);
                break;
            }
            default: break;
        }
        if (p.detuneCents > 0.0f && p.wave != 4) {
            const float dt2 = dt * detune;
            const float t = v.phase[1];
            const float x2 = p.wave == 2 ? (t < 0.5f ? 1.0f : -1.0f) + polyBlep(t, dt2) - polyBlep(std::fmod(t + 0.5f, 1.0f), dt2)
                                          : 2.0f * t - 1.0f - polyBlep(t, dt2);
            x = 0.5f * (x + x2);
            v.phase[1] += dt2;
            if (v.phase[1] >= 1.0f) v.phase[1] -= 1.0f;
        }
        if (p.sub > 0.0f) {
            x += p.sub * std::sin(kTwoPi * v.phase[2]);
        }

        v.phase[0] += dt;
        if (v.phase[0] >= 1.0f) v.phase[0] -= 1.0f;
        if (p.wave == 4) {
            v.phase[1] += dt;
            if (v.phase[1] >= 1.0f) v.phase[1] -= 1.0f;
            v.phase[2] += dt * 4.0f;
            if (v.phase[2] >= 1.0f) v.phase[2] -= 1.0f;
        } else if (p.sub > 0.0f) {
            v.phase[2] += dt * 0.5f;
            if (v.phase[2] >= 1.0f) v.phase[2] -= 1.0f;
        }

        // Filter, refreshed every 16 samples.
        if (p.cutoff > 0.0f) {
            if ((i & 15) == 0) {
                const float track = std::pow(2.0f, p.keyTrack * (static_cast<float>(v.note) - 48.0f) / 12.0f);
                const float fc = p.cutoff * track + p.envAmount * v.filterEnv * (0.5f + 0.5f * v.velocity);
                v.g = coefficient(sampleRate_, fc);
                v.k = 2.0f - 1.9f * p.resonance;
            }
            Svf f;
            f.ic1 = v.ic1;
            f.ic2 = v.ic2;
            f.run(x, v.g, v.k);
            v.ic1 = f.ic1;
            v.ic2 = f.ic2;
            x = f.lp;
        }

        out[i] += std::tanh(x * p.drive) / drive * v.env * velGain * p.level * gain;
    }
}

void Player::render(float* out, int frames, float gain, const NoteEvent* events, int count) {
    int pos = 0;
    int e = 0;
    while (pos < frames) {
        while (e < count && events[e].offset <= pos) {
            noteOn(events[e].note, events[e].velocity, true);
            e++;
        }
        const int end = e < count ? std::min(frames, events[e].offset) : frames;
        for (Voice& v : voices_) {
            if (!v.active) continue;
            if (v.sound >= 0) {
                renderDrum(v, out + pos, end - pos, gain);
            } else {
                renderVoice(v, out + pos, end - pos, gain);
            }
        }
        pos = end;
    }
    while (e < count) noteOn(events[e].note, events[e].velocity, true), e++;
}

}  // namespace rounds
