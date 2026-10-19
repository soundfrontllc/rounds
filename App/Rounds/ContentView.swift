import SwiftUI

struct ContentView: View {
    @EnvironmentObject var model: LooperModel
    @FocusState private var focused: Bool

    var body: some View {
        VStack(spacing: 0) {
            header
            Divider()
            HStack(alignment: .top, spacing: 16) {
                VStack(alignment: .leading, spacing: 16) {
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
        .frame(minWidth: 900, minHeight: 560)
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
            if model.audioRunning {
                Button("Stop audio") { model.stopAudio() }
            }
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 10)
    }

    // MARK: Loop

    private var loopPanel: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(alignment: .firstTextBaseline) {
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
            if let error = model.audioError {
                Text(error).foregroundStyle(.red).font(.callout)
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
            LegendRow(gesture: "Tap", action: "Record → Overdub → Play")
            LegendRow(gesture: "Double-tap", action: "Undo last layer")
            LegendRow(gesture: "Hold", action: "Stop")
            LegendRow(gesture: "Hold 2 s", action: "Clear")
        }
        .padding(16)
        .background(RoundedRectangle(cornerRadius: 12).fill(Color(nsColor: .controlBackgroundColor)))
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
