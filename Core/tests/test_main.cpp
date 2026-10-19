// Rounds core tests, including the timing harness.
//
// The harness simulates an audio interface with known input and output
// latency (and optionally a loopback cable), plays clicks into the looper and
// measures, in samples, when they come back out of the "speaker". Timing
// problems show up here as numbers instead of "it sounds a bit off".

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "engine.h"
#include "footswitch.h"

using rounds::Engine;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, ...)                                       \
    do {                                                       \
        g_checks++;                                            \
        if (!(cond)) {                                         \
            g_failures++;                                      \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            std::printf("       ");                            \
            std::printf(__VA_ARGS__);                          \
            std::printf("\n");                                 \
        }                                                      \
    } while (0)

static constexpr double kRate = 48000.0;

// ---------------------------------------------------------------------------
// Simulated interface

struct Sim {
    int inputLatency;
    int outputLatency;
    int block;
    bool loopback = false;
    float noise = 0.0f;
    Engine engine;
    std::vector<float> player;   // what the player plays, by real time
    std::vector<float> speaker;  // what comes out of the speaker, by real time
    std::vector<float> output;   // engine output, by engine clock

    Sim(int in, int out, int blockFrames, int64_t length, double maxLoopSeconds = 20.0)
        : inputLatency(in), outputLatency(out), block(blockFrames), engine(kRate, maxLoopSeconds, blockFrames) {
        player.assign(static_cast<size_t>(length), 0.0f);
        speaker.assign(static_cast<size_t>(length), 0.0f);
        output.assign(static_cast<size_t>(length), 0.0f);
        engine.setLatency(in, out);
    }

    void click(int64_t t, float amplitude = 1.0f) { player[static_cast<size_t>(t)] += amplitude; }

    void run() {
        std::mt19937 rng(1);
        std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
        const int64_t length = static_cast<int64_t>(player.size());
        std::vector<float> in(static_cast<size_t>(block));
        std::vector<float> outL(static_cast<size_t>(block));
        std::vector<float> outR(static_cast<size_t>(block));
        for (int64_t c = 0; c + block <= length; c += block) {
            for (int i = 0; i < block; i++) {
                const int64_t t = c + i - inputLatency;  // real time this input sample was played
                float mic = 0.0f;
                if (t >= 0) {
                    mic = player[static_cast<size_t>(t)];
                    if (loopback) mic += speaker[static_cast<size_t>(t)];
                }
                in[static_cast<size_t>(i)] = mic + noise * dist(rng);
            }
            engine.process(in.data(), outL.data(), outR.data(), block);
            for (int i = 0; i < block; i++) {
                output[static_cast<size_t>(c + i)] = outL[static_cast<size_t>(i)];
                const int64_t heard = c + i + outputLatency;
                if (heard < length) speaker[static_cast<size_t>(heard)] = outL[static_cast<size_t>(i)];
            }
        }
    }

    // First sample above half the click height near `expected`, or INT64_MIN.
    int64_t onsetNear(int64_t expected, int64_t window = 400) const {
        const int64_t from = std::max<int64_t>(0, expected - window);
        const int64_t to = std::min<int64_t>(static_cast<int64_t>(speaker.size()), expected + window);
        for (int64_t t = from; t < to; t++) {
            if (speaker[static_cast<size_t>(t)] > 0.5f) return t;
        }
        return INT64_MIN;
    }
};

static RoundsStatus status(Engine& engine) {
    RoundsStatus s;
    engine.getStatus(&s);
    return s;
}

// ---------------------------------------------------------------------------
// Timing harness

struct TimingResult {
    int measured = 0;
    int missing = 0;
    int64_t worstError = 0;
    int64_t firstError = 0;
    int64_t lastError = 0;
};

