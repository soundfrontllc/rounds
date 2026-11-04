import Accelerate
import AppKit
import AVFoundation
import Foundation
import os

/// Import song → stems: split a song once, then cut bar-aligned sections of
/// its stems into new parts. Holds the split song (mono stems) until another
/// song is opened, so several sections can be taken from one split.
@MainActor
final class StemImport: ObservableObject {
    enum Phase: Equatable {
        case idle
        case downloading(Double)
        case reading
        case splitting(done: Int, total: Int)
        case analysing
        case ready
        case failed(String)
    }

    @Published private(set) var phase = Phase.idle
    @Published private(set) var modelInstalled = StemModel.isInstalled
    @Published private(set) var songName = ""
    @Published private(set) var songSeconds: Double = 0
    @Published var grid = SongGrid(bpm: 120, downbeat: 0) { didSet { selectionChanged() } }
    @Published var startBar = 1 { didSet { selectionChanged() } }
    @Published var bars = 4 { didSet { selectionChanged() } }
    @Published var included = [Bool](repeating: true, count: StemSeparator.names.count) { didSet { selectionChanged() } }
    /// Each stem's level in the selected section (linear RMS).
    @Published private(set) var levels = [Float](repeating: 0, count: StemSeparator.names.count)
    /// Peaks of the whole mix for the overview (one per pixel column).
    @Published private(set) var overview: [Float] = []
    @Published private(set) var previewing = false
    /// How the last split went, e.g. "Split 15:02 of audio in 70 s with Core ML".
    @Published private(set) var splitSummary = ""
    /// For the event log.
    var onLog: ((String) -> Void)?

    private(set) var stems: [[Float]] = []   // mono, 44.1 kHz
    private let cancelled = OSAllocatedUnfairLock(initialState: false)
    private let preview = PreviewPlayer()

    nonisolated static let rate = StemSeparator.sampleRate
    static let lengths = [1, 2, 4, 8]
    static let audible: Float = 0.005  // a stem quieter than this over the song starts unticked

    var totalBars: Int { grid.bars(in: songSeconds) }
    var busy: Bool {
        switch phase {
        case .downloading, .reading, .splitting, .analysing: return true
        default: return false
        }
    }

    // MARK: Model

    func downloadModel() {
        guard !busy else { return }
        phase = .downloading(0)
        Task {
            do {
                try await StemModel.download { fraction in
                    Task { @MainActor in if case .downloading = self.phase { self.phase = .downloading(fraction) } }
                }
                modelInstalled = true
                phase = .idle
            } catch {
                phase = .failed(error.localizedDescription)
            }
        }
    }

    // MARK: Splitting

    func chooseSong() {
        let panel = NSOpenPanel()
        panel.title = "Import song → stems"
        panel.message = "Choose a song (WAV, AIFF, MP3, M4A, FLAC …)"
        panel.allowedContentTypes = [.audio]
        guard panel.runModal() == .OK, let url = panel.url else { return }
        split(url)
    }

    func split(_ url: URL) {
        guard modelInstalled, !busy else { return }
        stopPreview()
        stems = []
        overview = []
        songName = url.deletingPathExtension().lastPathComponent
        phase = .reading
        cancelled.withLock { $0 = false }
        let cancelled = self.cancelled
        Task.detached(priority: .userInitiated) {
            do {
                let song = try StereoAudio.read(url, sampleRate: Self.rate)
                await MainActor.run { self.songSeconds = song.seconds }
                let began = Date()
                let separator = try StemSeparator.make()
                let split = try separator.separate(song) { done, total in
                    Task { @MainActor in self.phase = .splitting(done: done, total: total) }
                    return !cancelled.withLock { $0 }
                }
                let took = Date().timeIntervalSince(began)
                let summary = String(format: "Split %d:%02d of audio in %.0f s with %@ (%.2f× real time)",
                                     Int(song.seconds) / 60, Int(song.seconds) % 60, took, separator.engineName,
                                     took / max(song.seconds, 0.001))
                await MainActor.run {
                    self.splitSummary = summary
                    self.onLog?(summary)
                    self.phase = .analysing
                }
                let mono = split.map(\.mono)
                let peaks = Self.peaks(song.mono, columns: 1200)
                // Tempo from the drums if they are there, else from the whole mix.
                let drums = mono[0]
                let source = vDSP.rootMeanSquare(drums) > 0.01 ? drums : song.mono
                let grid = TempoDetector.detect(source, sampleRate: Self.rate) ?? SongGrid(bpm: 120, downbeat: 0)
                await MainActor.run { self.finishSplit(mono, peaks: peaks, grid: grid) }
            } catch is CancellationError {
                await MainActor.run { self.phase = .idle }
            } catch {
                await MainActor.run { self.phase = .failed(error.localizedDescription) }
            }
        }
    }

