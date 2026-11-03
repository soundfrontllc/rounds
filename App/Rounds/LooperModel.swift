import AVFoundation
import CoreAudio
import Foundation
import SwiftUI

struct LogEntry: Identifiable {
    let id = UUID()
    let time: Date
    let source: String
    let text: String
}

struct AudioDevice: Identifiable, Hashable {
    let id: UInt32
    let name: String
    let inputChannels: Int
    let outputChannels: Int
}

enum LoopState: Int32 {
    case empty, recording, overdubbing, playing, stopped

    var label: String {
        switch self {
        case .empty: return "Empty"
        case .recording: return "Recording"
        case .overdubbing: return "Overdubbing"
        case .playing: return "Playing"
        case .stopped: return "Stopped"
        }
    }
}

/// One loop slot as the UI sees it.
struct LoopInfo: Identifiable, Equatable {
    let id: Int
    var state: LoopState = .empty
    var seconds: Double = 0          // loop length, or recorded so far while recording
    var position: Double = 0         // 0...1
    var layers = 0
    var level: Float = 0
    var gain: Float = 1
    var bars = 0                     // length in bars, 0 until there is a tempo
    var queued = Queued.none
    var midi = false                 // a MIDI loop (notes), else audio
    var instrument = Instrument.subBass
    var notes = 0
    var loopID = 0                   // the engine loop in this slot of the playing part
    var shared = false               // "New part" keeps it instead of copying it

    var isDrumLoop: Bool { state != .empty && midi && instrument.isKit }

    /// An action waiting for the next bar.
    enum Queued: Int32 {
        case none, tap, stop
    }
}

/// One part (Verse, Chorus …) as the UI sees it.
struct PartInfo: Identifiable, Equatable {
    let id: Int
    var exists = false
    var loops = 0
    var cycleBars = 0
}

/// UI-side state. Polls the engine; never touches the audio thread directly.
@MainActor
final class LooperModel: ObservableObject {
    @Published var devices: [AudioDevice] = []
    @Published var deviceID: UInt32 = 0
    @Published var inputChannel = 0
    @Published var bufferFrames = 128
    @Published var audioRunning = false
    @Published var audioError: String?
    @Published var micDenied = false

    @Published var state: LoopState = .empty
    @Published var loopSeconds: Double = 0
    @Published var recordedSeconds: Double = 0
    @Published var position: Double = 0   // 0...1
    @Published var layers = 0
    @Published var inputLevel: Float = 0
    @Published var outputLevel: Float = 0
    @Published var latencyIn = 0
    @Published var latencyOut = 0
    @Published var sampleRate: Double = 48000
    @Published var calibrating = false
    @Published var pedalDown = false
    @Published var monitorInput = false { didSet { applyMonitor() } }
    @Published var invertPedal = false { didSet { midi.invertPedal = invertPedal } }