// Every click the player played should be heard again exactly one loop
// length later, every cycle, forever.
static TimingResult measureClicks(const Sim& sim, const std::vector<int64_t>& clicks, int64_t loopLength,
                                  int64_t audibleFrom, int64_t until) {
    TimingResult r;
    bool first = true;
    for (int64_t n = 1;; n++) {
        bool any = false;
        for (int64_t click : clicks) {
            const int64_t expected = click + n * loopLength;
            if (expected >= until) continue;
            any = true;
            if (expected < audibleFrom) continue;
            const int64_t onset = sim.onsetNear(expected);
            if (onset == INT64_MIN) {
                r.missing++;
                continue;
            }
            const int64_t error = onset - expected;
            if (first) r.firstError = error;
            first = false;
            r.lastError = error;
            r.measured++;
            if (std::llabs(error) > std::llabs(r.worstError)) r.worstError = error;
        }
        if (!any) break;
    }
    return r;
}

static void testRecordAndPlaybackAlignment() {
    std::printf("Timing: recorded clicks come back on time (latency in/out, block)\n");
    std::printf("  %-22s %8s %8s %8s %8s\n", "in/out/block", "clicks", "missing", "worst", "drift");
    struct Case { int in, out, block; };
    const Case cases[] = {{0, 0, 64}, {200, 300, 128}, {37, 611, 256}, {1024, 1024, 512}, {512, 96, 32}};
    for (const Case& k : cases) {
        const int64_t start = 1000 + 17;      // not block aligned on purpose
        const int64_t loopLength = 48333;
        const int64_t total = start + loopLength * 21;
        Sim sim(k.in, k.out, k.block, total);
        const std::vector<int64_t> clicks = {start + 700, start + 12000, start + 24000, start + 36000};
        for (int64_t t : clicks) sim.click(t);
        sim.engine.send(ROUNDS_CMD_TAP, start);                     // record
        sim.engine.send(ROUNDS_CMD_TAP, start + loopLength);        // -> overdub
        sim.engine.send(ROUNDS_CMD_TAP, start + loopLength + 5000); // -> play
        sim.run();

        const RoundsStatus s = status(sim.engine);
        CHECK(s.loopLength == loopLength, "loop length %lld, expected %lld", (long long)s.loopLength,
              (long long)loopLength);
        const TimingResult r = measureClicks(sim, clicks, loopLength, start + loopLength + k.out, total - 500);
        char label[32];
        std::snprintf(label, sizeof label, "%d/%d/%d", k.in, k.out, k.block);
        std::printf("  %-22s %8d %8d %8lld %8lld\n", label, r.measured, r.missing, (long long)r.worstError,
                    (long long)(r.lastError - r.firstError));
        CHECK(r.missing == 0, "%d clicks missing", r.missing);
        CHECK(r.measured >= 70, "only %d clicks measured", r.measured);
        CHECK(r.worstError == 0, "worst timing error %lld samples", (long long)r.worstError);
    }
}

static void testOverdubAlignment() {
    std::printf("Timing: overdubbed clicks land where they were played\n");
    const int64_t start = 2000;
    const int64_t loopLength = 40000;
    const int64_t total = start + loopLength * 12;
    Sim sim(300, 450, 128, total);
    const std::vector<int64_t> base = {start + 5000};
    sim.click(base[0]);
    sim.engine.send(ROUNDS_CMD_TAP, start);
    sim.engine.send(ROUNDS_CMD_TAP, start + loopLength);
    sim.engine.send(ROUNDS_CMD_TAP, start + loopLength + 1000);
    // Overdub across the loop seam in the third cycle.
    const int64_t open = start + loopLength * 3 + 30000;
    const std::vector<int64_t> dub = {open + 4000, open + 15000};  // second one after the seam
    for (int64_t t : dub) sim.click(t, 1.0f);
    sim.engine.send(ROUNDS_CMD_TAP, open);
    sim.engine.send(ROUNDS_CMD_TAP, open + 20000);
    sim.run();

    const TimingResult rb = measureClicks(sim, base, loopLength, start + loopLength + 450, total - 500);
    const TimingResult rd = measureClicks(sim, dub, loopLength, open + 20000 + 450, total - 500);
    std::printf("  base clicks: %d measured, worst %lld; overdub clicks: %d measured, worst %lld, missing %d\n",
                rb.measured, (long long)rb.worstError, rd.measured, (long long)rd.worstError, rd.missing);
    CHECK(rb.worstError == 0 && rb.missing == 0, "base error %lld missing %d", (long long)rb.worstError, rb.missing);
    CHECK(rd.worstError == 0 && rd.missing == 0, "overdub error %lld missing %d", (long long)rd.worstError,
          rd.missing);
    CHECK(rd.measured >= 14, "only %d overdub clicks measured", rd.measured);
    CHECK(status(sim.engine).layers == 2, "layers %d", status(sim.engine).layers);
}

