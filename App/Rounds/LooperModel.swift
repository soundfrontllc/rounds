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
            updateLaunchkeyLEDs()
        }
    }
    @Published var loops: [LoopInfo] = (0..<Int(ROUNDS_MAX_LOOPS)).map { LoopInfo(id: $0) }
    /// Length later loops snap to, set by the first loop recorded.
    @Published var baseSeconds: Double = 0
    @Published var baseLoop = -1
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
    /// Per loop slot. All 8 slots' buffers (with 8 undo layers each) are
    /// allocated when audio starts: about 1 GB at 48 kHz.
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

    private func applyGain(_ loop: Int, _ gain: Float) {
        gains[loop] = gain
        loops[loop].gain = gain
        if let engine { rounds_engine_set_loop_gain(engine, Int32(loop), gain) }
    }

    // MARK: Controls

    func tap() { router.send(ROUNDS_CMD_TAP) }
    func undo() { router.send(ROUNDS_CMD_UNDO) }
    func stop() { router.send(ROUNDS_CMD_STOP) }
    func clear() { router.send(ROUNDS_CMD_CLEAR) }

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
            let target = Int((sqrt(gains[knob]) * 127).rounded())
            let crossed = previous >= 0 && (previous - target) * (value - target) <= 0
            guard crossed || abs(value - target) <= 2 else { return }
            knobInControl[knob] = true
            append("Launchkey", "Knob \(knob + 1) picked up loop \(knob + 1)'s volume")
        }
        applyGain(knob, Self.knobGain(value))
    }

    private var lastLEDState: [LoopState]?
    private var lastLEDSelected = -1

    private func launchkey(_ control: Launchkey.Control) {
        switch control {
        case let .loopPad(pad, down: true):
            selectedLoop = pad
        case let .knob(knob, value):
            knobMoved(knob, to: value)
        case let .drumPad(pad, velocity):
            midi.sendToLaunchkey([Launchkey.drumPadLED(pad, color: velocity > 0 ? Launchkey.Color.white : 0)])
        case .padMode(let mode) where mode != 2:
            // Pads were switched away from Session (Shift menu); take them back.
            midi.sendToLaunchkey([Launchkey.padModeSession])
            updateLaunchkeyLEDs(force: true)
        default:
            break
        }
    }

    private func updateLaunchkeyLEDs(force: Bool = false) {
        let states = loops.map(\.state)
        guard force || states != lastLEDState || selectedLoop != lastLEDSelected else { return }
        lastLEDState = states
        lastLEDSelected = selectedLoop
        var messages: [[UInt8]] = []
        for (pad, loopState) in states.enumerated() {
            let color = Self.slotColors[pad]
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
        }
        if updated != loops { loops = updated }
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
            if baseLoop >= 0 && baseLoop != Int(event.loop) && baseSeconds > 0 {
                text += String(format: ", %@ of the base", ratioText(Double(event.value) / sampleRate / baseSeconds))
            }
            append("Loop \(event.loop + 1)", text)
        case ROUNDS_EVENT_LAYER_COUNT.rawValue:
            append("Loop \(event.loop + 1)", "Layers: \(event.value)")
        case ROUNDS_EVENT_CALIBRATION_DONE.rawValue:
            if event.value < 0 {
                append("Audio", "Latency measurement failed: no clicks came back. Check the loopback cable and input level.")
            } else {
                append("Audio", String(format: "Measured round trip: %lld samples (%.1f ms). Using it from now on.",
                                       event.value, Double(event.value) * 1000 / sampleRate))
            }
        case ROUNDS_EVENT_QUEUE_FULL.rawValue:
            append("Loop", "Command dropped (queue full)")
        default:
            break
        }
    }

    func ratioText(_ ratio: Double) -> String {
        ratio >= 1 ? String(format: "%.0f×", ratio) : "1/\(Int((1 / ratio).rounded()))×"
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