    func cancel() { cancelled.withLock { $0 = true } }

    private func finishSplit(_ mono: [[Float]], peaks: [Float], grid: SongGrid) {
        stems = mono
        overview = peaks
        self.grid = grid
        startBar = 1
        bars = 4
        phase = .ready
        included = mono.map { vDSP.rootMeanSquare($0) >= Self.audible }
    }

    nonisolated private static func peaks(_ x: [Float], columns: Int) -> [Float] {
        guard !x.isEmpty else { return [] }
        let per = max(1, x.count / columns)
        return stride(from: 0, to: x.count, by: per).map { start in
            x.withUnsafeBufferPointer { vDSP.maximumMagnitude(UnsafeBufferPointer(rebasing: $0[start..<min(x.count, start + per)])) }
        }
    }

    // MARK: Section

    /// The selected section in 44.1 kHz samples (fractional start).
    var sectionStart: Double { grid.barStart(startBar) * Self.rate }
    var sectionLength: Double { Double(bars) * grid.barSeconds * Self.rate }

    private func selectionChanged() {
        guard !stems.isEmpty else { return }
        let clamped = min(max(1, startBar), max(1, totalBars - bars + 1))
        if clamped != startBar {
            startBar = clamped  // comes back here
            return
        }
        let start = Int(sectionStart)
        let end = min(start + Int(sectionLength), stems.first?.count ?? 0)
        levels = stems.map { stem in
            guard end > start else { return 0 }
            return stem.withUnsafeBufferPointer { vDSP.rootMeanSquare(UnsafeBufferPointer(rebasing: $0[start..<end])) }
        }
        if previewing { startPreview(deviceID: preview.deviceID) }
    }

    // MARK: Preview

    func togglePreview(deviceID: UInt32) {
        if previewing { stopPreview() } else { startPreview(deviceID: deviceID) }
    }

    private func startPreview(deviceID: UInt32) {
        let start = Int(sectionStart)
        let length = Int(sectionLength)
        guard !stems.isEmpty, start >= 0, start + length <= stems[0].count else { return }
        var mix = [Float](repeating: 0, count: length)
        for (i, stem) in stems.enumerated() where included[i] {
            stem.withUnsafeBufferPointer { mix = vDSP.add(mix, UnsafeBufferPointer(rebasing: $0[start..<(start + length)])) }
        }
        previewing = preview.play(mix, sampleRate: Self.rate, deviceID: deviceID)
    }

    func stopPreview() {
        preview.stop()
        previewing = false
    }
}

/// Plays the section on a loop through the interface Rounds uses (beside
/// Rounds' own audio, which keeps running).
final class PreviewPlayer {
    private var engine: AVAudioEngine?
    private(set) var deviceID: UInt32 = 0

    func play(_ samples: [Float], sampleRate: Double, deviceID: UInt32) -> Bool {
        stop()
        self.deviceID = deviceID
        guard let format = AVAudioFormat(standardFormatWithSampleRate: sampleRate, channels: 1),
              let buffer = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: AVAudioFrameCount(samples.count))
        else { return false }
        buffer.frameLength = AVAudioFrameCount(samples.count)
        samples.withUnsafeBufferPointer { buffer.floatChannelData![0].update(from: $0.baseAddress!, count: samples.count) }
        let engine = AVAudioEngine()
        if deviceID != 0 { try? engine.outputNode.auAudioUnit.setDeviceID(deviceID) }
        let player = AVAudioPlayerNode()
        engine.attach(player)
        engine.connect(player, to: engine.mainMixerNode, format: format)
        do {
            try engine.start()
        } catch {
            return false
        }
        player.scheduleBuffer(buffer, at: nil, options: .loops)
        player.play()
        self.engine = engine
        return true
    }

    func stop() {
        engine?.stop()
        engine = nil
    }
}