static void testLongRunDrift() {
    std::printf("Timing: no drift over 10 minutes\n");
    const int64_t start = 4800;
    const int64_t loopLength = 96000 + 7;  // ~2 s, deliberately odd
    const int64_t total = static_cast<int64_t>(kRate * 600);
    Sim sim(256, 256, 256, total);
    const std::vector<int64_t> clicks = {start + 10000, start + 60000};
    for (int64_t t : clicks) sim.click(t);
    sim.engine.send(ROUNDS_CMD_TAP, start);
    sim.engine.send(ROUNDS_CMD_TAP, start + loopLength);
    sim.engine.send(ROUNDS_CMD_TAP, start + loopLength + 100);
    sim.run();
    const TimingResult r = measureClicks(sim, clicks, loopLength, start + loopLength + 256, total - 500);
    std::printf("  %d clicks over %.0f s, worst error %lld, drift %lld samples\n", r.measured, total / kRate,
                (long long)r.worstError, (long long)(r.lastError - r.firstError));
    CHECK(r.worstError == 0 && r.missing == 0, "error %lld missing %d", (long long)r.worstError, r.missing);
    CHECK(r.measured > 590, "measured %d", r.measured);
}

static void testSeamIsSmooth() {
    std::printf("Seam: a continuous sine has no click at the loop point\n");
    const int64_t start = 1000;
    const int64_t loopLength = 48111;
    const int64_t total = start + loopLength * 4;
    Sim sim(128, 128, 128, total);
    const double freq = 220.0;
    for (int64_t t = start - 500; t < start + loopLength + 2000; t++) {
        sim.player[static_cast<size_t>(t)] = 0.5f * static_cast<float>(std::sin(2.0 * M_PI * freq * t / kRate));
    }
    sim.engine.send(ROUNDS_CMD_TAP, start);
    sim.engine.send(ROUNDS_CMD_TAP, start + loopLength);
    sim.engine.send(ROUNDS_CMD_TAP, start + loopLength + 4000);
    sim.run();

    const double normalStep = 0.5 * 2.0 * M_PI * freq / kRate;
    // Loop index 0 is heard at start + n * loopLength; look at the 3rd seam.
    const int64_t seam = start + loopLength * 3;
    double worst = 0.0;
    for (int64_t t = seam - 600; t < seam + 600; t++) {
        worst = std::max(worst, std::fabs(double(sim.speaker[size_t(t)]) - double(sim.speaker[size_t(t - 1)])));
    }
    std::printf("  biggest sample-to-sample jump at the seam: %.4f (a clean sine: %.4f)\n", worst, normalStep);
    CHECK(worst < normalStep * 1.5, "seam jump %.4f vs %.4f", worst, normalStep);
}

static void testCalibration() {
    std::printf("Calibration: loopback measurement finds the round trip\n");
    struct Case { int in, out, block; };
    const Case cases[] = {{180, 260, 128}, {700, 900, 512}, {40, 40, 32}};
    for (const Case& k : cases) {
        const int64_t total = static_cast<int64_t>(kRate * 5);
        Sim sim(k.in, k.out, k.block, total);
        sim.loopback = true;
        sim.noise = 0.002f;
        sim.engine.setLatency(100, 100);  // a wrong estimate to be corrected
        sim.engine.startCalibration();
        sim.run();
        RoundsEvent ev;
        int64_t measured = -2;
        while (sim.engine.pollEvent(&ev)) {
            if (ev.type == ROUNDS_EVENT_CALIBRATION_DONE) measured = ev.value;
        }
        const RoundsStatus s = status(sim.engine);
        std::printf("  actual %d, measured %lld, now using in %d / out %d\n", k.in + k.out, (long long)measured,
                    s.inputLatency, s.outputLatency);
        CHECK(measured == k.in + k.out, "measured %lld expected %d", (long long)measured, k.in + k.out);
        CHECK(s.inputLatency + s.outputLatency == k.in + k.out, "latency not applied");
    }
}

