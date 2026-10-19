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

/// UI-side state. Polls the engine; never touches the audio thread directly.
@MainActor
final class LooperModel: ObservableObject {
    @Published var devices: [AudioDevice] = []
    @Published var deviceID: UInt32 = 0
    @Published var inputChannel = 0
    @Published var bufferFrames = 128
    @Published var audioRunning = false
    @Published var audioError: String?

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

    @Published var log: [LogEntry] = []
    @Published var showLog = true

    let router = ControlRouter()
    private lazy var midi = MIDIInput(router: router)
    private var audio: OpaquePointer?
    private var engine: OpaquePointer?
    private var pollTimer: Timer?
    private let maxLoopSeconds = 120.0

    init() {
        refreshDevices()
        router.onGesture = { [weak self] gesture in self?.logGesture(gesture) }
        router.onPedal = { [weak self] down in self?.pedalDown = down }
        midi.onLog = { [weak self] line in self?.append("MIDI", line) }
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

    var selectedDevice: AudioDevice? { devices.first { $0.id == deviceID } }

    func startAudio() {
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
        router.setEngine(engine)
        guard rounds_audio_start(io) == 1 else {
            audioError = "The device didn't start"
            append("Audio", "Device didn't start")
            stopAudio()
            return
        }
        audioError = nil
        audioRunning = true
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

    // MARK: Controls

    func tap() { router.send(ROUNDS_CMD_TAP) }
    func undo() { router.send(ROUNDS_CMD_UNDO) }
    func stop() { router.send(ROUNDS_CMD_STOP) }
    func clear() { router.send(ROUNDS_CMD_CLEAR) }

    /// The space bar stands in for the footswitch.
    func spaceBar(down: Bool) { router.pedal(down: down, hostTime: 0) }

    // MARK: Polling

    private func poll() {
        guard let engine else {
            inputLevel = 0
            outputLevel = 0
            return
        }
        var s = RoundsStatus()
        rounds_engine_get_status(engine, &s)
        state = LoopState(rawValue: s.state) ?? .empty
        sampleRate = s.sampleRate
        loopSeconds = Double(s.loopLength) / s.sampleRate
        recordedSeconds = Double(s.recordedLength) / s.sampleRate
        position = s.loopLength > 0 ? Double(s.playPosition) / Double(s.loopLength) : 0
        layers = Int(s.layers)
        inputLevel = max(s.inputPeak, inputLevel * 0.8)
        outputLevel = max(s.outputPeak, outputLevel * 0.8)
        latencyIn = Int(s.inputLatency)
        latencyOut = Int(s.outputLatency)
        calibrating = s.calibrating != 0

        var event = RoundsEvent()
        while rounds_engine_poll_event(engine, &event) == 1 {
            describe(event, sampleRate: s.sampleRate)
        }
    }

    private func describe(_ event: RoundsEvent, sampleRate: Double) {
        switch UInt32(event.type) {
        case ROUNDS_EVENT_STATE_CHANGED.rawValue:
            append("Loop", "→ \(LoopState(rawValue: Int32(event.value))?.label ?? "?")")
        case ROUNDS_EVENT_LOOP_LENGTH.rawValue where event.value > 0:
            append("Loop", String(format: "Length %.3f s (%lld samples)", Double(event.value) / sampleRate, event.value))
        case ROUNDS_EVENT_LAYER_COUNT.rawValue:
            append("Loop", "Layers: \(event.value)")
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
        log.append(LogEntry(time: Date(), source: source, text: text))
        if log.count > 500 { log.removeFirst(log.count - 500) }
    }
}
