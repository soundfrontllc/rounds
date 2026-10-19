import Foundation
import MIDIKitIO

/// Listens to every MIDI source (the Launchkey shows up as two ports) and
/// forwards the footswitch to the control router. Everything received goes
/// to the event log so mapping problems are visible.
final class MIDIInput: @unchecked Sendable {
    private let manager = MIDIManager(clientName: "Rounds", model: "Rounds", manufacturer: "Soundfront")
    private let router: ControlRouter

    /// Called on the main thread with a readable line per message.
    var onLog: ((String) -> Void)?
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
                self?.received(events, timeStamp: timeStamp, sourceName: source?.displayName ?? "?")
            }
        )
    }

    private func received(_ events: [MIDIEvent], timeStamp: CoreMIDITimeStamp, sourceName: String) {
        for event in events {
            // Sustain jack: CC 64.
            if case let .cc(payload) = event, payload.controller.number == 64 {
                let value = Int(payload.value.midi1Value)
                var down = value >= 64
                if invertPedal { down.toggle() }
                router.pedal(down: down, hostTime: timeStamp)
            }
            if case .timingClock = event { continue }
            if case .activeSensing = event { continue }
            let line = "\(sourceName): \(event)"
            DispatchQueue.main.async { [weak self] in self?.onLog?(line) }
        }
    }
}