// ---------------------------------------------------------------------------
// Behavior

static void testFootswitchGestures() {
    std::printf("Footswitch: tap, double-tap, hold, long hold\n");
    rounds::Footswitch fs(rounds_footswitch_default_config());
    CHECK(fs.press(0.00) == ROUNDS_GESTURE_TAP, "first press");
    CHECK(fs.release(0.08) == ROUNDS_GESTURE_NONE, "release");
    CHECK(fs.press(0.20) == ROUNDS_GESTURE_DOUBLE_TAP, "second quick press");
    CHECK(fs.release(0.26) == ROUNDS_GESTURE_NONE, "release");
    CHECK(fs.press(0.40) == ROUNDS_GESTURE_TAP, "third quick press is a new tap");
    CHECK(fs.release(0.45) == ROUNDS_GESTURE_NONE, "release");
    CHECK(fs.press(1.00) == ROUNDS_GESTURE_TAP, "slow press");
    CHECK(fs.tick(1.50) == ROUNDS_GESTURE_NONE, "not yet a hold");
    CHECK(fs.tick(1.61) == ROUNDS_GESTURE_HOLD, "hold");
    CHECK(fs.tick(2.00) == ROUNDS_GESTURE_NONE, "hold fires once");
    CHECK(fs.tick(3.01) == ROUNDS_GESTURE_LONG_HOLD, "long hold");
    CHECK(fs.release(3.20) == ROUNDS_GESTURE_NONE, "release after long hold");
    CHECK(fs.press(3.30) == ROUNDS_GESTURE_TAP, "press right after a hold is a tap");
    CHECK(fs.release(3.35) == ROUNDS_GESTURE_NONE, "release");
    CHECK(fs.press(5.00) == ROUNDS_GESTURE_TAP, "press");
    CHECK(fs.release(5.70) == ROUNDS_GESTURE_HOLD, "hold caught on release between ticks");
    CHECK(fs.press(5.80) == ROUNDS_GESTURE_TAP, "repeat");
    CHECK(fs.press(5.81) == ROUNDS_GESTURE_NONE, "duplicate down ignored");
}

// Runs the engine on silence and applies commands at the given times.
struct Driver {
    Engine engine{kRate, 10.0, 128};
    int64_t now = 0;
    void advance(int64_t samples) {
        std::vector<float> in(128, 0.0f), l(128), r(128);
        for (int64_t done = 0; done < samples; done += 128) engine.process(in.data(), l.data(), r.data(), 128);
        now += ((samples + 127) / 128) * 128;
    }
    void send(RoundsCommand cmd) {
        engine.send(cmd, -1);
        advance(128);
    }
    RoundsStatus s() { return status(engine); }
};