    /// Loop slot the footswitch and keys act on (0...7).
    @Published var selectedLoop = 0 {
        didSet {
            router.select(loop: selectedLoop)
            followSelectedLoop()
            updateLaunchkeyLEDs()
        }
    }
    /// What the keys play. Follows the selected MIDI loop; stays put when an
    /// audio or empty loop is selected.
    @Published private(set) var keysInstrument = Instrument(rawValue: Int32(UserDefaults.standard.integer(forKey: "keysInstrument")))
        ?? .subBass {
        didSet { UserDefaults.standard.set(Int(keysInstrument.rawValue), forKey: "keysInstrument") }
    }
    /// The pads' kit while there is no drum loop (a drum loop plays its own).
    @Published private(set) var kit = Instrument(rawValue: Int32(UserDefaults.standard.integer(forKey: "kit")))
        .flatMap { $0.isKit ? $0 : nil } ?? .dryKit {
        didSet { UserDefaults.standard.set(Int(kit.rawValue), forKey: "kit") }
    }
    /// Pad last hit (for the window's pad row), and when.
    @Published var lastHit: (pad: Int, velocity: Int, time: Date)?
    private var lastRoutes: (ControlRouter.NoteRoute, ControlRouter.NoteRoute)?
    @Published var loops: [LoopInfo] = (0..<Int(ROUNDS_MAX_LOOPS)).map { LoopInfo(id: $0) }
    @Published var parts: [PartInfo] = (0..<Int(ROUNDS_MAX_PARTS)).map { PartInfo(id: $0, exists: $0 == 0) }
    @Published var currentPart = 0
    /// Part waiting for the end of the cycle (-1 = none), and how far off it is.
    @Published var nextPart = -1
    @Published var partSwitchBars: Double = 0
    /// Saved with the session.
    @Published var partNames = LooperModel.defaultPartNames
    static let defaultPartNames = ["Verse", "Chorus", "Bridge", "Part 4", "Part 5", "Part 6", "Part 7", "Part 8"]
    /// The Launchkey's Shift is held: the top pads switch parts.
    @Published private(set) var shiftDown = false
    /// The first loop recorded sets the tempo.
    @Published var baseSeconds: Double = 0
    @Published var baseLoop = -1
    @Published var bars = 0              // bars in the base loop, 0 = no tempo yet
    @Published var bpm: Double = 0
    @Published var cycleBars = 0
    @Published var positionText = "—"    // bar.beat.sixteenth in the cycle
    @Published var running = false
    @Published var quantize = UserDefaults.standard.object(forKey: "quantize") as? Bool ?? true {
        didSet {
            UserDefaults.standard.set(quantize, forKey: "quantize")
            applyTransportSettings()
        }
    }
    @Published var click = UserDefaults.standard.object(forKey: "click") as? Bool ?? true {
        didSet {
            UserDefaults.standard.set(click, forKey: "click")
            applyTransportSettings()
        }
    }
    private var barLength: Int64 = 0
    /// Kept here so volumes survive restarting the audio device.
    private var gains = [Float](repeating: 1, count: Int(ROUNDS_MAX_LOOPS))
    /// The Launchkey can't report where its knobs are, so the last value each
    /// knob sent is remembered across launches (-1 = never seen).
    private var knobValues = UserDefaults.standard.array(forKey: "knobValues") as? [Int]
        ?? [Int](repeating: -1, count: Int(ROUNDS_MAX_LOOPS))
    /// False while a loop's volume differs from its knob (changed in the window,
    /// or the knob moved while Rounds was closed): the knob then picks the
    /// volume up only once it passes it, instead of jumping.
    private var knobInControl = [Bool](repeating: true, count: Int(ROUNDS_MAX_LOOPS))

    /// Folder of the session last saved or opened (nil = never saved).
    @Published var sessionURL: URL?
    @Published var sessionBusy = false
    @Published var sessionMessage: String?
    var sessionName: String { sessionURL?.lastPathComponent ?? "Untitled" }

    @Published var log: [LogEntry] = []
    @Published var showLog = true

    let router = ControlRouter()
    private lazy var midi = MIDIInput(router: router)
    private var audio: OpaquePointer?
    private(set) var engine: OpaquePointer?
    private var pollTimer: Timer?
    /// Per loop. All 64 loops (8 parts of 8 slots) and 24 shared undo layers
    /// are allocated when audio starts: about 1 GB at 48 kHz.
    let maxLoopSeconds = 60.0
    /// The interface last started, so it is picked again when it is plugged back in.
    private var lastDeviceName: String?

    init() {
        refreshDevices()
        for (loop, value) in knobValues.enumerated() where value >= 0 { gains[loop] = Self.knobGain(value) }
        loops = loops.map { var l = $0; l.gain = gains[$0.id]; return l }
        var address = AudioObjectPropertyAddress(mSelector: kAudioHardwarePropertyDevices,
                                                 mScope: kAudioObjectPropertyScopeGlobal,
                                                 mElement: kAudioObjectPropertyElementMain)
        AudioObjectAddPropertyListenerBlock(AudioObjectID(kAudioObjectSystemObject), &address, .main) { [weak self] _, _ in
            MainActor.assumeIsolated { self?.devicesChanged() }
        }
        router.onGesture = { [weak self] gesture in self?.logGesture(gesture) }
        router.onPedal = { [weak self] down in self?.pedalDown = down }
        midi.onLog = { [weak self] line in self?.append("MIDI", line) }
        midi.onLaunchkey = { [weak self] control in self?.launchkey(control) }
        midi.onLaunchkeyReady = { [weak self] in self?.updateLaunchkeyLEDs(force: true) }
        NotificationCenter.default.addObserver(forName: NSApplication.willTerminateNotification, object: nil,
                                               queue: .main) { [weak self] _ in
            MainActor.assumeIsolated { self?.midi.releaseLaunchkey() }
        }
        do {
            try midi.start()
            append("App", "Listening to all MIDI inputs")
        } catch {
            append("App", "MIDI failed to start: \(error.localizedDescription)")
        }
        pollTimer = Timer.scheduledTimer(withTimeInterval: 1.0 / 30.0, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated { self?.poll() }
        }
    }

