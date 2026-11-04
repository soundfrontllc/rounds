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

static RoundsLoopStatus loopStatus(Engine& engine, int loop = 0) {
    RoundsLoopStatus s;
    engine.getLoopStatus(loop, &s);
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
        sim.engine.send(0, ROUNDS_CMD_TAP, start);                     // record
        sim.engine.send(0, ROUNDS_CMD_TAP, start + loopLength);        // -> play
        sim.run();

        // The base loop is rounded down to a multiple of 8 samples so 1/2x ... 1/8x loops divide it exactly.
        const int64_t expected = loopLength - loopLength % 8;
        const RoundsLoopStatus s = loopStatus(sim.engine);
        CHECK(s.loopLength == expected, "loop length %lld, expected %lld", (long long)s.loopLength,
              (long long)expected);
        const TimingResult r = measureClicks(sim, clicks, expected, start + loopLength + k.out, total - 500);
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
    sim.engine.send(0, ROUNDS_CMD_TAP, start);
    sim.engine.send(0, ROUNDS_CMD_TAP, start + loopLength);
    // Overdub across the loop seam in the third cycle.
    const int64_t open = start + loopLength * 3 + 30000;
    const std::vector<int64_t> dub = {open + 4000, open + 15000};  // second one after the seam
    for (int64_t t : dub) sim.click(t, 1.0f);
    sim.engine.send(0, ROUNDS_CMD_TAP, open);
    sim.engine.send(0, ROUNDS_CMD_TAP, open + 20000);
    sim.run();

    const TimingResult rb = measureClicks(sim, base, loopLength, start + loopLength + 450, total - 500);
    const TimingResult rd = measureClicks(sim, dub, loopLength, open + 20000 + 450, total - 500);
    std::printf("  base clicks: %d measured, worst %lld; overdub clicks: %d measured, worst %lld, missing %d\n",
                rb.measured, (long long)rb.worstError, rd.measured, (long long)rd.worstError, rd.missing);
    CHECK(rb.worstError == 0 && rb.missing == 0, "base error %lld missing %d", (long long)rb.worstError, rb.missing);
    CHECK(rd.worstError == 0 && rd.missing == 0, "overdub error %lld missing %d", (long long)rd.worstError,
          rd.missing);
    CHECK(rd.measured >= 14, "only %d overdub clicks measured", rd.measured);
    CHECK(loopStatus(sim.engine).layers == 1, "layers %d", loopStatus(sim.engine).layers);
}

