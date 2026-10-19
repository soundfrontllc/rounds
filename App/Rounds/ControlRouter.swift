import Foundation

/// Routes footswitch and button input to the engine. Lives off the main
/// thread so a busy UI never delays a footswitch press.
final class ControlRouter: @unchecked Sendable {
    private let queue = DispatchQueue(label: "rounds.control", qos: .userInteractive)
    private let footswitch = rounds_footswitch_create(rounds_footswitch_default_config())
    private var timer: DispatchSourceTimer?
    private var engine: OpaquePointer?
    private var pedalDown = false

    /// Called on the main thread with each gesture, for the event log and UI.
    var onGesture: ((RoundsGesture) -> Void)?
    var onPedal: ((Bool) -> Void)?

    init() {
        let timer = DispatchSource.makeTimerSource(queue: queue)
        timer.schedule(deadline: .now(), repeating: .milliseconds(10))
        timer.setEventHandler { [weak self] in self?.tick() }
        timer.resume()
        self.timer = timer
    }

    deinit {
        timer?.cancel()
        rounds_footswitch_destroy(footswitch)
    }

    func setEngine(_ engine: OpaquePointer?) {
        queue.sync { self.engine = engine }
    }

    /// Footswitch state change. hostTime is the CoreMIDI timestamp (0 = now).
    func pedal(down: Bool, hostTime: UInt64) {
        let stamp = hostTime == 0 ? rounds_host_time_now() : hostTime
        queue.async {
            guard down != self.pedalDown else { return }
            self.pedalDown = down
            let seconds = rounds_host_time_to_seconds(stamp)
            let gesture = down
                ? rounds_footswitch_press(self.footswitch, seconds)
                : rounds_footswitch_release(self.footswitch, seconds)
            self.handle(gesture, hostTime: stamp)
            DispatchQueue.main.async { self.onPedal?(down) }
        }
    }

    /// A button in the UI. Sent "now", not scheduled.
    func send(_ command: RoundsCommand) {
        queue.async {
            guard let engine = self.engine else { return }
            rounds_engine_send(engine, command, -1)
        }
    }

    private func tick() {
        let now = rounds_host_time_now()
        let gesture = rounds_footswitch_tick(footswitch, rounds_host_time_to_seconds(now))
        handle(gesture, hostTime: now)
    }

    private func handle(_ raw: Int32, hostTime: UInt64) {
        guard raw != Int32(ROUNDS_GESTURE_NONE.rawValue) else { return }
        let gesture = RoundsGesture(rawValue: UInt32(raw))
        let command = rounds_gesture_command(raw)
        if command >= 0, let engine {
            if gesture == ROUNDS_GESTURE_TAP || gesture == ROUNDS_GESTURE_DOUBLE_TAP {
                // Presses are scheduled at the moment the pedal moved.
                rounds_engine_send_at_host_time(engine, RoundsCommand(rawValue: UInt32(command)), hostTime)
            } else {
                rounds_engine_send(engine, RoundsCommand(rawValue: UInt32(command)), -1)
            }
        }
        DispatchQueue.main.async { self.onGesture?(gesture) }
    }
}
