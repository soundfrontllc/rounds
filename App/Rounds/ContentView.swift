import SwiftUI

struct ContentView: View {
    @EnvironmentObject var model: LooperModel
    @FocusState private var focused: Bool

    var body: some View {
        VStack(spacing: 0) {
            header
            Divider()
            transport
            Divider()
            HStack(alignment: .top, spacing: 16) {
                VStack(alignment: .leading, spacing: 16) {
                    LoopLanes()
                    loopPanel
                    controls
                    footswitchLegend
                }
                .frame(maxWidth: .infinity, alignment: .topLeading)
                if model.showLog {
                    EventLogView()
                        .frame(width: 420)
                }
            }
            .padding(16)
        }
        .frame(minWidth: 980, minHeight: 820)
        .focusable()
        .focused($focused)
        .focusEffectDisabled()
        .onAppear { focused = true }
        .onKeyPress(keys: [.space], phases: [.down, .up]) { press in
            model.spaceBar(down: press.phase == .down)
            return .handled
        }
    }

    // MARK: Header

    private var header: some View {
        HStack(spacing: 12) {
            Text("Rounds").font(.title2.weight(.semibold))
            Text(model.sessionName).font(.title3).foregroundStyle(.secondary)
            if model.sessionBusy { ProgressView().controlSize(.small) }
            Button("Open…") { model.openSession() }
                .disabled(!model.audioRunning || model.sessionBusy)
                .help("Open a saved session (⌘O)")
            Button("Save") { model.saveSession() }
                .disabled(!model.audioRunning || model.sessionBusy)
                .help("Save the loops and volumes to a session folder (⌘S)")
            Spacer()
            Picker("Interface", selection: $model.deviceID) {
                ForEach(model.devices) { device in
                    Text(device.name).tag(device.id)
                }
            }
            .frame(width: 240)
            Picker("Input", selection: $model.inputChannel) {
                ForEach(0..<(model.selectedDevice?.inputChannels ?? 1), id: \.self) { channel in
                    Text("In \(channel + 1)").tag(channel)
                }
            }
            .frame(width: 120)
            Picker("Buffer", selection: $model.bufferFrames) {
                ForEach([64, 128, 256, 512], id: \.self) { Text("\($0)").tag($0) }
            }
            .frame(width: 120)
            Button(model.audioRunning ? "Restart audio" : "Start audio") { model.startAudio() }
                .keyboardShortcut("r", modifiers: .command)
                .disabled(model.sessionBusy)
            if model.audioRunning {
                Button("Stop audio") { model.stopAudio() }
                    .disabled(model.sessionBusy)
            }
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 10)
    }

    // MARK: Transport

    private var transport: some View {
        HStack(spacing: 14) {
            Button(action: model.startStopAll) {
                Label(model.running ? "Stop all" : "Play all", systemImage: model.running ? "stop.fill" : "play.fill")
            }
            .disabled(!model.audioRunning || model.bars == 0)
            .help("Launchkey Play: stop every loop on the next bar, or start them all from the top")
            HStack(spacing: 6) {
                Text(tempoText).font(.system(.body, design: .monospaced).weight(.semibold))
                if model.bars > 0 {
                    Text("set by loop \(model.baseLoop + 1)").foregroundStyle(.secondary)
                    Button("½×") { model.halveTempo() }
                        .disabled(model.bars <= 1)
                        .help("Half the tempo: take loop \(model.baseLoop + 1) as half as many bars")
                    Button("2×") { model.doubleTempo() }
                        .disabled(model.bars >= 8)
                        .help("Double the tempo: take loop \(model.baseLoop + 1) as twice as many bars")
                }
            }
            Text(positionLabel)
                .font(.system(.body, design: .monospaced))
                .frame(width: 150, alignment: .leading)
            Spacer()
            Toggle("Quantize", isOn: $model.quantize)
                .help("Record, play and stop wait for the next bar while anything plays")
            Toggle("Click", isOn: $model.click)
                .help("A click on every beat while loops play")
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 8)
    }

    private var tempoText: String {
        model.bars > 0 ? String(format: "%.1f BPM", model.bpm) : "Tempo: set by the first loop"
    }

    private var positionLabel: String {
        guard model.bars > 0 else { return "" }
        return "\(model.running ? model.positionText : "—") / \(LooperModel.barsText(model.cycleBars))"
    }

    // MARK: Loop