    // MARK: Devices and audio

    func refreshDevices() {
        var list: [AudioDevice] = []
        for i in 0..<rounds_audio_device_count() {
            var info = RoundsAudioDevice()
            guard rounds_audio_device_info(i, &info) == 1 else { continue }
            let name = withUnsafePointer(to: &info.name) {
                $0.withMemoryRebound(to: CChar.self, capacity: 128) { String(cString: $0) }
            }
            guard info.inputChannels > 0 else { continue }
            list.append(AudioDevice(id: info.deviceID, name: name,
                                    inputChannels: Int(info.inputChannels), outputChannels: Int(info.outputChannels)))
        }
        devices = list
        if !list.contains(where: { $0.id == deviceID }) {
            let preferred = rounds_audio_default_input_device()
            deviceID = list.first(where: { $0.id == preferred && $0.outputChannels > 0 })?.id
                ?? list.first(where: { $0.outputChannels > 0 })?.id
                ?? list.first?.id ?? 0
        }
    }

    /// An interface was plugged in or unplugged.
    private func devicesChanged() {
        let before = Set(devices.map(\.name))
        let runningID = audioRunning ? deviceID : nil
        refreshDevices()
        let after = Set(devices.map(\.name))
        for name in after.subtracting(before).sorted() { append("Audio", "Connected: \(name)") }
        for name in before.subtracting(after).sorted() { append("Audio", "Disconnected: \(name)") }

        if let runningID, !devices.contains(where: { $0.id == runningID }) {
            stopAudio()
            audioError = "The audio interface was disconnected. Plug it back in and press Start audio."
        }
        if !audioRunning, let name = lastDeviceName, let match = devices.first(where: { $0.name == name }) {
            deviceID = match.id
        }
    }

    var selectedDevice: AudioDevice? { devices.first { $0.id == deviceID } }

    /// Opening the device through the HAL doesn't reliably make macOS ask for
    /// microphone access; without it the input is silently all zeros. So ask
    /// explicitly, and refuse to start rather than record silence.
    func startAudio() {
        switch AVCaptureDevice.authorizationStatus(for: .audio) {
        case .authorized:
            micDenied = false
            openAudio()
        case .notDetermined:
            append("App", "Asking macOS for microphone access")
            Task {
                let granted = await AVCaptureDevice.requestAccess(for: .audio)
                append("App", granted ? "Microphone access granted" : "Microphone access denied")
                if granted {
                    micDenied = false
                    openAudio()
                } else {
                    refuseWithoutMic()
                }
            }
        default:
            refuseWithoutMic()
        }
    }

    private func refuseWithoutMic() {
        stopAudio()
        micDenied = true
        audioError = "Rounds can't hear your interface: microphone access is off. Turn on Rounds in "
            + "System Settings → Privacy & Security → Microphone, then restart Rounds."
        append("Audio", "Not started: no microphone access, the input would be silent")
    }

