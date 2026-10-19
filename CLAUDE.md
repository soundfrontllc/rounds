# Rounds: guitar + MIDI looper for macOS

A focused live looper, a much simpler Ableton Session View with only what Corey needs. Controlled entirely from a Novation Launchkey 37 and a Yamaha FC5 footswitch (in the Launchkey's sustain jack). Guitar comes in through an audio interface.

Design references live in `design/`: `launchkey-37-mapping.png` (controller layout) and `main-window-mockup.html` (main window). Treat them as the source of truth for layout and behavior.

## Working rules for Claude Code

- **This is real-time audio.** Nothing on the audio thread may allocate, lock, log, call Objective-C/Swift runtime, or do file/network I/O. Communicate with the audio thread only through lock-free single-producer/single-consumer queues and atomics.
- **Keep the audio core small and separate.** It is C++ with a narrow C API. The Swift app never reaches into its internals, and UI changes never touch it.
- **Timing problems must show up as numbers.** Keep the timing test harness (milestone 1) passing; add a test whenever a timing bug is fixed.
- **You cannot hear or touch the hardware.** Every input path is visible in the on-screen event log. When a change needs hardware testing, say exactly what Corey should press and what should be seen or heard.
- Build in the milestone order below. Each milestone must be playable before starting the next. Don't add features from later milestones early.

## Tech stack

- **App/UI:** Swift + SwiftUI, Xcode project, macOS only.
- **MIDI:** CoreMIDI via [MIDIKit](https://github.com/orchetect/MIDIKit) (Swift package).
- **Audio:** Core Audio (AUHAL / AVAudioEngine I/O unit) driving a C++ real-time engine. Sample-accurate transport, all scheduling in samples.
- **Built-in sounds:** a simple drum sampler and a few basic synth/sample instruments (Sub Bass, Finger Bass, Synth Bass, E-Piano, Warm Pad, Mono Lead) inside the engine.
- **Sessions:** saved as a folder (JSON manifest + audio files as WAV/CAF).

## Core concepts

- **Loop:** one of 8 slots. Audio (from an input) or MIDI (from keys or drum pads). Has a color, length in bars, volume, pan, mute, and a stack of layers for undo.
- **Cycle:** the length of the longest loop in the current part. Shorter loops repeat under it (e.g. 1 bar ×8 under an 8-bar cycle). Loop lengths snap to bar multiples that divide evenly into the cycle (½ / ×2 controls adjust).
- **Tempo:** set from the first recorded loop (its length is taken as a whole number of bars), shown as "set by loop 1". Click/metronome follows it.
- **Quantize:** record, play, stop and part changes land on the next bar by default.
- **Part (scene):** a named set of loops (Verse, Chorus, Bridge…). Switching parts is queued and lands at the end of the current cycle. "New part" copies the current part and keeps shared loops (a loop marked shared, like Beat, appears in multiple parts).
- **Selected loop:** the target of the footswitch and the keys. Each MIDI loop owns its instrument; selecting it switches the keys to that instrument. Selecting an audio or empty loop leaves the keys on the last instrument.

## Controller mapping (Launchkey 37, DAW mode)

The app takes the Launchkey over in DAW/InControl mode so it can set pad LEDs and stop the controls sending their defaults. **The controller is a Launchkey 37 MK3.** Use Novation's Launchkey MK3 Programmer's Reference for DAW-mode messages (MK4 messages differ).

| Control | Action |
|---|---|
| Knobs 1–8 | Loop volume 1–8. Shift + knob: pan |
| Top pad row 1–8 | Select / arm loop 1–8. Shift + pad: switch to part 1–8 |
| Bottom pad row 1–8 | Drum sounds (velocity-sensitive): Kick, Snare, Clap, Closed hat, Open hat, Low tom, Shaker, Rim. Shift + pad: choose sound. Hits record into the drum loop (Beat) |
| Keys | Play the selected MIDI loop's instrument |
| Play | Start/stop all |
| Record | Same as footswitch tap |
| Track ◀ ▶ | Browse instrument for the keys |

**Pad LEDs (top row):** off = empty; solid loop color = playing, pulsing on its downbeat; red ring = recording or overdubbing; blinking = queued for next bar; dim = muted. Bottom row flashes on each hit.

## Footswitch (Yamaha FC5, acts on the selected loop)

| Gesture | Action |
|---|---|
| Tap | Record → Overdub → Play (cycles) |
| Double-tap | Undo last layer |
| Hold | Stop. Hold 2 s: clear |

A tap acts immediately on press (no waiting to see if a double-tap follows). Because that tap has already happened, double-tap and hold first revert what the tap did, then act: double-tap = revert the tap (e.g. drop the layer it just opened) and undo the last layer; hold = revert the tap and stop. A second press within 0.3 s is a double-tap; holding 0.6 s is a hold, 2 s a clear. This is implemented in `Core/src/footswitch.cpp` and `Looper::undoRevertingTap` / `stopRevertingTap`; Corey still has to confirm it feels right.

## Main window

One screen (see mockup): header with session name, tempo, position (bar.beat.sixteenth / cycle bars), quantize, click on/off, input, footswitch indicator, "Import song → stems". Then the parts strip, the 8 loop lanes with waveform/MIDI previews, state badge and volume, the drum pad row with kit picker, the selected-loop panel (undo, ½ / ×2, clear), the keys instrument list, a live Launchkey mirror, and the footswitch legend. Plus an event log (MIDI and footswitch) toggleable from day one.

## Building and testing

- Core tests and timing harness (any OS): `cmake -S Core -B Core/build && cmake --build Core/build && ./Core/build/rounds_tests`. Run them after every core change.
- Mac app: `./scripts/bootstrap.sh` (XcodeGen generates `Rounds.xcodeproj` from `project.yml`; the project file is not committed). CI builds the app on macOS for every PR.
- Space bar stands in for the footswitch in the app.

## Hard problems (solve deliberately)

1. **Latency compensation.** Recorded audio is shifted earlier by the measured round-trip latency. Include a calibration step (play a click out, record it back in, measure).
2. **Sample-accurate loop boundaries.** No drift between loops, ever. Short crossfades at loop seams to avoid clicks.
3. **Quantized start/stop.** Actions are scheduled to exact sample positions on the next bar.

## Milestones

1. **Single-track audio looper.** Guitar in, footswitch tap/double-tap/hold working, event log, latency calibration. Timing test harness: records a click through a loopback, loops it, and reports measured latency and drift in samples. Get timing right here before anything else.
2. **Multiple loops on one clock.** 8 slots, shorter loops repeat under the longest, top pads select loops, pad LEDs, knob volumes.
3. **Tempo from first loop**, click, bar grid, quantize.
4. **Drum pads and MIDI loops**, built-in instruments, keys follow the selected loop.
5. **Parts**, switching at the end of the cycle, new part copies with shared loops.
6. **Stem import.** Import a song and split it into stems as loops (approach to be decided).

Save/load sessions lands as soon as there is something worth saving (by milestone 2).

## Open questions

- Audio interface: can the GP-5 act as a USB audio interface, or is a separate one (Focusrite, MOTU M2) used?
- Stem separation approach for milestone 6 (local model vs. external tool).
- Does any MIDI need to go out to other apps, or is the built-in sound enough?
