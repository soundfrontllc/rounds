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

    /// Where keys and pads play: a loop slot (-1 = no loop, just the instrument).
    struct NoteRoute: Equatable {
        var loop: Int32
        var instrument: Int32
    }
    private var keysRoute = NoteRoute(loop: -1, instrument: Instrument.subBass.rawValue)
    private var padsRoute = NoteRoute(loop: -1, instrument: Instrument.dryKit.rawValue)
    /// The route each held note went down on, so its release goes there too.
    private var heldKeys: [Int32: NoteRoute] = [:]
    private var heldDrums: [Int32: NoteRoute] = [:]

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

    /// The Launchkey's Play button (or the window's): stop every loop on the
    /// next bar, or start every stopped loop together.
    func startStopAll(hostTime: UInt64) {
        let stamp = hostTime == 0 ? rounds_host_time_now() : hostTime
        queue.async {
            guard let engine = self.engine else { return }
            rounds_engine_send_at_host_time(engine, -1, ROUNDS_CMD_START_STOP_ALL, stamp)
        }
    }

    func setNoteRoutes(keys: NoteRoute, pads: NoteRoute) {
        queue.async {
            self.keysRoute = keys
            self.padsRoute = pads
        }
    }

    /// A key on the keyboard (velocity 0 = released), stamped with its MIDI time.
    func key(_ note: Int32, velocity: Int32, hostTime: UInt64) {
        let stamp = hostTime == 0 ? rounds_host_time_now() : hostTime
        queue.async { self.play(note, velocity: velocity, stamp: stamp, route: self.keysRoute, held: &self.heldKeys) }
    }

    /// A drum pad (the Launchkey's, or one clicked in the window).
    func drum(_ note: Int32, velocity: Int32, hostTime: UInt64) {
        let stamp = hostTime == 0 ? rounds_host_time_now() : hostTime
        queue.async { self.play(note, velocity: velocity, stamp: stamp, route: self.padsRoute, held: &self.heldDrums) }
    }

    private func play(_ note: Int32, velocity: Int32, stamp: UInt64, route: NoteRoute, held: inout [Int32: NoteRoute]) {
        guard let engine else { return }
        var target = route
        if velocity > 0 {
            held[note] = route
        } else if let down = held.removeValue(forKey: note) {
            target = down
        }
        rounds_engine_note(engine, target.loop, target.instrument, note, velocity, stamp)
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