    func openMicrophoneSettings() {
        if let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_Microphone") {
            NSWorkspace.shared.open(url)
        }
    }

    private func openAudio() {
        stopAudio()
        var info = RoundsAudioInfo()
        var error = [CChar](repeating: 0, count: 256)
        let errorLength = Int32(error.count)
        guard let io = rounds_audio_open(deviceID, Int32(inputChannel), Int32(bufferFrames), maxLoopSeconds,
                                         &info, &error, errorLength) else {
            audioError = String(cString: error)
            append("Audio", "Couldn't open device: \(audioError ?? "")")
            return
        }
        audio = io
        engine = rounds_audio_engine(io)
        applyMonitor()
        applyTransportSettings()
        for loop in gains.indices { applyGain(loop, gains[loop]) }
        router.setEngine(engine)
        router.select(loop: selectedLoop)
        guard rounds_audio_start(io) == 1 else {
            audioError = "The device didn't start"
            append("Audio", "Device didn't start")
            stopAudio()
            return
        }
        audioError = nil
        audioRunning = true
        lastDeviceName = selectedDevice?.name
        sampleRate = info.sampleRate
        append("Audio", String(format: "Started %@ at %.0f Hz, %d-frame buffer, estimated latency in %d / out %d samples",
                               selectedDevice?.name ?? "device", info.sampleRate, info.bufferFrames,
                               info.inputLatency, info.outputLatency))
    }

    func stopAudio() {
        router.setEngine(nil)
        if let audio {
            rounds_audio_close(audio)
            append("Audio", "Stopped")
        }
        audio = nil
        engine = nil
        audioRunning = false
    }

    func measureLatency() {
        guard let engine else { return }
        rounds_engine_start_calibration(engine)
        append("Audio", "Measuring latency: playing 8 clicks. The output must be cabled back into the input.")
    }

    private static let clickGain: Float = 0.35

    private func applyTransportSettings() {
        guard let engine else { return }
        rounds_engine_set_quantize(engine, quantize ? 1 : 0)
        rounds_engine_set_click_gain(engine, click ? Self.clickGain : 0)
    }

    /// Takes the first loop as twice / half as many bars: doubles or halves the tempo.
    func doubleTempo() { setBars(bars * 2) }
    func halveTempo() { setBars(bars / 2) }

    func setBars(_ newBars: Int) {
        guard let engine, newBars >= 1, newBars <= 8 else { return }
        rounds_engine_set_bars(engine, Int32(newBars))
    }

    private func applyMonitor() {
        guard let engine else { return }
        rounds_engine_set_monitor_gain(engine, monitorInput ? 1.0 : 0.0)
    }

    /// Loop volume, linear (1 = as recorded).
    /// Volume from the window (or a loaded session): the knob no longer matches.
    func setGain(_ loop: Int, _ gain: Float) {
        knobInControl[loop] = false
        applyGain(loop, gain)
    }

    /// Volume belongs to the loop in the slot, so it follows a shared loop
    /// into other parts.
    private func applyGain(_ loop: Int, _ gain: Float) {
        gains[loop] = gain
        loops[loop].gain = gain
        if let engine {
            rounds_engine_set_loop_gain(engine, rounds_engine_part_loop(engine, Int32(currentPart), Int32(loop)), gain)
        }
    }

    /// After a part switch or a session load the knobs no longer match the loops.
    func knobsLetGo() {
        knobInControl = [Bool](repeating: false, count: knobInControl.count)
    }

    // MARK: Controls

    func tap() { router.send(ROUNDS_CMD_TAP) }
    func startStopAll() { router.startStopAll(hostTime: 0) }
    func undo() { router.send(ROUNDS_CMD_UNDO) }
    func stop() { router.send(ROUNDS_CMD_STOP) }
    func clear() { router.send(ROUNDS_CMD_CLEAR) }

    // MARK: Parts

    /// Switches at the end of the cycle; an unused part's pad makes a new part.
    func switchPart(_ part: Int) {
        guard let engine, part >= 0, part < parts.count else { return }
        if parts[part].exists {
            rounds_engine_switch_part(engine, Int32(part))
        } else {
            newPart()
        }
    }

    /// Copies the playing part (keeping shared loops) and switches to the copy.
    func newPart() {
        guard let engine else { return }
        rounds_engine_new_part(engine)
    }

    func setShared(_ shared: Bool) {
        guard let engine else { return }
        let loop = loops[selectedLoop]
        rounds_engine_set_shared(engine, Int32(loop.loopID), shared ? 1 : 0)
        loops[selectedLoop].shared = shared
        append("Loop \(selectedLoop + 1)", shared ? "Shared: new parts keep this loop" : "Not shared: new parts get a copy")
    }

    func renamePart(_ part: Int, to name: String) {
        let trimmed = name.trimmingCharacters(in: .whitespaces)
        guard !trimmed.isEmpty else { return }
        partNames[part] = trimmed
    }

    func partTitle(_ part: Int) -> String { "\(part + 1) · \(partNames[part])" }

    // MARK: Instruments

    /// The drum loop the pads record into: the selected loop if it is one,
    /// else the first.
    var drumLoop: Int? {
        if loops[selectedLoop].isDrumLoop { return selectedLoop }
        return loops.first(where: \.isDrumLoop)?.id
    }

    /// Keys play the selected loop if it is (or may become) a MIDI loop; an
    /// audio loop leaves them on their own. Pads go to the drum loop, or make
    /// the selected empty loop one.
    private func updateNoteRoutes() {
        let selected = loops[selectedLoop]
        // A loop recording audio still becomes MIDI if keys (or pads) come in.
        let open = selected.state == .empty || (selected.state == .recording && !selected.midi)
        let keysOnLoop = open || selected.midi
        let keys = ControlRouter.NoteRoute(loop: keysOnLoop ? Int32(selectedLoop) : -1, instrument: keysInstrument.rawValue)
        // A selected open loop takes the pads even when a drum loop exists, so
        // a second beat can be recorded; it starts on the drum loop's kit.
        let pads: ControlRouter.NoteRoute
        let padsKit = drumLoop.map { loops[$0].instrument } ?? kit
        if open {
            pads = ControlRouter.NoteRoute(loop: Int32(selectedLoop), instrument: padsKit.rawValue)
        } else if let drumLoop {
            pads = ControlRouter.NoteRoute(loop: Int32(drumLoop), instrument: loops[drumLoop].instrument.rawValue)
        } else {
            pads = ControlRouter.NoteRoute(loop: -1, instrument: kit.rawValue)
        }
        guard lastRoutes == nil || lastRoutes! != (keys, pads) else { return }
        lastRoutes = (keys, pads)
        router.setNoteRoutes(keys: keys, pads: pads)
    }

    private func followSelectedLoop() {
        let loop = loops[selectedLoop]
        if loop.midi && loop.state != .empty && !loop.instrument.isKit { keysInstrument = loop.instrument }
        updateNoteRoutes()
    }

    /// Picks what the keys play; a selected MIDI loop switches to it.
    func selectInstrument(_ instrument: Instrument) {
        if instrument.isKit { return selectKit(instrument) }
        keysInstrument = instrument
        let loop = loops[selectedLoop]
        if loop.midi && loop.state != .empty && !loop.instrument.isKit, let engine {
            rounds_engine_set_instrument(engine, Int32(selectedLoop), instrument.rawValue)
            loops[selectedLoop].instrument = instrument
            append("Loop \(selectedLoop + 1)", "Instrument: \(instrument.name)")
        } else {
            append("Keys", instrument.name)
        }
        updateNoteRoutes()
    }

    func selectKit(_ newKit: Instrument) {
        guard newKit.isKit else { return }
        kit = newKit
        if let drumLoop, let engine {
            rounds_engine_set_instrument(engine, Int32(drumLoop), newKit.rawValue)
            loops[drumLoop].instrument = newKit
            append("Loop \(drumLoop + 1)", "Kit: \(newKit.name)")
        } else {
            append("Pads", newKit.name)
        }
        updateNoteRoutes()
    }

    /// Track ◀ ▶: the next instrument for the keys, or kit if a drum loop is selected.
    func browseInstrument(_ step: Int) {
        let selected = loops[selectedLoop]
        if selected.isDrumLoop {
            let kits = Instrument.kits
            let index = kits.firstIndex(of: selected.instrument) ?? 0
            selectKit(kits[(index + step + kits.count) % kits.count])
        } else {
            let synths = Instrument.synths
            let index = synths.firstIndex(of: keysInstrument) ?? 0
            selectInstrument(synths[(index + step + synths.count) % synths.count])
        }
    }

    /// A pad clicked in the window (plays like the Launchkey's).
    func hitPad(_ pad: Int) {
        router.drum(DrumPads.notes[pad], velocity: 100, hostTime: 0)
        router.drum(DrumPads.notes[pad], velocity: 0, hostTime: 0)
        lastHit = (pad, 100, Date())
    }

    /// The space bar stands in for the footswitch.
    func spaceBar(down: Bool) { router.pedal(down: down, hostTime: 0) }

    // MARK: Launchkey

    /// Slot colours on the pads. Red is left out: it means recording.
    static let slotColors: [UInt8] = [
        Launchkey.Color.orange, Launchkey.Color.blue, Launchkey.Color.pink, Launchkey.Color.purple,
        Launchkey.Color.green, Launchkey.Color.cyan, Launchkey.Color.yellow, Launchkey.Color.white,
    ]
    /// Squared so the low end of the knob has finer control.
    static func knobGain(_ value: Int) -> Float {
        let fraction = Float(value) / 127
        return fraction * fraction
    }

    private func knobMoved(_ knob: Int, to value: Int) {
        let previous = knobValues[knob]
        knobValues[knob] = value
        UserDefaults.standard.set(knobValues, forKey: "knobValues")
        if !knobInControl[knob] {
            // Take over once the knob reaches or crosses the current volume.
            let target = Int((sqrt(loops[knob].gain) * 127).rounded())
            let crossed = previous >= 0 && (previous - target) * (value - target) <= 0
            guard crossed || abs(value - target) <= 2 else { return }
            knobInControl[knob] = true
            append("Launchkey", "Knob \(knob + 1) picked up loop \(knob + 1)'s volume")
        }
        applyGain(knob, Self.knobGain(value))
    }

    private var lastLEDState: [String]?
    private var lastLEDSelected = -1

    private func launchkey(_ control: Launchkey.Control) {
        switch control {
        case let .shift(down):
            shiftDown = down
            updateLaunchkeyLEDs(force: true)
        case let .loopPad(pad, down: true) where shiftDown:
            switchPart(pad)
        case let .loopPad(pad, down: true):
            selectedLoop = pad
        case let .knob(knob, value):
            knobMoved(knob, to: value)
        case let .drumPad(pad, velocity):
            midi.sendToLaunchkey([Launchkey.drumPadLED(pad, color: velocity > 0 ? Launchkey.Color.white : 0)])
            if velocity > 0 { lastHit = (pad, velocity, Date()) }
        case .trackLeft(down: true):
            browseInstrument(-1)
        case .trackRight(down: true):
            browseInstrument(1)
        case .padMode(let mode) where mode != 2:
            // Pads were switched away from Session (Shift menu); take them back.
            midi.sendToLaunchkey([Launchkey.padModeSession])
            updateLaunchkeyLEDs(force: true)
        default:
            break
        }
    }

    private func updateLaunchkeyLEDs(force: Bool = false) {
        let key = shiftDown
            ? ["parts \(currentPart) \(nextPart)"] + parts.map { "\($0.exists)" }
            : loops.map { "\($0.state)/\($0.queued)" }
        guard force || key != lastLEDState || selectedLoop != lastLEDSelected else { return }
        lastLEDState = key
        lastLEDSelected = selectedLoop
        var messages: [[UInt8]] = []
        if shiftDown {
            // Shift held: the top row shows the parts. Green = playing,
            // blinking green = next, white = another part, off = unused.
            for part in parts {
                let pad = part.id
                if pad == nextPart {
                    messages.append(Launchkey.loopPadLED(pad, color: Launchkey.Color.dimWhite))
                    messages.append(Launchkey.loopPadLED(pad, color: Launchkey.Color.green, mode: .flashing))
                } else if pad == currentPart {
                    messages.append(Launchkey.loopPadLED(pad, color: Launchkey.Color.green))
                } else {
                    messages.append(Launchkey.loopPadLED(pad, color: part.exists ? Launchkey.Color.dimWhite : Launchkey.Color.off))
                }
            }
            midi.sendToLaunchkey(messages)
            return
        }
        for loop in loops {
            let pad = loop.id
            let loopState = loop.state
            let color = Self.slotColors[pad]
            // Blinking = waiting for the next bar.
            switch (loop.queued, loopState) {
            case (.tap, .empty):
                messages.append(Launchkey.loopPadLED(pad, color: Launchkey.Color.off))
                messages.append(Launchkey.loopPadLED(pad, color: Launchkey.Color.red, mode: .flashing))
                continue
            case (.tap, _):
                messages.append(Launchkey.loopPadLED(pad, color: Launchkey.Color.dimWhite))
                messages.append(Launchkey.loopPadLED(pad, color: color, mode: .flashing))
                continue
            case (.stop, _):
                messages.append(Launchkey.loopPadLED(pad, color: color))
                messages.append(Launchkey.loopPadLED(pad, color: Launchkey.Color.off, mode: .flashing))
                continue
            case (.none, _):
                break
            }
            switch loopState {
            case .empty:
                messages.append(Launchkey.loopPadLED(pad, color: Launchkey.Color.off))
            case .recording:
                messages.append(Launchkey.loopPadLED(pad, color: Launchkey.Color.red))
            case .overdubbing:
                messages.append(Launchkey.loopPadLED(pad, color: color))
                messages.append(Launchkey.loopPadLED(pad, color: Launchkey.Color.red, mode: .flashing))
            case .playing:
                messages.append(Launchkey.loopPadLED(pad, color: color))
            case .stopped:
                messages.append(Launchkey.loopPadLED(pad, color: Launchkey.Color.dimWhite))
            }
            if pad == selectedLoop && loopState != .overdubbing {
                messages.append(Launchkey.loopPadLED(pad, color: Launchkey.Color.white, mode: .flashing))
            }
        }
        midi.sendToLaunchkey(messages)
    }

    // MARK: Polling

    private func poll() {
        guard let engine else {
            inputLevel = 0
            outputLevel = 0
            return
        }
        var s = RoundsStatus()
        rounds_engine_get_status(engine, &s)
        sampleRate = s.sampleRate
        baseSeconds = Double(s.baseLength) / s.sampleRate
        baseLoop = Int(s.baseLoop)
        if Int(s.bars) != bars, s.bars > 0 {
            append("Tempo", String(format: "%.1f BPM: loop %d is %d bar%@", 240 * s.sampleRate / Double(s.barLength),
                                   s.baseLoop + 1, s.bars, s.bars == 1 ? "" : "s"))
        }
        bars = Int(s.bars)
        barLength = s.barLength
        bpm = s.barLength > 0 ? 240 * s.sampleRate / Double(s.barLength) : 0
        cycleBars = s.barLength > 0 ? Int(s.cycleLength / s.barLength) : 0
        running = s.running != 0
        positionText = Self.position(s.cyclePosition, barLength: s.barLength)
        if Int(s.part) != currentPart {
            currentPart = Int(s.part)
            knobsLetGo()
        }
        nextPart = Int(s.nextPart)
        partSwitchBars = s.barLength > 0 && s.partSwitchIn > 0 ? Double(s.partSwitchIn) / Double(s.barLength) : 0
        var updatedParts = parts
        for i in updatedParts.indices {
            var p = RoundsPartStatus()
            rounds_engine_get_part_status(engine, Int32(i), &p)
            updatedParts[i].exists = p.exists != 0
            updatedParts[i].loops = Int(p.loops)
            updatedParts[i].cycleBars = s.barLength > 0 ? Int(p.cycleLength / s.barLength) : 0
        }
        if updatedParts != parts { parts = updatedParts }
        var updated = loops
        for i in updated.indices {
            var l = RoundsLoopStatus()
            rounds_engine_get_loop_status(engine, Int32(i), &l)
            let loopState = LoopState(rawValue: l.state) ?? .empty
            updated[i].state = loopState
            updated[i].seconds = Double(loopState == .recording ? l.recordedLength : l.loopLength) / s.sampleRate
            updated[i].position = l.loopLength > 0 ? Double(l.playPosition) / Double(l.loopLength) : 0
            updated[i].layers = Int(l.layers)
            updated[i].level = max(l.peak, updated[i].level * 0.8)
            updated[i].gain = l.gain
            updated[i].bars = s.barLength > 0 ? Int(l.loopLength / s.barLength) : 0
            updated[i].queued = LoopInfo.Queued(rawValue: l.queued) ?? .none
            updated[i].midi = l.kind == Int32(ROUNDS_KIND_MIDI.rawValue)
            updated[i].instrument = Instrument(rawValue: l.instrument) ?? .subBass
            updated[i].notes = Int(l.notes)
            updated[i].loopID = Int(l.loopID)
            updated[i].shared = l.shared != 0
            gains[i] = l.gain
            let sameLoop = loops[i].loopID == updated[i].loopID
            if sameLoop && updated[i].midi && loopState != .empty && !(loops[i].midi && loops[i].state != .empty) {
                append("Loop \(i + 1)", "Is a MIDI loop: \(updated[i].instrument.name)")
                if i == selectedLoop && !updated[i].instrument.isKit { keysInstrument = updated[i].instrument }
            }
        }
        let partChanged = updated.map(\.loopID) != loops.map(\.loopID)
        if updated != loops { loops = updated }
        if partChanged { followSelectedLoop() }
        updateNoteRoutes()
        let selected = loops[selectedLoop]
        state = selected.state
        loopSeconds = selected.state == .recording ? 0 : selected.seconds
        recordedSeconds = selected.state == .recording ? selected.seconds : 0
        position = selected.position
        layers = selected.layers
        inputLevel = max(s.inputPeak, inputLevel * 0.8)
        outputLevel = max(s.outputPeak, outputLevel * 0.8)
        latencyIn = Int(s.inputLatency)
        latencyOut = Int(s.outputLatency)
        calibrating = s.calibrating != 0
        updateLaunchkeyLEDs()

        var event = RoundsEvent()
        while rounds_engine_poll_event(engine, &event) == 1 {
            describe(event, sampleRate: s.sampleRate)
        }
    }

    private func describe(_ event: RoundsEvent, sampleRate: Double) {
        switch UInt32(event.type) {
        case ROUNDS_EVENT_STATE_CHANGED.rawValue:
            append("Loop \(event.loop + 1)", "→ \(LoopState(rawValue: Int32(event.value))?.label ?? "?")")
        case ROUNDS_EVENT_LOOP_LENGTH.rawValue where event.value > 0:
            var text = String(format: "Length %.3f s (%lld samples)", Double(event.value) / sampleRate, event.value)
            if barLength > 0 && Int(event.loop) != baseLoop {
                text += ", " + Self.barsText(Int(event.value / barLength))
            }
            append("Loop \(event.loop + 1)", text)
        case ROUNDS_EVENT_QUEUED.rawValue:
            let state = loops[Int(event.loop)].state
            switch LoopInfo.Queued(rawValue: Int32(event.value)) ?? .none {
            case .tap: append("Loop \(event.loop + 1)", state == .empty ? "Records at the next bar" : "Plays at the next bar")
            case .stop: append("Loop \(event.loop + 1)", "Stops at the next bar")
            case .none: break
            }
        case ROUNDS_EVENT_LAYER_COUNT.rawValue:
            append("Loop \(event.loop + 1)", "Layers: \(event.value)")
        case ROUNDS_EVENT_CALIBRATION_DONE.rawValue:
            if event.value < 0 {
                append("Audio", "Latency measurement failed: no clicks came back. Check the loopback cable and input level.")
            } else {
                append("Audio", String(format: "Measured round trip: %lld samples (%.1f ms). Using it from now on.",
                                       event.value, Double(event.value) * 1000 / sampleRate))
            }
        case ROUNDS_EVENT_PART_CHANGED.rawValue:
            append("Part", "\(partTitle(Int(event.value))) playing")
        case ROUNDS_EVENT_PART_QUEUED.rawValue:
            append("Part", event.value >= 0 ? "\(partTitle(Int(event.value))) starts at the end of the cycle"
                                            : "Part switch taken back")
        case ROUNDS_EVENT_PART_CREATED.rawValue:
            append("Part", event.value >= 0
                   ? "\(partTitle(Int(event.value))) made from \(partTitle(currentPart)): shared loops kept, others copied"
                   : "All 8 parts are used")
        case ROUNDS_EVENT_QUEUE_FULL.rawValue:
            append("Loop", "Command dropped (queue full)")
        default:
            break
        }
    }

    static func barsText(_ bars: Int) -> String { "\(bars) bar\(bars == 1 ? "" : "s")" }

    /// bar.beat.sixteenth (from 1), 4/4.
    static func position(_ samples: Int64, barLength: Int64) -> String {
        guard barLength > 0 else { return "—" }
        let sixteenths = Int(samples * 16 / barLength)
        return "\(sixteenths / 16 + 1).\(sixteenths / 4 % 4 + 1).\(sixteenths % 4 + 1)"
    }

    private func logGesture(_ gesture: RoundsGesture) {
        let name: String
        switch gesture {
        case ROUNDS_GESTURE_TAP: name = "Tap"
        case ROUNDS_GESTURE_DOUBLE_TAP: name = "Double-tap → undo"
        case ROUNDS_GESTURE_HOLD: name = "Hold → stop"
        case ROUNDS_GESTURE_LONG_HOLD: name = "Long hold → clear"
        default: return
        }
        append("Footswitch", name)
    }

    func append(_ source: String, _ text: String) {
        let entry = LogEntry(time: Date(), source: source, text: text)
        log.append(entry)
        if log.count > 500 { log.removeFirst(log.count - 500) }
        logFile.write(entry)
    }

    private let logFile = LogFile()
}

/// Mirrors the event log to ~/Library/Logs/Rounds/events.log (a new file each
/// launch) so a whole test session can be read back without copying from the UI.
final class LogFile: @unchecked Sendable {
    static let url = FileManager.default.homeDirectoryForCurrentUser
        .appendingPathComponent("Library/Logs/Rounds/events.log")

    private let queue = DispatchQueue(label: "rounds.logfile", qos: .utility)
    private var handle: FileHandle?
    private let format: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "yyyy-MM-dd HH:mm:ss.SSS"
        return f
    }()

    init() {
        queue.async {
            let fm = FileManager.default
            try? fm.createDirectory(at: Self.url.deletingLastPathComponent(), withIntermediateDirectories: true)
            fm.createFile(atPath: Self.url.path, contents: nil)
            self.handle = try? FileHandle(forWritingTo: Self.url)
        }
    }

    func write(_ entry: LogEntry) {
        queue.async {
            let line = "\(self.format.string(from: entry.time))  \(entry.source.padding(toLength: 10, withPad: " ", startingAt: 0))  \(entry.text)\n"
            self.handle?.write(Data(line.utf8))
        }
    }
}
