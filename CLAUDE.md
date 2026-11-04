# Rounds: guitar + MIDI looper for macOS

A focused live looper, a much simpler Ableton Session View with only what Corey needs. Controlled entirely from a Novation Launchkey 37 and a Yamaha FC5 footswitch (in the Launchkey's sustain jack). Guitar comes in through a Valeton GP-5 used as the USB audio interface ("GP-5 Audio": 2 in / 2 out at 44.1 kHz, headphones on the GP-5, which monitors the guitar directly, so Rounds' Monitor input stays off).

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
| Tap | Record → Play → Overdub (cycles) |
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
2. **Multiple loops on one clock.** 8 slots, shorter loops repeat under the longest, top pads select loops, pad LEDs, knob volumes. Until bars arrive in milestone 3, the first loop recorded sets a base length and later loops snap to ⅛× … 8× of it (Corey's choice).
3. **Tempo from first loop**, click, bar grid, quantize. Confirmed on hardware: the first loop is taken as 1, 2, 4 or 8 bars of 4/4, whichever gives 80–160 BPM (½× / 2× buttons reinterpret it; saved with the session). Later loops snap to 1, 2, 4, 8 … bars (no sub-bar loops). With Quantize on and anything playing, record / play / stop wait for the next bar; a tap up to ⅛ bar late (max 250 ms) counts for the bar just gone, and a recording then starts from that bar line using a short input history. Overdub in/out and undo stay immediate. A second tap, undo or stop takes a queued action back. With nothing playing, actions start at once and the grid moves with them. Click on every beat while anything plays (accented downbeat). Launchkey Play = stop all on the next bar / start all from the top of the longest loop. Queued pads blink.
4. **Drum pads and MIDI loops**, built-in instruments, keys follow the selected loop. Confirmed on hardware: a loop records audio until a note arrives while it records, then it is a MIDI loop (notes only), playing the instrument the keys or pads had; notes up to 100 ms before a recording or overdub starts land on its start. Keys play the selected loop (empty or MIDI); with an audio loop selected they just play the keys instrument. Pads play and record into the selected loop when it is empty or recording (it becomes a drum loop, a MIDI loop with a kit, starting on the existing drum loop's kit); otherwise they go to the drum loop. Track ◀ ▶ browses the six synths (kits when a drum loop is selected) and switches a selected MIDI loop's sound. Two synthesized kits (Dry, Electro). Keys on any MIDI channel play the keys (Corey's Launchkey sends on channel 10). MIDI loops save their notes in session.json. Not yet: Shift + pad to choose a pad's sound, MIDI previews in the lanes.
5. **Parts**, switching at the end of the cycle, new part copies with shared loops. Confirmed on hardware: each part has the 8 slots. New part (the window's "+ New part", or Shift + an unused part's pad) copies the playing part into the next unused part, keeping shared loops as the same loop and copying the others with their overdubs merged, then switches to it. The Shared toggle in the loop panel marks a loop shared. A switch is heard exactly where the longest loop of the playing part comes round: shared loops play straight through, the new part's other loops start from their top, and the old part's loops fall silent (closing any recording or overdub). Asking again for the playing or the waiting part takes the switch back. With nothing playing a switch is immediate and leaves the part stopped (Play starts it). Knobs pick up the new part's volumes. While Shift is held the top pads show the parts (green = playing, blinking green = next, white = other parts). Up to 8 parts, 64 loops in all sharing 24 overdub layers (the oldest is merged when they run low). Sessions save every part and its name; right-click a part to rename it. Not yet: deleting a part.
6. **Stem import.** Import a song and split it into stems as loops. Corey chose: split inside Rounds, and the song sets the tempo (stretched when the session already has one). Confirmed on hardware (split, whole song in the lanes, knobs): HT-Demucs 6-stem (drums, bass, other, vocals, guitar, piano) as StemSplit's ONNX export, pinned to StemSplitio/htdemucs-6s-onnx at 49df9b69 (fp16 weights, 136 MB, SHA-256 checked), downloaded on first use to ~/Library/Application Support/Rounds/Models and run with onnxruntime 1.19.2 on the CPU (about a quarter of real time). Faster: the same model converted to Core ML at FP16 by `scripts/convert_stem_model.py` (HTDemucs6S_F16.mlpackage in the same folder, installed on Corey's Mac; not hosted anywhere yet) runs about 3.9x faster on the CPU, with stems 28–41 dB from the original on music; Rounds uses it when present. Measured and rejected: the Neural Engine (FP16 maths throws some stems far off, 0–21 dB), Core ML on the GPU (hangs on this model), onnxruntime's own Core ML provider (91 of 1419 ops, silent output). Tempo and bar 1 come from the drums stem (onset envelope, then a fine comb over the whole song, then snapped to the attacks); the window lets you adjust BPM, move bar 1 by a beat or 10 ms, pick a start bar (click the waveform) and 1/2/4/8 bars, untick stems, and preview the section through the interface. Add as part puts each ticked stem (mono) in slots 1, 2 … of a new empty part named after the song and bar; the loop seam is cross-faded with the audio just past the section. Stretching uses Apple's TimePitch, corrected against a click track so beats land within about a millisecond. The split song stays in memory until another is chosen. Each split logs its time and engine. **Whole song** (Corey's choices: the song's grid sets the tempo; each stem in its own lane, play only): "Load whole song" puts each ticked stem in its lane (drums 1, bass 2, other 3, vocals 4, guitar 5, piano 6) in every part, playing in time with the loops; loops go in lanes 7–8 and any lane whose stem was unticked (the load is refused if a chosen lane holds a loop in any part). A stem lane only plays: it shows the stem's waveform across the song, its knob sets the stem's volume (a knob that doesn't match a volume moves it at once, scaled to meet it at the end of its travel, then follows exactly; same for loop knobs), it has mute; tap, stop and undo do nothing there, keys and pads aren't recorded into it, and Clear (hold 2 s, or Remove stem) takes the stem out, freeing the lane. Its pad shows the lane colour while the song plays, dim white otherwise. "Add bars as part" skips stem lanes. The song's bars become the session grid (a bar rounded to a whole sample, or the song stretched to the loops' tempo if loops already set one); loops snap to them, quantize and the click follow it. Play starts the song and the loops together from the song's start bar, heard in full (everything starts outputLatency after the press so the first beat isn't cut); stop lands on the next bar line and Play carries on from there; clicking the Song panel's strip picks the start bar, or jumps there on the next bar line while playing. Sessions save the song as the split stems (24-bit ALAC), its grid and each stem's lane, loaded before the loops. A live recording that drifts will slowly slip against the loops (the grid is one steady tempo). Code: `App/Rounds/Stems/`.

Save/load sessions lands as soon as there is something worth saving (by milestone 2).

## Open questions

- Does any MIDI need to go out to other apps, or is the built-in sound enough?
