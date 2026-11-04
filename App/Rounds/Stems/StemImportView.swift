import SwiftUI

/// The "Import song → stems" sheet: get the model, split a song, pick bars,
/// add them as a part.
struct StemImportView: View {
    @EnvironmentObject var model: LooperModel
    @ObservedObject var stems: StemImport
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack {
                Text("Import song → stems").font(.title2.weight(.semibold))
                Spacer()
                if stems.phase == .ready {
                    Button("Choose another song…") { stems.chooseSong() }
                }
            }
            content
            Divider()
            HStack {
                if let message = model.sessionMessage { Text(message).foregroundStyle(.red).font(.callout) }
                Spacer()
                if stems.phase == .ready {
                    Button(stems.previewing ? "Stop preview" : "Preview") { stems.togglePreview(deviceID: model.deviceID) }
                        .help("Loop the section through \(model.selectedDevice?.name ?? "the interface")")
                    Button("Load whole song") { Task { await model.loadWholeSong() } }
                        .disabled(!model.audioRunning || model.sessionBusy)
                        .help("Put each ticked stem in its lane (drums 1, bass 2, other 3, vocals 4, guitar 5, piano 6) "
                              + "in every part, playing the whole song along with the loops")
                    Button("Add bars as part") { Task { await model.addStemsAsPart() } }
                        .keyboardShortcut(.defaultAction)
                        .disabled(!model.audioRunning || model.sessionBusy || !stems.included.contains(true))
                }
                Button("Done") {
                    stems.stopPreview()
                    dismiss()
                }
            }
        }
        .padding(20)
        .frame(width: 860)
        .frame(minHeight: 300)
    }

    @ViewBuilder private var content: some View {
        if !stems.modelInstalled {
            VStack(alignment: .leading, spacing: 10) {
                Text("Rounds splits songs with HT-Demucs (Meta's 6-stem model: drums, bass, guitar, piano, vocals, other), "
                     + "running on this Mac. It needs a one-time download of 136 MB from Hugging Face.")
                    .fixedSize(horizontal: false, vertical: true)
                if case let .downloading(fraction) = stems.phase {
                    ProgressView(value: fraction) { Text("Downloading the model…") }
                } else {
                    Button("Download the model (136 MB)") { stems.downloadModel() }
                }
                failure
            }
        } else {
            switch stems.phase {
            case .idle, .failed, .downloading:
                VStack(alignment: .leading, spacing: 10) {
                    Text("Choose a song. Rounds splits it into stems (about a fifth of its length on this Mac), "
                         + "finds its tempo, and lets you pick bars to loop.")
                    Button("Choose song…") { stems.chooseSong() }
                    failure
                }
            case .reading:
                ProgressView { Text("Reading \(stems.songName)…") }
            case let .splitting(done, total):
                HStack {
                    ProgressView(value: Double(done), total: Double(total)) { Text("Splitting \(stems.songName): \(done) of \(total)") }
                    Button("Cancel") { stems.cancel() }
                }
            case .analysing:
                ProgressView { Text("Finding the tempo…") }
            case .ready:
                ready
            }
        }
    }

    @ViewBuilder private var failure: some View {
        if case let .failed(text) = stems.phase { Text(text).foregroundStyle(.red).font(.callout) }
    }

    private var ready: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(spacing: 10) {
                Text(stems.songName).font(.headline)
                Text(String(format: "%d:%02d", Int(stems.songSeconds) / 60, Int(stems.songSeconds) % 60)).foregroundStyle(.secondary)
                Spacer()
                Text(String(format: "%.2f BPM", stems.grid.bpm)).font(.system(.body, design: .monospaced).weight(.semibold))
                Button("−") { stems.grid.bpm -= 0.05 }.help("Slower by 0.05 BPM")
                Button("+") { stems.grid.bpm += 0.05 }.help("Faster by 0.05 BPM")
                Button("½×") { stems.grid.bpm /= 2 }
                Button("2×") { stems.grid.bpm *= 2 }
            }
            HStack(spacing: 10) {
                Text("Bar 1 starts at \(String(format: "%.3f", stems.grid.downbeat)) s").foregroundStyle(.secondary)
                Button("◀ beat") { stems.grid = stems.grid.shifted(beats: -1) }
                Button("beat ▶") { stems.grid = stems.grid.shifted(beats: 1) }
                Button("−10 ms") { nudge(-0.01) }
                Button("+10 ms") { nudge(0.01) }
                Spacer()
                Text(fitText).foregroundStyle(.secondary).font(.callout)
            }
            SongOverview(stems: stems)
                .frame(height: 90)
            HStack(spacing: 14) {
                Stepper("From bar \(stems.startBar)", value: $stems.startBar, in: 1...max(1, stems.totalBars))
                Picker("Length", selection: $stems.bars) {
                    ForEach(StemImport.lengths, id: \.self) { Text(LooperModel.barsText($0)).tag($0) }
                }
                .pickerStyle(.segmented)
                .frame(width: 300)
                Spacer()
                Text("Click the waveform to pick a bar").foregroundStyle(.secondary).font(.callout)
            }
            HStack(alignment: .top, spacing: 8) {
                ForEach(StemSeparator.names.indices, id: \.self) { i in
                    VStack(alignment: .leading, spacing: 6) {
                        Toggle(StemSeparator.names[i], isOn: $stems.included[i])
                        LevelMeter(label: "", level: stems.levels[i] * 4)
                    }
                    .padding(8)
                    .background(RoundedRectangle(cornerRadius: 6).fill(Color(nsColor: .controlBackgroundColor)))
                }
            }
            Text(stems.splitSummary).foregroundStyle(.secondary).font(.callout)
            Text("Load whole song: each ticked stem plays in its own lane (drums 1, bass 2, other 3, vocals 4, guitar 5, piano 6), "
                 + "in every part. Add bars as part: ticked stems become loops of a new part. "
                 + "Stems that are nearly silent in the song start unticked.")
                .fixedSize(horizontal: false, vertical: true)
                .foregroundStyle(.secondary).font(.callout)
        }
    }

    private func nudge(_ seconds: Double) {
        var grid = stems.grid
        grid.downbeat += seconds
        stems.grid = grid.shifted(beats: 0)
    }

    private var fitText: String {
        guard model.bars > 0 else { return String(format: "Sets the tempo: %.1f BPM", stems.grid.bpm) }
        let change = (model.bpm / stems.grid.bpm - 1) * 100
        if abs(change) < 0.2 { return String(format: "Matches the session (%.1f BPM)", model.bpm) }
        return String(format: "Stretched to %.1f BPM (%+.1f%%)", model.bpm, change)
    }
}

