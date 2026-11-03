import Foundation
import MIDIKitIO

/// Listens to every MIDI source (the Launchkey shows up as two ports) and
/// forwards the footswitch to the control router. Takes the Launchkey over in
/// DAW mode through its DAW port. Everything received goes to the event log so
/// mapping problems are visible.
final class MIDIInput: @unchecked Sendable {
    private static let launchkeyTag = "LaunchkeyDAW"

    private lazy var manager = MIDIManager(
        clientName: "Rounds", model: "Rounds", manufacturer: "Soundfront",
        notificationHandler: { [weak self] notification in
            switch notification {
            case .added, .removed, .setupChanged: self?.checkLaunchkey()
            default: break
            }
        }
    )
    private let router: ControlRouter
    private var launchkeyConnected = false

    /// Called on the main thread with a readable line per message.
    var onLog: ((String) -> Void)?
    /// Called on the main thread with each Launchkey control in DAW mode.
    var onLaunchkey: ((Launchkey.Control) -> Void)?
    /// Called on the main thread when the Launchkey has (re)entered DAW mode.
    var onLaunchkeyReady: (() -> Void)?
    /// The FC5's polarity depends on how the Launchkey detected it at power-up.
    var invertPedal = false

    init(router: ControlRouter) {
        self.router = router
    }

    func start() throws {
        try manager.start()
        try manager.addInputConnection(
            to: .allOutputs,
            tag: "AllSources",
            receiver: .events { [weak self] events, timeStamp, source in
                self?.received(events, timeStamp: timeStamp, source: source)
            }
        )
        try manager.addOutputConnection(to: .inputs(matching: [.name(Launchkey.dawInName)]), tag: Self.launchkeyTag)
        checkLaunchkey()
    }

    /// Sends raw bytes to the Launchkey's DAW port (no-op when it isn't connected).
    func sendToLaunchkey(_ messages: [[UInt8]]) {
        guard let connection = manager.managedOutputConnections[Self.launchkeyTag] else { return }
        try? connection.send(events: messages.compactMap(Self.event))
    }

    /// MIDIKit only sends events, so rebuild the guide's note and CC bytes as events.
    private static func event(_ bytes: [UInt8]) -> MIDIEvent? {
        guard bytes.count == 3 else { return nil }
        let channel = UInt4(bytes[0] & 0x0F)
        let data1 = UInt7(bytes[1] & 0x7F)
        let data2 = UInt7(bytes[2] & 0x7F)
        switch bytes[0] & 0xF0 {
        case 0x90: return .noteOn(data1, velocity: .midi1(data2), channel: channel)
        case 0xB0: return .cc(data1, value: .midi1(data2), channel: channel)
        default: return nil
        }
    }

    /// Back to standalone mode, so the Launchkey works normally after Rounds quits.
    func releaseLaunchkey() {
        sendToLaunchkey([Launchkey.exitDAWMode])
    }

    // Endpoints change in bursts as a USB device appears; settle before checking.
    private func checkLaunchkey() {
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { [weak self] in
            guard let self else { return }
            let present = !(self.manager.managedOutputConnections[Self.launchkeyTag]?.endpoints.isEmpty ?? true)
            guard present != self.launchkeyConnected else { return }
            self.launchkeyConnected = present
            if present {
                self.sendToLaunchkey([Launchkey.enterDAWMode, Launchkey.padModeSession, Launchkey.potModeVolume])
                self.onLog?("Launchkey connected: entered DAW mode (pads: Session, knobs: Volume)")
                self.onLaunchkeyReady?()
            } else {
                self.onLog?("Launchkey disconnected")
            }
        }
    }

    private func received(_ events: [MIDIEvent], timeStamp: CoreMIDITimeStamp, source: MIDIOutputEndpoint?) {
        let fromDAWPort = source?.name == Launchkey.dawOutName
        let sourceName = source?.displayName ?? "?"
        for event in events {
            if case .timingClock = event { continue }
            if case .activeSensing = event { continue }
            if fromDAWPort, let control = Launchkey.parse(event.midi1RawBytes()) {
                // Record is the footswitch's tap; keep the MIDI timestamp for timing.
                if control == .record(down: true) { router.tap(hostTime: timeStamp) }
                if control == .play(down: true) { router.startStopAll(hostTime: timeStamp) }
                if case let .drumPad(pad, velocity) = control {
                    router.drum(DrumPads.notes[pad], velocity: Int32(velocity), hostTime: timeStamp)
                }
                // A held pad streams pressure every ~10 ms; it would bury the log.
                if control == .padPressure { continue }
                DispatchQueue.main.async { [weak self] in
                    self?.onLog?("Launchkey: \(control.logText)")
                    self?.onLaunchkey?(control)
                }
                continue
            }
            // Sustain jack: CC 64 (stays on the MIDI port in DAW mode).
            if case let .cc(payload) = event, payload.controller.number == 64 {
                let value = Int(payload.value.midi1Value)
                var down = value >= 64
                if invertPedal { down.toggle() }
                router.pedal(down: down, hostTime: timeStamp)
            }
            // Keys (the Launchkey's MIDI port, or any other keyboard), on any
            // channel: Corey's Launchkey sends its keys on channel 10.
            switch event {
            case let .noteOn(payload):
                router.key(Int32(payload.note.number), velocity: Int32(payload.velocity.midi1Value), hostTime: timeStamp)
            case let .noteOff(payload):
                router.key(Int32(payload.note.number), velocity: 0, hostTime: timeStamp)
            default:
                break
            }
            let line = "\(sourceName): \(event)"
            DispatchQueue.main.async { [weak self] in self?.onLog?(line) }
        }
    }
}