    private var loopPanel: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(alignment: .firstTextBaseline) {
                Text("Loop \(model.selectedLoop + 1)").font(.headline)
                StateBadge(state: model.state)
                Spacer()
                Text(lengthText).font(.system(.title3, design: .monospaced))
                Text("· \(model.layers) layer\(model.layers == 1 ? "" : "s")").foregroundStyle(.secondary)
            }
            ProgressView(value: model.state == .recording ? 1 : model.position)
                .tint(model.state.color)
                .animation(nil, value: model.position)
            HStack(spacing: 16) {
                LevelMeter(label: "In", level: model.inputLevel)
                LevelMeter(label: "Out", level: model.outputLevel)
            }
            HStack(spacing: 12) {
                Text(latencyText).font(.callout).foregroundStyle(.secondary)
                Button(model.calibrating ? "Measuring…" : "Measure latency") { model.measureLatency() }
                    .disabled(!model.audioRunning || model.calibrating)
                    .help("Cable an output back into the selected input, then press. Rounds plays 8 clicks and times them.")
                Spacer()
                Toggle("Monitor input", isOn: $model.monitorInput)
                    .help("Play the input through Rounds. Leave off if your interface or GP-5 already monitors.")
            }
            if let message = model.sessionMessage {
                Text(message).foregroundStyle(.red).font(.callout)
            }
            if let error = model.audioError {
                HStack {
                    Text(error).foregroundStyle(.red).font(.callout)
                    if model.micDenied {
                        Button("Open Microphone settings") { model.openMicrophoneSettings() }
                    }
                }
            }
        }
        .padding(16)
        .background(RoundedRectangle(cornerRadius: 12).fill(Color(nsColor: .controlBackgroundColor)))
    }

    private var lengthText: String {
        switch model.state {
        case .empty: return "—"
        case .recording: return String(format: "%.2f s", model.recordedSeconds)
        default: return String(format: "%.2f s", model.loopSeconds)
        }
    }

    private var latencyText: String {
        let total = model.latencyIn + model.latencyOut
        let ms = Double(total) * 1000 / model.sampleRate
        return String(format: "Latency compensation: %d samples (%.1f ms)", total, ms)
    }

    // MARK: Controls

    private var controls: some View {
        HStack(spacing: 10) {
            Button(action: model.tap) { Label(tapLabel, systemImage: "record.circle") }
                .controlSize(.large)
            Button(action: model.undo) { Label("Undo", systemImage: "arrow.uturn.backward") }
                .controlSize(.large)
            Button(action: model.stop) { Label("Stop", systemImage: "stop.fill") }
                .controlSize(.large)
            Button(role: .destructive, action: model.clear) { Label("Clear", systemImage: "trash") }
                .controlSize(.large)
            Spacer()
            Toggle("Event log", isOn: $model.showLog).toggleStyle(.switch)
        }
        .disabled(!model.audioRunning)
    }

    private var tapLabel: String {
        switch model.state {
        case .empty: return "Record"
        case .recording: return "Close loop"
        case .overdubbing: return "Play"
        case .playing: return "Overdub"
        case .stopped: return "Play"
        }
    }

    private var footswitchLegend: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Circle()
                    .fill(model.pedalDown ? Color.red : Color.secondary.opacity(0.3))
                    .frame(width: 12, height: 12)
                Text("Footswitch").font(.headline)
                Text("(space bar works too)").foregroundStyle(.secondary).font(.callout)
                Spacer()
                Toggle("Invert pedal", isOn: $model.invertPedal)
                    .help("Turn on if the pedal acts backwards (fires on release).")
            }
            LegendRow(gesture: "Tap", action: "Record → Play → Overdub")
            LegendRow(gesture: "Double-tap", action: "Undo last layer")
            LegendRow(gesture: "Hold", action: "Stop")
            LegendRow(gesture: "Hold 2 s", action: "Clear")
        }
        .padding(16)
        .background(RoundedRectangle(cornerRadius: 12).fill(Color(nsColor: .controlBackgroundColor)))
    }
}

// MARK: - Loop lanes

extension LoopInfo {
    /// Matches the slot's pad colour on the Launchkey (LooperModel.slotColors).
    var color: Color {
        [Color.orange, .blue, .pink, .purple, .green, .cyan, .yellow, .gray][id]
    }
}

struct LoopLanes: View {
    @EnvironmentObject var model: LooperModel

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text("Loops").font(.headline)
                Spacer()
                Text(baseText).font(.callout).foregroundStyle(.secondary)
            }
            ForEach(model.loops) { loop in
                LoopLane(loop: loop, selected: loop.id == model.selectedLoop)
                    .contentShape(Rectangle())
                    .onTapGesture { model.selectedLoop = loop.id }
            }
        }
        .padding(12)
        .background(RoundedRectangle(cornerRadius: 12).fill(Color(nsColor: .controlBackgroundColor)))
    }

    private var baseText: String {
        guard model.bars > 0 else { return "The first loop recorded sets the tempo; the others snap to whole bars" }
        return "Loops snap to 1, 2, 4, 8 … bars"
    }
}