/// The whole song's waveform with its bar lines and the chosen section.
struct SongOverview: View {
    @ObservedObject var stems: StemImport

    var body: some View {
        GeometryReader { geo in
            Canvas { context, size in
                let seconds = max(stems.songSeconds, 0.001)
                let x = { (t: Double) in CGFloat(t / seconds) * size.width }
                // Section.
                let from = stems.grid.barStart(stems.startBar)
                let to = from + Double(stems.bars) * stems.grid.barSeconds
                context.fill(Path(CGRect(x: x(from), y: 0, width: x(to) - x(from), height: size.height)),
                             with: .color(.accentColor.opacity(0.2)))
                // Bar lines (every bar if there's room, else every 4).
                let every = stems.grid.barSeconds / seconds * size.width < 6 ? 4 : 1
                var bar = 1
                while stems.grid.barStart(bar) < seconds {
                    let lineX = x(stems.grid.barStart(bar))
                    let strong = (bar - 1) % 4 == 0
                    if (bar - 1) % every == 0 {
                        context.fill(Path(CGRect(x: lineX, y: 0, width: 1, height: size.height)),
                                     with: .color(.secondary.opacity(strong ? 0.45 : 0.18)))
                    }
                    bar += 1
                }
                // Waveform.
                let peaks = stems.overview
                guard !peaks.isEmpty else { return }
                var path = Path()
                for (i, p) in peaks.enumerated() {
                    let px = CGFloat(i) / CGFloat(peaks.count) * size.width
                    let h = CGFloat(min(1, p)) * size.height / 2
                    path.addRect(CGRect(x: px, y: size.height / 2 - h, width: max(1, size.width / CGFloat(peaks.count)), height: 2 * h))
                }
                context.fill(path, with: .color(.primary.opacity(0.55)))
            }
            .contentShape(Rectangle())
            .onTapGesture { location in
                let t = Double(location.x / geo.size.width) * stems.songSeconds
                stems.startBar = max(1, Int(((t - stems.grid.downbeat) / stems.grid.barSeconds).rounded(.down)) + 1)
            }
        }
        .background(RoundedRectangle(cornerRadius: 6).fill(Color(nsColor: .textBackgroundColor)))
    }
}
