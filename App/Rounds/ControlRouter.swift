import Foundation

/// Routes footswitch and button input to the engine. Lives off the main
/// thread so a busy UI never delays a footswitch press.
final class ControlRouter: @unchecked Sendable {
    private let queue = DispatchQueue(label: "rounds.control", qos: .userInteractive)
    private let footswitch = rounds_footswitch_create(rounds_footswitch_default_config())
    private var timer: DispatchSourceTimer?
    private var engine: OpaquePointer?
    private var pedalDown = false
    /// The loop the footswitch, Record and the window's buttons act on.
    private var selected: Int32 = 0
    private var gestureLoop: Int32 = 0

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

    /// Queued behind any pedal press already received, so a press made just
    /// before selecting still acts on the loop that was selected at the time.
    func select(loop: Int) {
        queue.async { self.selected = Int32(loop) }
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

    /// A single tap from a hardware button (the Launchkey's Record), scheduled
    /// at the moment it was pressed like a footswitch tap, without gestures.
    func tap(hostTime: UInt64) {
        let stamp = hostTime == 0 ? rounds_host_time_now() : hostTime
        queue.async {
            guard let engine = self.engine else { return }
            rounds_engine_send_at_host_time(engine, self.selected, ROUNDS_CMD_TAP, stamp)
        }
    }

    /// A button in the UI. Sent "now", not scheduled.
    func send(_ command: RoundsCommand) {
        queue.async {
            guard let engine = self.engine else { return }
            rounds_engine_send(engine, self.selected, command, -1)
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
        // Double-tap and hold revert their tap first, so they go to the tap's loop
        // even if another pad was selected in between.
        if gesture == ROUNDS_GESTURE_TAP { gestureLoop = selected }
        if command >= 0, let engine {
            if gesture == ROUNDS_GESTURE_TAP || gesture == ROUNDS_GESTURE_DOUBLE_TAP {
                // Presses are scheduled at the moment the pedal moved.
                rounds_engine_send_at_host_time(engine, gestureLoop, RoundsCommand(rawValue: UInt32(command)), hostTime)
            } else {
                rounds_engine_send(engine, gestureLoop, RoundsCommand(rawValue: UInt32(command)), -1)
            }
        }
        DispatchQueue.main.async { self.onGesture?(gesture) }
    }
}