struct LoopLane: View {
    @EnvironmentObject var model: LooperModel
    let loop: LoopInfo
    let selected: Bool

    var body: some View {
        HStack(spacing: 10) {
            Text("\(loop.id + 1)")
                .font(.system(.callout, design: .monospaced).weight(.semibold))
                .frame(width: 22, height: 22)
                .background(Circle().fill(loop.state == .empty ? Color.secondary.opacity(0.2) : loop.color))
            Text(stateText)
                .font(.callout)
                .foregroundStyle(loop.queued == .none ? loop.state.color : .orange)
                .frame(width: 110, alignment: .leading)
            Text(lengthText)
                .font(.system(.callout, design: .monospaced))
                .frame(width: 120, alignment: .leading)
            ProgressView(value: loop.state == .recording ? 1 : loop.position)
                .tint(loop.state == .empty ? .secondary : loop.color)
                .animation(nil, value: loop.position)
            LevelMeter(label: "", level: loop.level).frame(width: 70)
            Slider(value: Binding(get: { Double(loop.gain) }, set: { model.setGain(loop.id, Float($0)) }), in: 0...1)
                .frame(width: 110)
                .help("Volume (knob \(loop.id + 1) on the Launchkey)")
        }
        .padding(.horizontal, 8)
        .padding(.vertical, 3)
        .background(RoundedRectangle(cornerRadius: 6).fill(selected ? Color.accentColor.opacity(0.15) : .clear))
    }

    private var lengthText: String {
        switch loop.state {
        case .empty: return "—"
        case .recording: return String(format: "%.2f s", loop.seconds)
        default:
            guard loop.bars > 0 else { return String(format: "%.2f s", loop.seconds) }
            return String(format: "%.2f s  %@", loop.seconds, LooperModel.barsText(loop.bars))
        }
    }

    private var stateText: String {
        switch loop.queued {
        case .none: return loop.state.label
        case .tap: return loop.state == .empty ? "Record next bar" : "Play next bar"
        case .stop: return "Stop next bar"
        }
    }
}

// MARK: - Pieces

extension LoopState {
    var color: Color {
        switch self {
        case .empty: return .secondary
        case .recording, .overdubbing: return .red
        case .playing: return .green
        case .stopped: return .orange
        }
    }
}

struct StateBadge: View {
    let state: LoopState
    var body: some View {
        Text(state.label)
            .font(.headline)
            .padding(.horizontal, 12)
            .padding(.vertical, 5)
            .background(Capsule().fill(state.color.opacity(0.18)))
            .foregroundStyle(state.color)
    }
}

struct LevelMeter: View {
    let label: String
    let level: Float
    var body: some View {
        HStack(spacing: 6) {
            Text(label).font(.caption).foregroundStyle(.secondary).frame(width: 24, alignment: .leading)
            GeometryReader { geo in
                ZStack(alignment: .leading) {
                    Capsule().fill(Color.secondary.opacity(0.15))
                    Capsule()
                        .fill(level >= 1 ? Color.red : Color.green)
                        .frame(width: geo.size.width * CGFloat(min(1, displayLevel)))
                }
            }
            .frame(height: 6)
        }
    }

    // Roughly -60 dB..0 dB mapped to 0..1.
    private var displayLevel: Float {
        guard level > 0 else { return 0 }
        return max(0, (20 * log10(level) + 60) / 60)
    }
}

struct LegendRow: View {
    let gesture: String
    let action: String
    var body: some View {
        HStack {
            Text(gesture).frame(width: 90, alignment: .leading)
            Text(action).foregroundStyle(.secondary)
        }
        .font(.callout)
    }
}

struct EventLogView: View {
    @EnvironmentObject var model: LooperModel

    private static let timeFormat: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "HH:mm:ss.SSS"
        return f
    }()

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                Text("Event log").font(.headline)
                Spacer()
                Button("Clear") { model.log.removeAll() }
            }
            ScrollViewReader { proxy in
                List(model.log) { entry in
                    HStack(alignment: .top, spacing: 8) {
                        Text(Self.timeFormat.string(from: entry.time)).foregroundStyle(.secondary)
                        Text(entry.source).foregroundStyle(.secondary).frame(width: 74, alignment: .leading)
                        Text(entry.text).textSelection(.enabled)
                    }
                    .font(.system(size: 11, design: .monospaced))
                    .id(entry.id)
                }
                .onChange(of: model.log.count) {
                    if let last = model.log.last { proxy.scrollTo(last.id, anchor: .bottom) }
                }
            }
        }
    }
}
