# Rounds

A focused guitar + MIDI live looper for macOS, played from a Novation Launchkey 37 MK3 and a Yamaha FC5 footswitch.

See [CLAUDE.md](CLAUDE.md) for the spec and build plan, and `design/` for the controller mapping and main window mockup (open `design/main-window-mockup.html` in a browser).

## Run it on your Mac

You need Xcode (from the App Store) and [Homebrew](https://brew.sh).

```sh
git clone https://github.com/soundfrontllc/rounds.git
cd rounds
./scripts/bootstrap.sh     # installs XcodeGen, generates Rounds.xcodeproj, opens it
```

In Xcode press **⌘R**. The first time, macOS asks for microphone access; allow it so Rounds can hear your interface. After pulling new changes, run `./scripts/bootstrap.sh` again if files were added.

## Layout

- `Core/` — the C++ real-time audio engine (looper, footswitch gestures, latency probe) behind the C API in `Core/include/rounds_core.h`. `Core/platform/` holds the Core Audio device code.
- `Core/tests/` — tests and the timing harness. Run them anywhere with CMake:
  ```sh
  cmake -S Core -B Core/build && cmake --build Core/build && ./Core/build/rounds_tests
  ```
- `App/Rounds/` — the SwiftUI app: device picker, loop display, event log, MIDI input (MIDIKit).