static void testLongRunDrift() {
    std::printf("Timing: no drift over 10 minutes\n");
    const int64_t start = 4800;
    const int64_t loopLength = 96000 + 7;  // ~2 s, deliberately odd
    const int64_t total = static_cast<int64_t>(kRate * 600);
    Sim sim(256, 256, 256, total);
    const std::vector<int64_t> clicks = {start + 10000, start + 60000};
    for (int64_t t : clicks) sim.click(t);
    sim.engine.send(0, ROUNDS_CMD_TAP, start);
    sim.engine.send(0, ROUNDS_CMD_TAP, start + loopLength);
    sim.run();
    const int64_t expected = loopLength - loopLength % 8;
    CHECK(loopStatus(sim.engine).loopLength == expected, "loop length %lld", (long long)loopStatus(sim.engine).loopLength);
    const TimingResult r = measureClicks(sim, clicks, expected, start + loopLength + 256, total - 500);
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
    sim.engine.send(0, ROUNDS_CMD_TAP, start);
    sim.engine.send(0, ROUNDS_CMD_TAP, start + loopLength);
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
    void send(RoundsCommand cmd, int loop = 0) {
        engine.send(loop, cmd, -1);
        advance(128);
    }
    RoundsLoopStatus s(int loop = 0) { return loopStatus(engine, loop); }
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
        CHECK(d.s().state == ROUNDS_LOOP_PLAYING && d.s().layers == 0, "record -> play (%d)", d.s().state);
        d.advance(4800);
        d.send(ROUNDS_CMD_TAP);
        CHECK(d.s().state == ROUNDS_LOOP_OVERDUBBING, "play -> overdub");
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
        d.send(ROUNDS_CMD_TAP);  // -> play (loop closed by this press)
        d.send(ROUNDS_CMD_STOP_REVERTING_TAP);
        CHECK(d.s().state == ROUNDS_LOOP_STOPPED && d.s().layers == 0, "hold right after finishing a loop stops it");
        d.send(ROUNDS_CMD_TAP);
        CHECK(d.s().state == ROUNDS_LOOP_PLAYING, "tap restarts");
        d.send(ROUNDS_CMD_TAP);  // -> overdub
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
    {
        Driver d;
        d.send(ROUNDS_CMD_TAP);
        d.advance(48000);
        d.send(ROUNDS_CMD_TAP);  // -> play (loop closed by this press)
        d.send(ROUNDS_CMD_UNDO_REVERTING_TAP);
        CHECK(d.s().state == ROUNDS_LOOP_PLAYING && d.s().loopLength > 0,
              "double-tap right after closing the loop keeps it playing (state %d)", d.s().state);
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
    sim.engine.send(0, ROUNDS_CMD_TAP, start);
    sim.engine.send(0, ROUNDS_CMD_TAP, start + loopLength);
    std::vector<int64_t> dubIndexes;
    for (int i = 0; i < dubs; i++) {
        const int64_t open = start + loopLength * (2 + i) + 2000 + i * 3000;
        sim.click(open + 1500);
        dubIndexes.push_back(open + 1500 - start - loopLength * (2 + i));
        sim.engine.send(0, ROUNDS_CMD_TAP, open);
        sim.engine.send(0, ROUNDS_CMD_TAP, open + 2500);
    }
    sim.run();
    // Check the last cycle: base click plus all 12 overdub clicks.
    const int64_t cycle = start + loopLength * (dubs + 4);
    int heard = sim.onsetNear(cycle + 1000, 2) != INT64_MIN ? 1 : 0;
    for (int64_t idx : dubIndexes) heard += sim.onsetNear(cycle + idx, 2) != INT64_MIN ? 1 : 0;
    const RoundsLoopStatus s = loopStatus(sim.engine);
    std::printf("  heard %d of %d clicks, %d undoable layers\n", heard, dubs + 1, s.layers);
    CHECK(heard == dubs + 1, "heard %d", heard);
    CHECK(s.layers == 8, "undoable layers %d", s.layers);
}

// ---------------------------------------------------------------------------
// Several loops on one clock

static void testLoopsSnapToBars() {
    std::printf("Loops: later loops snap to 1, 2, 4, 8 ... bars of the first loop's tempo\n");
    const int64_t base = 192000;  // 4 s: 2 bars at 120 BPM
    const int64_t bar = base / 2;
    struct Case { int loop; double played; int64_t expected; };
    const Case cases[] = {{1, 1.9, 2 * base}, {2, 0.55, bar}, {3, 1.2, base}, {4, 0.3, bar}, {5, 0.8, base}};
    Driver d;
    d.engine.send(0, ROUNDS_CMD_TAP, -1);
    d.advance(base);
    d.send(ROUNDS_CMD_TAP);
    CHECK(status(d.engine).baseLength == base && status(d.engine).baseLoop == 0, "base %lld from loop %d",
          (long long)status(d.engine).baseLength, status(d.engine).baseLoop);
    CHECK(status(d.engine).bars == 2 && status(d.engine).barLength == bar, "taken as %d bars of %lld",
          status(d.engine).bars, (long long)status(d.engine).barLength);
    for (const Case& k : cases) {
        d.send(ROUNDS_CMD_TAP, k.loop);
        d.advance(static_cast<int64_t>(base * k.played) - 128);
        d.send(ROUNDS_CMD_TAP, k.loop);
        std::printf("  played %.2fx the first loop -> %lld bars\n", k.played, (long long)(d.s(k.loop).loopLength / bar));
        CHECK(d.s(k.loop).loopLength == k.expected, "loop %d length %lld, expected %lld", k.loop + 1,
              (long long)d.s(k.loop).loopLength, (long long)k.expected);
    }
    for (int loop = 0; loop < 7; loop++) d.send(ROUNDS_CMD_CLEAR, loop);
    CHECK(status(d.engine).baseLength == 0 && status(d.engine).bars == 0, "tempo resets when every loop is empty");
}

// Three loops of 1x, 2x and 1/2x, recorded one after another at arbitrary
// moments, each with its own click. Every click must come back on time on
// every repeat for 10 minutes, including one played after loop 2 was closed
// (it snapped longer, so recording carried on to the snapped end).
static void testLoopsStayInStep() {
    std::printf("Timing: loops of 1x, 2x and 1/2x stay in step for 10 minutes\n");
    const int64_t L = 192000;  // 2 bars, so 1/2x is one bar
    const int64_t s = 1000;
    const int64_t total = static_cast<int64_t>(kRate * 600);
    Sim sim(200, 300, 128, total);

    sim.click(s + 1000);
    sim.engine.send(0, ROUNDS_CMD_TAP, s);
    sim.engine.send(0, ROUNDS_CMD_TAP, s + L);

    const int64_t t1 = s + L + 5000;
    const int64_t close1 = t1 + L * 19 / 10;  // snaps to 2L
    sim.click(t1 + 20000);
    sim.click(t1 + L * 193 / 100);             // after the tap, before the snapped end
    sim.engine.send(1, ROUNDS_CMD_TAP, t1);
    sim.engine.send(1, ROUNDS_CMD_TAP, close1);

    const int64_t t2 = t1 + 2 * L + 7000;
    const int64_t close2 = t2 + L * 55 / 100;  // snaps to L/2
    sim.click(t2 + 30000);
    sim.engine.send(2, ROUNDS_CMD_TAP, t2);
    sim.engine.send(2, ROUNDS_CMD_TAP, close2);
    sim.run();

    struct Check { const char* name; int loop; std::vector<int64_t> clicks; int64_t length; int64_t from; };
    const Check checks[] = {{"1x", 0, {s + 1000}, L, s + L + 300},
                            {"2x", 1, {t1 + 20000, t1 + L * 193 / 100}, 2 * L, t1 + 2 * L + 300},
                            {"1/2x", 2, {t2 + 30000}, L / 2, close2 + 300}};
    for (const Check& c : checks) {
        CHECK(loopStatus(sim.engine, c.loop).loopLength == c.length, "loop %d length %lld", c.loop + 1,
              (long long)loopStatus(sim.engine, c.loop).loopLength);
        const TimingResult r = measureClicks(sim, c.clicks, c.length, c.from, total - 500);
        std::printf("  %-5s %4d clicks, missing %d, worst %lld, drift %lld samples\n", c.name, r.measured, r.missing,
                    (long long)r.worstError, (long long)(r.lastError - r.firstError));
        CHECK(r.worstError == 0 && r.missing == 0, "%s: error %lld missing %d", c.name, (long long)r.worstError,
              r.missing);
        CHECK(r.measured > 100, "%s: measured %d", c.name, r.measured);
    }
}

// A stopped loop restarted while another plays rejoins in phase; restarted
// alone it starts from the top and every other loop moves with it.
static void testRestartInPhase() {
    std::printf("Loops: restarting keeps loops in step\n");
    const int64_t L = 48000;
    Driver d;
    d.send(ROUNDS_CMD_TAP, 0);
    d.advance(L - 128);
    d.send(ROUNDS_CMD_TAP, 0);
    d.send(ROUNDS_CMD_TAP, 1);
    d.advance(2 * L - 128);
    d.send(ROUNDS_CMD_TAP, 1);  // loop 2 is 2L
    d.advance(30000);
    const int64_t phase = d.s(1).playPosition - d.s(0).playPosition;  // loop 2 relative to loop 1
    d.send(ROUNDS_CMD_STOP, 0);
    d.advance(10000);
    d.send(ROUNDS_CMD_TAP, 0);  // rejoins while loop 2 plays
    const int64_t rejoined = ((d.s(1).playPosition - d.s(0).playPosition) % L + L) % L;
    CHECK(rejoined == ((phase % L) + L) % L, "loop 1 rejoined in phase (%lld vs %lld)", (long long)rejoined,
          (long long)(((phase % L) + L) % L));
    d.send(ROUNDS_CMD_STOP, 0);
    d.send(ROUNDS_CMD_STOP, 1);
    d.advance(70000);
    d.send(ROUNDS_CMD_TAP, 1);  // alone: from the top
    CHECK(d.s(1).playPosition < 1024, "restarted alone from the top (position %lld)", (long long)d.s(1).playPosition);
    d.advance(5000);
    d.send(ROUNDS_CMD_TAP, 0);
    const int64_t now = ((d.s(1).playPosition - d.s(0).playPosition) % L + L) % L;
    CHECK(now == ((phase % L) + L) % L, "loops kept their relative phase (%lld vs %lld)", (long long)now,
          (long long)(((phase % L) + L) % L));
}

// A re-recorded loop that snaps longer than was played must play silence
// until recording reaches the snapped end, not the previous take.
static void testSnappedLongerIsSilent() {
    std::printf("Loops: the not-yet-recorded end of a snapped loop is silent\n");
    const int64_t L = 48000;
    const int64_t total = 12 * L;
    Sim sim(0, 0, 128, total);
    sim.engine.send(0, ROUNDS_CMD_TAP, 1000);
    sim.engine.send(0, ROUNDS_CMD_TAP, 1000 + L);  // silent base loop
    const int64_t take1 = 1000 + L + 3000;
    sim.click(take1 + L * 197 / 100);  // near the end of a 2L take
    sim.engine.send(1, ROUNDS_CMD_TAP, take1);
    sim.engine.send(1, ROUNDS_CMD_TAP, take1 + 2 * L);
    const int64_t take2 = take1 + 3 * L;
    sim.engine.send(1, ROUNDS_CMD_CLEAR, take2 - 100);
    sim.engine.send(1, ROUNDS_CMD_TAP, take2);
    sim.engine.send(1, ROUNDS_CMD_TAP, take2 + L * 19 / 10);  // snaps to 2L
    sim.run();
    float loudest = 0.0f;
    for (int64_t t = take2; t < total; t++) loudest = std::max(loudest, std::fabs(sim.speaker[size_t(t)]));
    std::printf("  loudest sample after the second take: %.3f\n", loudest);
    CHECK(loudest < 0.01f, "old take leaked through: %.3f", loudest);
}

static void testLoopGain() {
    std::printf("Loops: each loop has its own volume\n");
    const int64_t L = 48000;
    Sim sim(0, 0, 128, 4 * L);
    sim.click(2000);
    sim.engine.setLoopGain(0, 0.5f);
    sim.engine.send(0, ROUNDS_CMD_TAP, 1000);
    sim.engine.send(0, ROUNDS_CMD_TAP, 1000 + L);
    sim.run();
    const float heard = sim.speaker[size_t(2000 + L)];
    std::printf("  click at gain 0.5 comes back at %.3f\n", heard);
    CHECK(std::fabs(heard - 0.5f) < 1e-4f, "heard %.3f", heard);
    CHECK(std::fabs(loopStatus(sim.engine).gain - 0.5f) < 1e-6f, "gain reported");
}

// ---------------------------------------------------------------------------
// Session save/load

// Runs an engine block by block with clicks on its input, keeping its output.
struct Rig {
    Engine engine{kRate, 10.0, 128};
    std::vector<int64_t> clicks;  // engine clock times of input clicks
    std::vector<float> out;
    int64_t now = 0;
    void run(int64_t samples) {
        std::vector<float> in(128), l(128), r(128);
        for (int64_t done = 0; done < samples; done += 128) {
            for (int i = 0; i < 128; i++) {
                in[size_t(i)] = 0.0f;
                for (int64_t c : clicks) if (c == now + i) in[size_t(i)] = 1.0f;
            }
            engine.process(in.data(), l.data(), r.data(), 128);
            out.insert(out.end(), l.begin(), l.end());
            now += 128;
        }
    }
    // Runs until the transfer finishes; returns its final status.
    int finish() {
        for (int blocks = 0; blocks < 5000; blocks++) {
            const int status = engine.transferStatus();
            if (status != 1) return status;
            run(128);
        }
        return 1;
    }
    std::vector<int64_t> onsets(int64_t from, int64_t to) const {
        std::vector<int64_t> found;
        for (int64_t t = std::max<int64_t>(from, 1); t < to && t < int64_t(out.size()); t++) {
            if (out[size_t(t)] > 0.5f && out[size_t(t - 1)] <= 0.5f) found.push_back(t);
        }
        return found;
    }
};

static int64_t mod(int64_t a, int64_t m) { return ((a % m) + m) % m; }

static void testSaveAndLoad() {
    std::printf("Sessions: saved loops load back sample-exact and in step\n");
    const int64_t L = 48000;
    Rig a;
    // Loop 1 is the base (L), loop 2 is 2L recorded later at an odd offset,
    // so the two loops' own starts are out of line with each other.
    a.clicks = {1000 + 5000, 1000 + L + 7777 + 30000};
    a.engine.send(0, ROUNDS_CMD_TAP, 1000);
    a.engine.send(0, ROUNDS_CMD_TAP, 1000 + L);
    a.engine.send(1, ROUNDS_CMD_TAP, 1000 + L + 7777);
    a.engine.send(1, ROUNDS_CMD_TAP, 1000 + 3 * L + 7777);
    a.run(6 * L);
    const auto heardA = a.onsets(4 * L, 6 * L);
    CHECK(heardA.size() == 3, "original plays 3 clicks in 2L (got %zu)", heardA.size());

    std::vector<std::vector<float>> saved(2, std::vector<float>(size_t(a.engine.maxLoopSamples())));
    int64_t lengths[2] = {0, 0};
    for (int loop = 0; loop < 2; loop++) {
        CHECK(a.engine.beginExport(loop), "export %d started", loop);
        CHECK(a.finish() == 2, "export %d finished", loop);
        lengths[loop] = a.engine.takeExport(saved[size_t(loop)].data(), a.engine.maxLoopSamples());
    }
    CHECK(lengths[0] == L && lengths[1] == 2 * L, "saved lengths %lld, %lld", (long long)lengths[0],
          (long long)lengths[1]);
    CHECK(saved[0][5000] > 0.5f, "loop 1's click is saved at its place in the loop");
    CHECK(!a.engine.beginExport(5) || a.finish() == -1, "saving an empty loop fails");

    Rig b;
    b.run(12345);  // a different clock from the engine that saved them
    for (int loop = 0; loop < 2; loop++) {
        CHECK(b.engine.beginImport(loop, saved[size_t(loop)].data(), lengths[loop]), "import %d started", loop);
        b.engine.send(loop, ROUNDS_CMD_TAP, -1);  // ignored while loading
        CHECK(b.finish() == 2, "import %d finished", loop);
        CHECK(loopStatus(b.engine, loop).state == ROUNDS_LOOP_STOPPED, "loop %d loads stopped", loop);
    }
    CHECK(status(b.engine).baseLoop == 0 && status(b.engine).baseLength == L, "base is loop 1 again (loop %d)",
          status(b.engine).baseLoop);
    b.engine.send(1, ROUNDS_CMD_TAP, -1);  // start loop 2 first, then loop 1 joins
    b.run(L / 3);
    b.engine.send(0, ROUNDS_CMD_TAP, -1);
    const int64_t from = b.now + 2 * L;
    b.run(5 * L);
    const auto heardB = b.onsets(from, from + 2 * L);
    CHECK(heardB.size() == 3, "loaded session plays 3 clicks in 2L (got %zu)", heardB.size());
    if (heardA.size() == 3 && heardB.size() == 3) {
        // Same pattern, only shifted in time: every gap between clicks matches.
        int64_t worst = INT64_MAX;
        for (size_t k = 0; k < 3; k++) {  // try lining up each loaded click with the first original one
            const int64_t shift = heardB[k] - heardA[0];
            int64_t error = 0;
            for (size_t i = 0; i < 3; i++) {
                int64_t best = INT64_MAX;
                for (size_t j = 0; j < 3; j++) best = std::min<int64_t>(best, std::llabs(mod(heardB[j] - heardA[i] - shift + L, 2 * L) - L));
                error = std::max(error, best);
            }
            worst = std::min(worst, error);
        }
        std::printf("  worst click offset after reload: %lld samples\n", (long long)worst);
        CHECK(worst == 0, "loops reloaded out of step by %lld samples", (long long)worst);
    }
    float peak = 0.0f;
    for (int64_t t = from; t < from + 2 * L; t++) peak = std::max(peak, b.out[size_t(t)]);
    CHECK(std::fabs(peak - 1.0f) < 1e-6f, "click level unchanged (%.6f)", peak);
}

// ---------------------------------------------------------------------------
// Tempo, bar grid and quantize

static void testTempoFromFirstLoop() {
    std::printf("Tempo: the first loop is taken as 1, 2, 4 or 8 bars at 80-160 BPM\n");
    struct Case { double seconds; int bars; };
    const Case cases[] = {{2.0, 1}, {4.0, 2}, {3.0, 1}, {2.9, 1}, {6.0, 2}, {10.0, 4}, {1.2, 1}, {30.0, 8}};
    for (const Case& c : cases) {
        const int bars = Engine::chooseBars(static_cast<int64_t>(c.seconds * kRate), kRate);
        std::printf("  %4.1f s -> %d bar%s, %.1f BPM\n", c.seconds, bars, bars == 1 ? "" : "s", 240.0 * bars / c.seconds);
        CHECK(bars == c.bars, "%.1f s: %d bars, expected %d", c.seconds, bars, c.bars);
    }
    Driver d;
    d.send(ROUNDS_CMD_TAP);
    d.advance(192000 - 128);
    d.send(ROUNDS_CMD_TAP);
    CHECK(status(d.engine).bars == 2, "4 s is 2 bars");
    d.engine.setBars(4);
    d.advance(128);
    CHECK(status(d.engine).bars == 4 && status(d.engine).barLength == 48000, "doubled to 4 bars of %lld",
          (long long)status(d.engine).barLength);
    d.engine.setBars(3);
    d.advance(128);
    CHECK(status(d.engine).bars == 4, "3 bars is refused");
}

// Driver with quantize on, a 2-bar base loop playing; helpers to move to a
// point in the bar.
struct Grid : Driver {
    int64_t bar = 0;
    Grid() {
        engine.setQuantize(true);
        send(ROUNDS_CMD_TAP);
        advance(192000 - 128);
        send(ROUNDS_CMD_TAP);
        bar = status(engine).barLength;
    }
    int64_t inBar() { return status(engine).cyclePosition % bar; }
    // Advances to `fraction` of the way through the next bar (or this one if still ahead).
    void to(double fraction) {
        const int64_t target = static_cast<int64_t>(fraction * bar);
        int64_t ahead = target - inBar();
        if (ahead < 128) ahead += bar;
        advance(ahead - 128);  // send() runs one more block
    }
};

static void testQuantizedActions() {
    std::printf("Quantize: record, play and stop wait for the next bar\n");
    Grid g;
    CHECK(g.bar == 96000, "bar %lld", (long long)g.bar);

    g.to(0.4);
    g.send(ROUNDS_CMD_TAP, 1);
    CHECK(g.s(1).state == ROUNDS_LOOP_EMPTY && g.s(1).queued == ROUNDS_QUEUED_TAP, "mid-bar record is queued");
    g.advance(g.bar / 2);
    CHECK(g.s(1).state == ROUNDS_LOOP_EMPTY, "still waiting before the bar line");
    g.advance(g.bar / 5);
    CHECK(g.s(1).state == ROUNDS_LOOP_RECORDING && g.s(1).queued == 0, "recording from the bar line");
    CHECK(g.s(1).recordedLength == g.inBar(), "recording started exactly on the bar line (%lld vs %lld)",
          (long long)g.s(1).recordedLength, (long long)g.inBar());

    g.to(0.03);
    g.send(ROUNDS_CMD_TAP, 2);
    CHECK(g.s(2).state == ROUNDS_LOOP_RECORDING, "a tap just after the bar line starts at once");
    CHECK(g.s(2).recordedLength == g.inBar(), "... from that bar line (%lld vs %lld)",
          (long long)g.s(2).recordedLength, (long long)g.inBar());

    g.to(0.5);
    g.send(ROUNDS_CMD_TAP, 3);
    g.send(ROUNDS_CMD_UNDO_REVERTING_TAP, 3);
    CHECK(g.s(3).queued == 0, "double-tap takes a queued record back");
    g.send(ROUNDS_CMD_TAP, 4);
    g.send(ROUNDS_CMD_STOP, 4);
    CHECK(g.s(4).queued == 0, "stop takes a queued record back");
    g.send(ROUNDS_CMD_TAP, 5);
    g.send(ROUNDS_CMD_TAP, 5);
    CHECK(g.s(5).queued == 0, "a second tap takes a queued record back");
    g.advance(g.bar);
    CHECK(g.s(3).state == ROUNDS_LOOP_EMPTY && g.s(4).state == ROUNDS_LOOP_EMPTY && g.s(5).state == ROUNDS_LOOP_EMPTY,
          "cancelled loops never started");

    g.to(0.6);
    g.send(ROUNDS_CMD_STOP, 0);
    CHECK(g.s(0).state == ROUNDS_LOOP_PLAYING && g.s(0).queued == ROUNDS_QUEUED_STOP, "mid-bar stop is queued");
    g.advance(g.bar / 2);
    CHECK(g.s(0).state == ROUNDS_LOOP_STOPPED, "stopped at the bar line");
    g.to(0.25);
    g.send(ROUNDS_CMD_TAP, 0);
    CHECK(g.s(0).queued == ROUNDS_QUEUED_TAP, "restart is queued while others play");
    g.advance(g.bar);
    CHECK(g.s(0).state == ROUNDS_LOOP_PLAYING, "restarted on the bar line");
    CHECK(g.s(0).playPosition % g.bar == g.inBar(), "... in step with the grid (%lld vs %lld)",
          (long long)(g.s(0).playPosition % g.bar), (long long)g.inBar());

    g.to(0.5);
    g.send(ROUNDS_CMD_TAP, 0);                 // the press opens an overdub at once...
    CHECK(g.s(0).state == ROUNDS_LOOP_OVERDUBBING, "overdub isn't quantized");
    g.send(ROUNDS_CMD_STOP_REVERTING_TAP, 0);  // ...and the hold takes it back and stops
    CHECK(g.s(0).queued == ROUNDS_QUEUED_STOP && g.s(0).state == ROUNDS_LOOP_PLAYING && g.s(0).layers == 0,
          "hold drops the overdub and stops on the next bar");
    g.send(ROUNDS_CMD_UNDO, 0);
    CHECK(g.s(0).queued == 0 && g.s(0).state == ROUNDS_LOOP_PLAYING, "undo takes a queued stop back");
}

// Through the simulated interface with latency: a late tap still records the
// downbeat it was meant for, and a queued recording doesn't pick up what was
// played before its bar line.
static void testQuantizedRecordingAudio() {
    std::printf("Quantize: late and early taps record from the right bar line\n");
    const int64_t L = 192000;
    const int64_t bar = L / 2;
    const int64_t s0 = 1000;
    Sim sim(200, 300, 128, s0 + 14 * bar);
    sim.engine.setQuantize(true);
    sim.engine.send(0, ROUNDS_CMD_TAP, s0);
    sim.engine.send(0, ROUNDS_CMD_TAP, s0 + L);  // silent 2-bar base; bars at s0 + n * bar

    const int64_t x = s0 + 3 * bar;
    sim.click(x + 500);                          // played on the downbeat...
    sim.engine.send(1, ROUNDS_CMD_TAP, x + 3000); // ...pressed 3000 samples late
    sim.engine.send(1, ROUNDS_CMD_TAP, x + bar + 500);

    const int64_t z = s0 + 6 * bar;
    sim.click(z - bar / 2 + 1000);               // before the bar line: not recorded
    sim.click(z + 4000);
    sim.engine.send(2, ROUNDS_CMD_TAP, z - bar / 2);
    sim.engine.send(2, ROUNDS_CMD_TAP, z + 2 * bar + 500);
    sim.run();

    const TimingResult late = measureClicks(sim, {x + 500}, bar, x + bar + 1000, s0 + 14 * bar - 500);
    std::printf("  late tap:  %d repeats, missing %d, worst %lld samples\n", late.measured, late.missing,
                (long long)late.worstError);
    CHECK(late.measured >= 9 && late.missing == 0 && late.worstError == 0, "late tap lost its downbeat");
    const TimingResult early = measureClicks(sim, {z + 4000}, 2 * bar, z + 2 * bar + 1000, s0 + 14 * bar - 500);
    std::printf("  early tap: %d repeats, missing %d, worst %lld samples\n", early.measured, early.missing,
                (long long)early.worstError);
    CHECK(early.measured >= 3 && early.missing == 0 && early.worstError == 0, "queued recording out of time");
    float leaked = 0.0f;
    for (int64_t n = 1; n <= 3; n++) {
        const int64_t t = z - bar / 2 + 1000 + n * 2 * bar;
        for (int64_t k = t - 50; k < t + 50; k++) leaked = std::max(leaked, sim.speaker[size_t(k)]);
    }
    CHECK(leaked < 0.01f, "audio from before the bar line was recorded (%.3f)", leaked);
    CHECK(loopStatus(sim.engine, 1).loopLength == bar && loopStatus(sim.engine, 2).loopLength == 2 * bar,
          "lengths %lld, %lld", (long long)loopStatus(sim.engine, 1).loopLength,
          (long long)loopStatus(sim.engine, 2).loopLength);
}

static void testClick() {
    std::printf("Click: every beat is heard on the grid, accented downbeats\n");
    const int64_t L = 192000;
    const int64_t beat = L / 8;
    const int64_t s0 = 1000;
    Sim sim(200, 300, 128, s0 + 3 * L);
    sim.engine.setClickGain(1.0f);
    sim.engine.send(0, ROUNDS_CMD_TAP, s0);
    sim.engine.send(0, ROUNDS_CMD_TAP, s0 + L);
    sim.engine.send(0, ROUNDS_CMD_STOP, s0 + 2 * L + 100);
    sim.run();
    int64_t worst = 0;
    int heard = 0;
    float down = 0.0f, other = 0.0f;
    for (int64_t t = s0 + L + beat; t < s0 + 2 * L; t += beat) {
        int64_t onset = INT64_MIN;
        float peak = 0.0f;
        for (int64_t k = t - 300; k < t + 300; k++) {
            if (onset == INT64_MIN && std::fabs(sim.speaker[size_t(k)]) > 0.05f) onset = k;
            peak = std::max(peak, std::fabs(sim.speaker[size_t(k)]));
        }
        if (onset == INT64_MIN) continue;
        heard++;
        worst = std::max<int64_t>(worst, std::llabs(onset - (t + 1)));  // the sine's first non-zero sample
        (((t - s0) / beat) % 4 == 0 ? down : other) = peak;
    }
    std::printf("  %d beats, worst %lld samples off, downbeat %.2f vs %.2f\n", heard, (long long)worst, down, other);
    CHECK(heard == 7 && worst == 0, "heard %d, worst %lld", heard, (long long)worst);
    CHECK(down > other * 1.4f, "downbeat accented");
    float after = 0.0f;
    for (int64_t t = s0 + 2 * L + 2000; t < s0 + 3 * L; t++) after = std::max(after, std::fabs(sim.speaker[size_t(t)]));
    CHECK(after < 1e-3f, "click stops when nothing plays (%.4f)", after);
}

static void testStartStopAll() {
    std::printf("Play: start/stop all\n");
    Grid g;
    g.send(ROUNDS_CMD_TAP, 1);  // queued to the next bar
    g.advance(g.bar);
    g.advance(2 * g.bar - 128);
    g.send(ROUNDS_CMD_TAP, 1);  // a 4-bar loop, so it is the longest
    CHECK(g.s(1).loopLength == 4 * g.bar, "loop 2 is 4 bars (%lld)", (long long)(g.s(1).loopLength / g.bar));
    g.engine.send(-1, ROUNDS_CMD_START_STOP_ALL, -1);
    g.advance(2 * g.bar);
    CHECK(g.s(0).state == ROUNDS_LOOP_STOPPED && g.s(1).state == ROUNDS_LOOP_STOPPED, "all stopped on a bar line");
    g.advance(5000);
    g.engine.send(-1, ROUNDS_CMD_START_STOP_ALL, -1);
    g.advance(128);
    CHECK(g.s(0).state == ROUNDS_LOOP_PLAYING && g.s(1).state == ROUNDS_LOOP_PLAYING, "all playing");
    CHECK(g.s(1).playPosition < 512 && g.inBar() < 512, "from the top of the longest, on a bar line (%lld, %lld)",
          (long long)g.s(1).playPosition, (long long)g.inBar());
}

// ---------------------------------------------------------------------------
// MIDI loops and instruments (milestone 4)

// Runs the engine with a host clock that counts samples, so notes and taps
// can be stamped with exact times. Keeps the engine's output.
struct Keys {
    Engine engine{kRate, 10.0, 128};
    int64_t now = 0;
    std::vector<float> out;
    Keys(int in = 0, int outLat = 0) { engine.setLatency(in, outLat); }
    void run(int64_t samples) {
        std::vector<float> in(128, 0.0f), l(128), r(128);
        for (int64_t done = 0; done < samples; done += 128) {
            engine.processTimed(in.data(), l.data(), r.data(), 128, static_cast<uint64_t>(now) + 1, 1.0);
            out.insert(out.end(), l.begin(), l.end());
            now += 128;
        }
    }
    // Runs up to sample `t` (a block boundary at or before it).
    void runTo(int64_t t) {
        if (t > now) run(((t - now) / 128) * 128);
    }
    // Pressed at sample t (applied one block later, like the hardware).
    void note(int loop, int instrument, int n, int velocity, int64_t t) {
        runTo(t);
        engine.note(loop, instrument, n, velocity, static_cast<uint64_t>(t) + 1);
    }
    // A footswitch tap pressed at t lands at t + 128.
    void tap(int loop, int64_t t) {
        runTo(t);
        engine.sendAtHostTime(loop, ROUNDS_CMD_TAP, static_cast<uint64_t>(t) + 1);
    }
    void command(int loop, RoundsCommand cmd) {
        engine.send(loop, cmd, -1);
        run(256);
    }
    RoundsLoopStatus s(int loop = 0) { return loopStatus(engine, loop); }
    // First sample at or after `from` where the output starts after silence.
    int64_t onset(int64_t from, int64_t to) const {
        for (int64_t t = std::max<int64_t>(from, 1); t < to && t < int64_t(out.size()); t++) {
            if (std::fabs(out[size_t(t)]) > 1e-4f && std::fabs(out[size_t(t - 1)]) <= 1e-4f) return t;
        }
        return -1;
    }
    float peak(int64_t from, int64_t to) const {
        float p = 0.0f;
        for (int64_t t = std::max<int64_t>(from, 0); t < to && t < int64_t(out.size()); t++) p = std::max(p, std::fabs(out[size_t(t)]));
        return p;
    }
};

static void testMidiLoopTiming() {
    std::printf("MIDI loops: pads record into a loop and play back on the sample\n");
    const int in = 200, outLat = 300;
    Keys k(in, outLat);
    const int64_t start = 10000 + 128;  // the tap lands a block after it is pressed
    k.tap(0, 10000);
    k.note(0, ROUNDS_INSTRUMENT_DRY_KIT, 37, 110, start + 1000);  // rim
    k.note(0, ROUNDS_INSTRUMENT_DRY_KIT, 37, 0, start + 2000);
    k.note(0, ROUNDS_INSTRUMENT_DRY_KIT, 42, 90, start + 24000);  // closed hat
    k.note(0, ROUNDS_INSTRUMENT_DRY_KIT, 42, 0, start + 25000);
    k.tap(0, start + 48000 - 128);
    k.run(256);
    const int64_t L = k.s().loopLength;
    CHECK(k.s().kind == ROUNDS_KIND_MIDI, "became a MIDI loop");
    CHECK(k.s().instrument == ROUNDS_INSTRUMENT_DRY_KIT, "playing the kit it was recorded with");
    CHECK(k.s().notes == 2, "two notes recorded (%d)", k.s().notes);
    CHECK(L == 48000, "length %lld", (long long)L);
    k.run(48000 * 3);
    // A note pressed at real time t is heard at t + L: written outputLatency earlier.
    for (int cycle = 1; cycle <= 2; cycle++) {
        const int64_t rim = start + 1000 + cycle * L - outLat;
        const int64_t hat = start + 24000 + cycle * L - outLat;
        const int64_t gotRim = k.onset(rim - 2000, rim + 2000);
        const int64_t gotHat = k.onset(hat - 2000, hat + 2000);
        CHECK(gotRim == rim, "cycle %d: rim off by %lld samples", cycle, (long long)(gotRim - rim));
        CHECK(gotHat == hat, "cycle %d: hat off by %lld samples", cycle, (long long)(gotHat - hat));
    }
}

static void testMidiOverdubAndUndo() {
    std::printf("MIDI loops: overdub a layer, undo it\n");
    Keys k;
    k.tap(0, 1000);
    const int64_t start = 1128;
    k.note(0, ROUNDS_INSTRUMENT_DRY_KIT, 37, 100, start + 500);
    k.tap(0, start + 48000 - 128);
    k.run(256);
    const int64_t L = k.s().loopLength;
    // Overdub: a hat at a quarter of the loop.
    k.tap(0, start + L + 1000);
    k.run(256);
    CHECK(k.s().state == ROUNDS_LOOP_OVERDUBBING, "overdubbing");
    k.note(0, ROUNDS_INSTRUMENT_DRY_KIT, 42, 100, start + L + 12000);
    k.note(0, ROUNDS_INSTRUMENT_DRY_KIT, 42, 0, start + L + 13000);
    k.tap(0, start + L + 30000);
    k.run(256);
    CHECK(k.s().notes == 2 && k.s().layers == 1, "one layer with one note (%d notes, %d layers)", k.s().notes, k.s().layers);
    k.runTo(start + 3 * L);
    const int64_t hat = start + 12000 + 3 * L;
    k.run(L);
    CHECK(k.onset(hat - 1000, hat + 1000) == hat, "overdubbed note plays where it was played");
    k.command(0, ROUNDS_CMD_UNDO);
    CHECK(k.s().notes == 1 && k.s().layers == 0, "undo removes the layer's note (%d)", k.s().notes);
    k.runTo(start + 5 * L);
    k.run(L);
    const int64_t hat5 = start + 12000 + 5 * L;
    CHECK(k.peak(hat5 - 100, hat5 + 2000) < 1e-4f, "undone note is gone");
    const int64_t rim5 = start + 500 + 5 * L;
    CHECK(k.onset(rim5 - 1000, rim5 + 1000) == rim5, "base note still plays");
}

static void testMidiNoteBeforeStart() {
    std::printf("MIDI loops: a note just before the recording starts lands on its start\n");
    Keys k;
    k.note(0, ROUNDS_INSTRUMENT_SUB_BASS, 40, 100, 9000);  // 1128 samples before the tap lands
    k.tap(0, 10000);
    k.note(0, ROUNDS_INSTRUMENT_SUB_BASS, 40, 0, 20000);
    k.tap(0, 10128 + 48000 - 128);
    k.run(256);
    CHECK(k.s().kind == ROUNDS_KIND_MIDI && k.s().notes == 1, "note picked up (%d)", k.s().notes);
    CHECK(k.s().instrument == ROUNDS_INSTRUMENT_SUB_BASS, "plays the keys' instrument");
    CHECK(k.engine.beginExport(0), "export");
    k.run(256);
    CHECK(k.engine.transferStatus() == 2, "export done");
    std::vector<RoundsNote> notes(16);
    int64_t length = 0;
    int instrument = -1;
    const int count = k.engine.takeExportNotes(notes.data(), 16, &length, &instrument);
    CHECK(count == 1 && notes[0].start == 0, "on the loop's start (%d notes, start %lld)", count, (long long)notes[0].start);
    CHECK(count == 1 && notes[0].length == 20000 - 9000, "full length kept (%lld)", (long long)notes[0].length);
}

static void testMidiSaveAndLoad() {
    std::printf("MIDI loops: save and load\n");
    Keys k;
    k.tap(0, 1000);
    const int64_t start = 1128;
    k.note(0, ROUNDS_INSTRUMENT_DRY_KIT, 37, 100, start + 7000);
    k.note(0, ROUNDS_INSTRUMENT_DRY_KIT, 37, 0, start + 8000);
    k.tap(0, start + 48000 - 128);
    k.run(256);
    k.engine.setInstrument(0, ROUNDS_INSTRUMENT_ELECTRO_KIT);
    k.run(256);
    CHECK(k.s().instrument == ROUNDS_INSTRUMENT_ELECTRO_KIT, "instrument switched");
    k.engine.beginExport(0);
    k.run(256);
    std::vector<RoundsNote> notes(16);
    int64_t length = 0;
    int instrument = -1;
    const int count = k.engine.takeExportNotes(notes.data(), 16, &length, &instrument);
    CHECK(count == 1 && length == 48000 && instrument == ROUNDS_INSTRUMENT_ELECTRO_KIT, "exported");
    float samples[4] = {};
    CHECK(k.engine.takeExport(samples, 4) == -1, "not audio");

    Keys loaded;
    loaded.run(256);
    CHECK(loaded.engine.beginImportNotes(2, notes.data(), count, length, instrument), "import");
    loaded.run(256);
    CHECK(loaded.engine.transferStatus() == 2, "import done");
    CHECK(loaded.s(2).state == ROUNDS_LOOP_STOPPED && loaded.s(2).kind == ROUNDS_KIND_MIDI && loaded.s(2).notes == 1,
          "loaded stopped as MIDI");
    CHECK(loaded.s(2).instrument == ROUNDS_INSTRUMENT_ELECTRO_KIT, "with its instrument");
    CHECK(status(loaded.engine).bars > 0, "sets the tempo");
    const int64_t tapAt = 20000;
    loaded.tap(2, tapAt);
    loaded.run(48000);
    const int64_t rim = tapAt + 128 + 7000;
    CHECK(loaded.onset(rim - 1000, rim + 1000) == rim, "plays from the top (%lld)", (long long)(loaded.onset(rim - 1000, rim + 1000) - rim));
}

static void testMidiQuantizedRecording() {
    std::printf("MIDI loops: a queued recording picks up a note played just before its bar line\n");
    Keys k;
    k.engine.setQuantize(true);
    k.tap(0, 1000);             // an (empty-sounding) audio loop sets the tempo
    k.tap(0, 1000 + 192000);
    k.run(256);
    const int64_t bar = status(k.engine).barLength;
    CHECK(bar == 96000, "bar %lld", (long long)bar);
    // Next bar line after now, in engine time (no latency here).
    const int64_t pos = status(k.engine).cyclePosition % bar;
    const int64_t line = k.now + (bar - pos) + bar;  // the one after next, to leave room
    k.tap(1, line - bar / 2);
    k.run(256);
    CHECK(k.s(1).queued == ROUNDS_QUEUED_TAP, "recording queued");
    k.note(1, ROUNDS_INSTRUMENT_E_PIANO, 64, 90, line - 1500);  // a little early
    k.note(1, ROUNDS_INSTRUMENT_E_PIANO, 64, 0, line + 6000);
    k.note(1, ROUNDS_INSTRUMENT_E_PIANO, 67, 90, line + 30000);
    k.note(1, ROUNDS_INSTRUMENT_E_PIANO, 67, 0, line + 40000);
    k.runTo(line + 256);
    k.run(256);
    CHECK(k.s(1).state == ROUNDS_LOOP_RECORDING && k.s(1).kind == ROUNDS_KIND_MIDI, "recording MIDI from the bar line");
    k.tap(1, line + bar - 128 + 2000);  // closed a little late: snaps to one bar
    k.run(512);
    CHECK(k.s(1).loopLength == bar, "one bar (%lld)", (long long)k.s(1).loopLength);
    k.engine.beginExport(1);
    k.run(256);
    std::vector<RoundsNote> notes(16);
    int64_t length = 0;
    int instrument = -1;
    const int count = k.engine.takeExportNotes(notes.data(), 16, &length, &instrument);
    CHECK(count == 2, "two notes (%d)", count);
    if (count == 2) {
        CHECK(notes[0].start == 0 && notes[0].length == 7500, "early note on the bar line (%lld, %lld)",
              (long long)notes[0].start, (long long)notes[0].length);
        CHECK(notes[1].start == 30000, "second note where it was played (%lld)", (long long)notes[1].start);
    }
}

static void testStoppedMidiLetsGo() {
    std::printf("MIDI loops: stopping releases held loop notes\n");
    Keys k;
    k.tap(0, 1000);
    k.note(0, ROUNDS_INSTRUMENT_WARM_PAD, 60, 100, 2000);
    k.note(0, ROUNDS_INSTRUMENT_WARM_PAD, 60, 0, 40000);
    k.tap(0, 1128 + 48000 - 128);
    k.run(48000 + 20000);  // into the second time round: the pad sounds
    CHECK(k.peak(k.now - 2000, k.now) > 0.01f, "pad sounds");
    k.command(0, ROUNDS_CMD_STOP);
    k.run(48000 * 3);
    CHECK(k.peak(k.now - 4000, k.now) < 1e-4f, "silent after stopping");
}

static void testInstruments() {
    std::printf("Instruments: every sound plays, stays in range and ends\n");
    static const char* names[] = {"Sub Bass", "Finger Bass", "Synth Bass", "E-Piano", "Warm Pad", "Mono Lead", "Dry Kit", "Electro Kit"};
    for (int i = 0; i < ROUNDS_INSTRUMENT_COUNT; i++) {
        Keys k;
        k.run(256);
        const bool kit = i >= ROUNDS_INSTRUMENT_DRY_KIT;
        const int notes[] = {36, 38, 39, 42, 46, 45, 58, 37};
        for (int n = 0; n < (kit ? 8 : 3); n++) {
            const int note = kit ? notes[n] : 36 + 12 * n;
            k.note(-1, i, note, 100, k.now + 2000 + n * 12000);
            k.note(-1, i, note, 0, k.now + 9000);
        }
        k.run(48000 * 4);
        const float peak = k.peak(0, k.now);
        bool finite = true;
        for (float v : k.out) finite = finite && std::isfinite(v);
        CHECK(finite && peak > 0.02f && peak < 1.2f, "%s: peak %.3f", names[i], peak);
        CHECK(k.peak(k.now - 4800, k.now) < 1e-4f, "%s: silent after release", names[i]);
    }
}

// ---------------------------------------------------------------------------
// Parts (milestone 5)

static RoundsLoopStatus poolStatus(Engine& engine, int id) {
    RoundsLoopStatus s;
    engine.getPoolLoopStatus(id, &s);
    return s;
}

static RoundsPartStatus partStatus(Engine& engine, int part) {
    RoundsPartStatus s;
    engine.getPartStatus(part, &s);
    return s;
}

static void testPartSwitchTiming() {
    std::printf("Parts: a switch lands where the longest loop comes round, sample-exact\n");
    const int in = 200, outLat = 300;
    Keys k(in, outLat);
    const int64_t L = 48000;
    // Slot 1: a rim at 1000 (shared). Slot 2: a hat at 6000, recorded 24000 later.
    const int64_t start = 10000 + 128;
    k.tap(0, 10000);
    k.note(0, ROUNDS_INSTRUMENT_DRY_KIT, 37, 110, start + 1000);
    k.note(0, ROUNDS_INSTRUMENT_DRY_KIT, 37, 0, start + 2000);
    k.tap(0, start + L - 128);
    const int64_t start2 = start + L + 24000;
    k.tap(1, start2 - 128);
    k.note(1, ROUNDS_INSTRUMENT_DRY_KIT, 42, 100, start2 + 6000);
    k.note(1, ROUNDS_INSTRUMENT_DRY_KIT, 42, 0, start2 + 7000);
    k.tap(1, start2 + L - 128);
    k.run(256);
    CHECK(k.s(0).loopLength == L && k.s(1).loopLength == L, "two 1-bar loops");
    k.engine.setShared(0, true);
    k.runTo(start2 + L + 20000);
    const int64_t applied = k.now;
    k.engine.newPart();
    k.run(128);
    const int64_t R = start + ((applied + outLat - start + L - 1) / L) * L;  // next time the rim loop comes round
    CHECK(status(k.engine).nextPart == 1, "new part waits for the end of the cycle");
    CHECK(status(k.engine).partSwitchIn == R - (k.now + outLat), "heard in %lld samples (expected %lld)",
          (long long)status(k.engine).partSwitchIn, (long long)(R - (k.now + outLat)));
    CHECK(partStatus(k.engine, 1).exists == 1 && partStatus(k.engine, 1).loops == 2, "part 2 has both loops");
    k.runTo(R + 3 * L);
    CHECK(status(k.engine).part == 1 && status(k.engine).nextPart == -1, "switched to part 2");
    CHECK(k.s(0).loopID == 0 && k.s(1).loopID == 9, "slot 1 shared (loop %d), slot 2 copied (loop %d)", k.s(0).loopID,
          k.s(1).loopID);
    // What is written at clock c is heard at c + outLat.
    for (int64_t cycle = -1; cycle <= 1; cycle++) {
        const int64_t rim = R + cycle * L + 1000 - outLat;
        CHECK(k.onset(rim - 500, rim + 500) == rim, "rim plays straight through (cycle %lld, off by %lld)",
              (long long)cycle, (long long)(k.onset(rim - 500, rim + 500) - rim));
    }
    const int64_t oldHat = R - L + 30000 - outLat;  // the hat loop started 24000 into the rim's cycle
    CHECK(k.onset(oldHat - 500, oldHat + 500) == oldHat, "old hat before the switch");
    for (int64_t cycle = 0; cycle <= 1; cycle++) {
        const int64_t hat = R + cycle * L + 6000 - outLat;
        CHECK(k.onset(hat - 500, hat + 500) == hat, "copied hat starts from its top at the switch (off by %lld)",
              (long long)(k.onset(hat - 500, hat + 500) - hat));
        CHECK(k.peak(oldHat + (cycle + 1) * L, oldHat + (cycle + 1) * L + 500) < 0.02f, "old part's hat is silent");
    }

    // Back to part 1: asking twice takes it back; asking again queues it.
    k.engine.switchPart(0);
    k.run(128);
    CHECK(status(k.engine).nextPart == 0, "queued");
    k.engine.switchPart(0);
    k.run(128);
    CHECK(status(k.engine).nextPart == -1, "asking again takes it back");
    const int64_t applied2 = k.now;
    k.engine.switchPart(0);
    k.run(128);
    const int64_t R2 = start + ((applied2 + outLat - start + L - 1) / L) * L;
    k.runTo(R2 + 2 * L);
    CHECK(status(k.engine).part == 0, "back in part 1");
    const int64_t hat = R2 + 6000 - outLat;
    CHECK(k.onset(hat - 500, hat + 500) == hat, "part 1's hat restarts from its top (off by %lld)",
          (long long)(k.onset(hat - 500, hat + 500) - hat));
}

static void testNewPartCopiesAudio() {
    std::printf("Parts: a new part copies audio loops (overdubs merged) and keeps shared ones\n");
    const int64_t L = 48000;
    Rig r;
    auto tap = [&r](int slot) {
        r.engine.send(slot, ROUNDS_CMD_TAP, -1);
        const int64_t at = r.now;
        r.run(128);
        return at;
    };
    const int64_t t0 = tap(0);
    r.clicks = {t0 + 1000, t0 + 2 * L + 20000};
    r.run(L - 128);
    tap(0);                       // a 1-bar loop with a click at 1000
    r.run(L - 128);
    tap(0);                       // overdub...
    r.run(L / 2);
    tap(0);                       // ...with a click at 20000
    CHECK(r.engine.transferStatus() == 0 && loopStatus(r.engine, 0).layers == 1, "one overdub layer");
    r.run(L / 2 + 30000);
    const int64_t t1 = tap(1);
    r.clicks.push_back(t1 + 5000);
    r.run(L - 128);
    tap(1);
    r.engine.setShared(1, true);
    r.run(10000);
    const int64_t applied = r.now;
    r.engine.newPart();
    r.run(128);
    const int64_t R = t0 + ((applied - t0 + L - 1) / L) * L;
    r.run(R + 2 * L - r.now);
    CHECK(status(r.engine).part == 1, "switched");
    const RoundsLoopStatus copy = loopStatus(r.engine, 0);
    CHECK(copy.loopID == 8 && copy.layers == 0 && copy.state == ROUNDS_LOOP_PLAYING && copy.loopLength == L,
          "slot 1 is a copy (loop %d, %d layers, state %d)", copy.loopID, copy.layers, copy.state);
    CHECK(loopStatus(r.engine, 1).loopID == 1 && loopStatus(r.engine, 1).shared == 1, "slot 2 is the shared loop");
    const std::vector<int64_t> after = r.onsets(R, R + L);
    const std::vector<int64_t> want = {R + 1000, R + 20000, R + mod(t1 + 5000 - t0, L)};
    CHECK(after == want, "copy plays base and overdub, shared loop in step (%zu onsets)", after.size());

    r.engine.send(0, ROUNDS_CMD_CLEAR, -1);
    r.run(128);
    CHECK(poolStatus(r.engine, 0).state == ROUNDS_LOOP_PLAYING && poolStatus(r.engine, 0).layers == 1,
          "clearing the copy leaves part 1's loop alone");
    r.engine.send(1, ROUNDS_CMD_CLEAR, -1);
    r.run(128);
    CHECK(poolStatus(r.engine, 1).state == ROUNDS_LOOP_EMPTY && partStatus(r.engine, 0).loops == 1,
          "clearing a shared loop clears it in every part");
}

static void testPartSwitchClosesAndStops() {
    std::printf("Parts: leaving a part closes its overdubs; with nothing playing a switch is immediate\n");
    Driver d;
    d.send(ROUNDS_CMD_TAP);
    d.advance(48000 - 128);
    d.send(ROUNDS_CMD_TAP);
    d.engine.newPart();
    d.advance(128);
    CHECK(status(d.engine).nextPart == 1, "new part queued");
    d.send(ROUNDS_CMD_TAP);
    CHECK(d.s().state == ROUNDS_LOOP_OVERDUBBING, "overdubbing in part 1");
    d.advance(48000);
    CHECK(status(d.engine).part == 1, "switched");
    CHECK(poolStatus(d.engine, 0).state == ROUNDS_LOOP_PLAYING && poolStatus(d.engine, 0).layers == 1,
          "the overdub closed at the switch (state %d, %d layers)", poolStatus(d.engine, 0).state,
          poolStatus(d.engine, 0).layers);
    d.engine.send(-1, ROUNDS_CMD_START_STOP_ALL, -1);
    d.advance(128);
    CHECK(d.s().state == ROUNDS_LOOP_STOPPED, "part 2 stopped");
    d.engine.switchPart(0);
    d.advance(128);
    CHECK(status(d.engine).part == 0 && status(d.engine).nextPart == -1, "switch with nothing playing is immediate");
    CHECK(d.s().state == ROUNDS_LOOP_STOPPED, "and leaves the part stopped");
    d.engine.switchPart(5);
    d.advance(128);
    CHECK(status(d.engine).part == 0 && status(d.engine).nextPart == -1, "a part that doesn't exist is ignored");
}

static void testSharedLayerPool() {
    std::printf("Parts: 32 overdubs on 8 loops share 24 layers, merging the oldest\n");
    Driver d;
    for (int slot = 0; slot < 8; slot++) {
        d.send(ROUNDS_CMD_TAP, slot);
        d.advance(9600 - 128);
        d.send(ROUNDS_CMD_TAP, slot);
    }
    bool allOpened = true;
    for (int round = 0; round < 4; round++) {
        for (int slot = 0; slot < 8; slot++) {
            d.send(ROUNDS_CMD_TAP, slot);
            allOpened = allOpened && d.s(slot).layerOpen == 1;
            d.advance(2000);
            d.send(ROUNDS_CMD_TAP, slot);
            d.advance(512);
        }
    }
    int layers = 0;
    for (int slot = 0; slot < 8; slot++) layers += d.s(slot).layers;
    CHECK(allOpened, "every overdub got a layer");
    CHECK(layers <= Engine::kLayers - 3 && d.s(0).layers < 4, "%d layers kept, loop 1 has %d", layers, d.s(0).layers);
}

static void testSessionParts() {
    std::printf("Parts: reset and set_part_loop rebuild a session's parts\n");
    Driver d;
    d.send(ROUNDS_CMD_TAP);
    d.advance(48000 - 128);
    d.send(ROUNDS_CMD_TAP);
    d.engine.reset();
    d.advance(128);
    CHECK(d.s().state == ROUNDS_LOOP_EMPTY && partStatus(d.engine, 1).exists == 0, "reset empties everything");
    d.engine.setPartLoop(2, 3, 5);
    d.advance(128);
    CHECK(partStatus(d.engine, 2).exists == 1 && d.engine.partLoop(2, 3) == 5 && d.engine.partLoop(2, 0) == 16,
          "part 3 made, slot 4 plays loop 6");
    // An empty part for imported stems avoids loop 6 (part 3 uses it) and doesn't switch.
    d.engine.addEmptyPart(0);
    d.engine.addEmptyPart(1);
    d.advance(128);
    CHECK(partStatus(d.engine, 1).exists == 1 && status(d.engine).part == 0, "empty part 2 made, still in part 1");
    bool fresh = true;
    for (int slot = 0; slot < 8; slot++) {
        const int id = d.engine.partLoop(1, slot);
        fresh = fresh && id != 5 && id >= 0 && poolStatus(d.engine, id).state == ROUNDS_LOOP_EMPTY;
        for (int other = 0; other < 8; other++) fresh = fresh && id != d.engine.partLoop(0, other) && id != d.engine.partLoop(2, other);
    }
    CHECK(fresh, "its loops are empty and no other part's");
}

// ---------------------------------------------------------------------------
// Whole song (milestone 6)

static RoundsStatus songStatus(Engine& engine) { return status(engine); }

static void testSongTiming() {
    std::printf("Song: plays in time from its bars, sets the grid, stops and jumps on bar lines\n");
    const int in = 200, outLat = 300;
    Keys k(in, outLat);
    k.engine.setQuantize(true);
    const int64_t bar = 24000, downbeat = 5000;
    const int bars = 12;
    // One stem: a click on every downbeat, louder on bar 5.
    std::vector<float> stem(static_cast<size_t>(downbeat + bars * bar), 0.0f);
    for (int b = 0; b < bars; b++) {
        for (int j = 0; j < 32; j++) stem[static_cast<size_t>(downbeat + b * bar + j)] = b == 4 ? 0.9f : 0.5f;
    }
    const float* stems[] = {stem.data()};
    CHECK(k.engine.loadSong(stems, 1, static_cast<int64_t>(stem.size()), bar, downbeat), "song loads");
    k.run(256);
    CHECK(songStatus(k.engine).songLoaded == 1 && status(k.engine).barLength == bar, "the song sets the bar length");
    k.engine.setSongLane(5, 0);
    k.run(128);

    // Play: bar 1 is heard in full, outLat after the press lands.
    k.runTo(20000);
    const int64_t pressed = k.now;
    k.engine.send(-1, ROUNDS_CMD_START_STOP_ALL, -1);
    k.run(128);
    const int64_t top = pressed + outLat;  // real time bar 1 is heard
    // What is heard at real time t was written at clock t - outLat.
    for (int b = 0; b < 3; b++) {
        const int64_t expected = top + b * bar - outLat;
        k.runTo(expected + 2000);
        CHECK(k.onset(expected - 1000, expected + 1000) == expected, "bar %d heard on time (off by %lld)", b + 1,
              (long long)(k.onset(expected - 1000, expected + 1000) - expected));
    }
    CHECK(status(k.engine).songBar == 3, "status says bar 3 (%d)", status(k.engine).songBar);

    // A loop recorded over it snaps to the song's bars and starts on its bar line.
    k.tap(0, top + 3 * bar + 2000 - outLat);           // just after bar 4's line: counts for it
    k.tap(0, top + 5 * bar - 3000 - outLat);           // close it ~2 bars later
    k.run(256);
    CHECK(k.s(0).loopLength == 2 * bar, "loop is 2 of the song's bars (%lld)", (long long)k.s(0).loopLength);

    // Stop mid-bar: the song stops on the next bar line and Play carries on from there.
    k.runTo(top + 6 * bar + bar / 2 - outLat);
    k.engine.send(-1, ROUNDS_CMD_START_STOP_ALL, -1);
    k.run(128);
    CHECK(status(k.engine).songPlaying == 1, "still playing until the bar line");
    k.runTo(top + 7 * bar - outLat + 1000);
    CHECK(status(k.engine).songPlaying == 0 && status(k.engine).songBar == 8, "stopped at bar 8's line (bar %d)",
          status(k.engine).songBar);
    const int64_t silentFrom = top + 7 * bar - outLat;
    CHECK(k.peak(silentFrom + 100, silentFrom + 3000) < 1e-4f, "nothing after the stop");

    // Jump to bar 5 and play: its (loud) downbeat is heard on the bar line.
    k.engine.setSongBar(5);
    k.run(128);
    CHECK(status(k.engine).songBar == 5, "start bar 5");
    const int64_t pressed2 = k.now;
    k.engine.send(-1, ROUNDS_CMD_START_STOP_ALL, -1);
    k.run(128);
    const int64_t top2 = pressed2 + outLat;
    k.runTo(top2 + 2000);
    const int64_t loud = top2 - outLat;
    CHECK(k.onset(loud - 1000, loud + 1000) == loud && k.peak(loud, loud + 32) > 0.8f, "bar 5 heard at the top (off by %lld)",
          (long long)(k.onset(loud - 1000, loud + 1000) - loud));
    CHECK(k.s(0).state == ROUNDS_LOOP_PLAYING && k.s(0).playPosition == mod(k.now - top2, 2 * bar),
          "the loop restarted with it, from its top (at %lld, expected %lld)", (long long)k.s(0).playPosition,
          (long long)mod(k.now - top2, 2 * bar));

    // While playing, a jump lands on the next bar line.
    k.runTo(top2 + bar / 3);
    k.engine.setSongBar(2);
    k.run(128);
    const int64_t line = top2 + bar - outLat;  // bar 6's line, now bar 2
    k.runTo(line + 2000);
    CHECK(k.onset(line - 1000, line + 1000) == line && k.peak(line, line + 32) < 0.6f, "jumped to bar 2 on the bar line");

    // Gain and unload.
    k.engine.setSongGain(0, 0.0f);
    k.runTo(line + bar + 2000);
    CHECK(k.peak(line + bar - 50, line + bar + 500) < 0.1f || k.s(0).state == ROUNDS_LOOP_PLAYING, "gain 0 mutes the song");
    k.engine.loadSong(nullptr, 0, 0, 0, 0);
    k.run(256);
    CHECK(status(k.engine).songLoaded == 0 && status(k.engine).barLength == bar, "unloaded; the loop keeps the tempo");
}

static void testSongLanes() {
    std::printf("Song lanes: only stems in a lane are heard; a stem lane only plays; clear takes the stem out\n");
    Keys k(0, 0);
    const int64_t bar = 24000;
    std::vector<float> a(static_cast<size_t>(8 * bar), 0.25f), b(static_cast<size_t>(8 * bar), 0.5f);
    const float* stems[] = {a.data(), b.data()};
    k.engine.loadSong(stems, 2, 8 * bar, bar, 0);
    k.run(256);
    k.engine.setSongLane(0, 0);
    k.run(128);
    CHECK(k.s(0).songStem == 0 && k.s(1).songStem == -1, "stem 0 in lane 1, lane 2 empty");
    k.engine.send(-1, ROUNDS_CMD_START_STOP_ALL, -1);
    k.run(4096);
    CHECK(std::fabs(k.peak(k.now - 1000, k.now) - 0.25f) < 1e-4f, "only the stem in a lane is heard (%f)", k.peak(k.now - 1000, k.now));
    CHECK(k.s(0).peak > 0.2f, "the lane shows the stem's level");

    // Moving stem 1 into lane 2, then stem 1 into lane 3: one lane each.
    k.engine.setSongLane(1, 1);
    k.engine.setSongLane(2, 1);
    k.run(1024);
    CHECK(k.s(1).songStem == -1 && k.s(2).songStem == 1, "a stem sits in one lane");
    CHECK(std::fabs(k.peak(k.now - 500, k.now) - 0.75f) < 1e-4f, "both stems heard");

    // Tap and notes on a stem lane don't record.
    k.tap(0, k.now);
    k.note(0, ROUNDS_INSTRUMENT_SUB_BASS, 48, 100, k.now);
    k.note(0, ROUNDS_INSTRUMENT_SUB_BASS, 48, 0, k.now + 500);
    k.run(4096);
    CHECK(k.s(0).state == ROUNDS_LOOP_EMPTY && k.s(0).songStem == 0, "a stem lane doesn't record");

    // Clear takes the stem out; the lane is a normal lane again.
    k.command(0, ROUNDS_CMD_CLEAR);
    k.run(1024);
    CHECK(k.s(0).songStem == -1, "clear takes the stem out of the lane");
    CHECK(std::fabs(k.peak(k.now - 500, k.now) - 0.5f) < 0.02f, "and it is no longer heard (the played bass note may still ring) (%f)", k.peak(k.now - 500, k.now));
    k.tap(0, k.now);
    k.run(1024);
    CHECK(k.s(0).state == ROUNDS_LOOP_RECORDING, "the lane records again");

    // Unloading empties the lanes.
    k.engine.loadSong(nullptr, 0, 0, 0, 0);
    k.run(256);
    CHECK(k.s(2).songStem == -1, "unloading empties the lanes");
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
    testLoopsSnapToBars();
    testLoopsStayInStep();
    testRestartInPhase();
    testSnappedLongerIsSilent();
    testLoopGain();
    testSaveAndLoad();
    testTempoFromFirstLoop();
    testQuantizedActions();
    testQuantizedRecordingAudio();
    testClick();
    testStartStopAll();
    testMidiLoopTiming();
    testMidiOverdubAndUndo();
    testMidiNoteBeforeStart();
    testMidiSaveAndLoad();
    testMidiQuantizedRecording();
    testStoppedMidiLetsGo();
    testInstruments();
    testPartSwitchTiming();
    testNewPartCopiesAudio();
    testPartSwitchClosesAndStops();
    testSharedLayerPool();
    testSessionParts();
    testSongTiming();
    testSongLanes();
    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