static void testGestureCommands() {
    std::printf("Gestures: what double-tap and hold do in each state\n");
    {
        Driver d;
        d.send(ROUNDS_CMD_TAP);
        CHECK(d.s().state == ROUNDS_LOOP_RECORDING, "recording");
        d.send(ROUNDS_CMD_UNDO_REVERTING_TAP);
        CHECK(d.s().state == ROUNDS_LOOP_EMPTY, "double-tap at record start leaves it empty");
    }
    {
        Driver d;
        d.send(ROUNDS_CMD_TAP);
        d.advance(48000);
        d.send(ROUNDS_CMD_TAP);
        CHECK(d.s().state == ROUNDS_LOOP_OVERDUBBING, "record -> overdub (%d)", d.s().state);
        d.advance(4800);
        d.send(ROUNDS_CMD_TAP);
        CHECK(d.s().state == ROUNDS_LOOP_PLAYING && d.s().layers == 1, "overdub -> play, 1 layer");
        d.advance(4800);
        d.send(ROUNDS_CMD_TAP);
        CHECK(d.s().state == ROUNDS_LOOP_OVERDUBBING, "play -> overdub");
        d.send(ROUNDS_CMD_UNDO_REVERTING_TAP);
        CHECK(d.s().state == ROUNDS_LOOP_PLAYING && d.s().layers == 0 && d.s().layerOpen == 0,
              "double-tap from play drops the new layer and undoes the last one (state %d, layers %d)",
              d.s().state, d.s().layers);
        CHECK(d.s().loopLength > 0, "loop kept");
    }
    {
        Driver d;
        d.send(ROUNDS_CMD_TAP);
        d.advance(48000);
        d.send(ROUNDS_CMD_TAP);  // -> overdub (opened by this press)
        d.send(ROUNDS_CMD_STOP_REVERTING_TAP);
        CHECK(d.s().state == ROUNDS_LOOP_STOPPED && d.s().layers == 0, "hold right after finishing a loop stops it");
        d.send(ROUNDS_CMD_TAP);
        CHECK(d.s().state == ROUNDS_LOOP_PLAYING, "tap restarts");
        d.send(ROUNDS_CMD_TAP);
        d.advance(4800);
        d.send(ROUNDS_CMD_TAP);  // overdub closed by this press
        d.send(ROUNDS_CMD_STOP_REVERTING_TAP);
        CHECK(d.s().state == ROUNDS_LOOP_STOPPED && d.s().layers == 1, "hold keeps a layer the tap just finished");
        d.send(ROUNDS_CMD_CLEAR);
        CHECK(d.s().state == ROUNDS_LOOP_EMPTY && d.s().loopLength == 0, "long hold clears");
    }
    {
        Driver d;
        d.send(ROUNDS_CMD_TAP);
        d.send(ROUNDS_CMD_STOP_REVERTING_TAP);
        CHECK(d.s().state == ROUNDS_LOOP_EMPTY, "hold at record start cancels the recording");
        d.send(ROUNDS_CMD_TAP);
        d.advance(1000);
        d.send(ROUNDS_CMD_TAP);
        CHECK(d.s().state == ROUNDS_LOOP_RECORDING, "a tap too soon (<100 ms) doesn't close the loop");
    }
}

static void testManyLayersMerge() {
    std::printf("Layers: past the undo limit, old layers merge into the base without losing audio\n");
    const int64_t start = 1000;
    const int64_t loopLength = 48000;
    const int dubs = 12;
    const int64_t total = start + loopLength * (dubs + 6);
    Sim sim(0, 0, 128, total);
    sim.click(start + 1000);
    sim.engine.send(ROUNDS_CMD_TAP, start);
    sim.engine.send(ROUNDS_CMD_TAP, start + loopLength);
    sim.engine.send(ROUNDS_CMD_TAP, start + loopLength + 500);
    std::vector<int64_t> dubIndexes;
    for (int i = 0; i < dubs; i++) {
        const int64_t open = start + loopLength * (2 + i) + 2000 + i * 3000;
        sim.click(open + 1500);
        dubIndexes.push_back(open + 1500 - start - loopLength * (2 + i));
        sim.engine.send(ROUNDS_CMD_TAP, open);
        sim.engine.send(ROUNDS_CMD_TAP, open + 2500);
    }
    sim.run();
    // Check the last cycle: base click plus all 12 overdub clicks.
    const int64_t cycle = start + loopLength * (dubs + 4);
    int heard = sim.onsetNear(cycle + 1000, 2) != INT64_MIN ? 1 : 0;
    for (int64_t idx : dubIndexes) heard += sim.onsetNear(cycle + idx, 2) != INT64_MIN ? 1 : 0;
    const RoundsStatus s = status(sim.engine);
    std::printf("  heard %d of %d clicks, %d undoable layers\n", heard, dubs + 1, s.layers);
    CHECK(heard == dubs + 1, "heard %d", heard);
    CHECK(s.layers == 8, "undoable layers %d", s.layers);
}

int main() {
    testRecordAndPlaybackAlignment();
    testOverdubAlignment();
    testLongRunDrift();
    testSeamIsSmooth();
    testCalibration();
    testFootswitchGestures();
    testGestureCommands();
    testManyLayersMerge();
    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
