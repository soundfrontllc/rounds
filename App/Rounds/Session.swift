import AppKit
import AVFoundation
import Foundation

/// A saved session is a folder: `session.json` plus one mono 32-bit float WAV
/// per recorded loop (`loop-1.wav` …). Each loop is saved with its overdubs
/// merged (undo history isn't kept), rotated so every file starts at the base
/// loop's top: the loops line up when the files are laid side by side.
struct SessionManifest: Codable {
    struct Loop: Codable {
        var slot: Int
        var gain: Float
        var file: String?      // nil for an empty slot
        var samples: Int64?    // loop length at `sampleRate`
    }

    var format = 1
    var sampleRate: Double
    var baseLoop: Int
    var selectedLoop: Int
    var loops: [Loop]

    static let fileName = "session.json"
}

enum SessionError: LocalizedError {
    case message(String)
    var errorDescription: String? {
        switch self { case let .message(text): return text }
    }
}

enum SessionFiles {
    static let defaultFolder = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Music/Rounds")

    static func isSession(_ folder: URL) -> Bool {
        FileManager.default.fileExists(atPath: folder.appendingPathComponent(SessionManifest.fileName).path)
    }

    /// Writes the session into a scratch folder, then swaps it in, so a failed
    /// save never leaves half a session behind.
    static func write(_ manifest: SessionManifest, audio: [Int: [Float]], to folder: URL) throws {
        let fm = FileManager.default
        if fm.fileExists(atPath: folder.path) && !isSession(folder) {
            throw SessionError.message("\(folder.lastPathComponent) already exists and isn't a Rounds session, so it wasn't replaced.")
        }
        try fm.createDirectory(at: folder.deletingLastPathComponent(), withIntermediateDirectories: true)
        let scratch = try fm.url(for: .itemReplacementDirectory, in: .userDomainMask, appropriateFor: folder, create: true)
        let staged = scratch.appendingPathComponent(folder.lastPathComponent)
        try fm.createDirectory(at: staged, withIntermediateDirectories: false)
        defer { try? fm.removeItem(at: scratch) }

        for loop in manifest.loops {
            guard let file = loop.file, let samples = audio[loop.slot] else { continue }
            try writeWAV(samples, sampleRate: manifest.sampleRate, to: staged.appendingPathComponent(file))
        }
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        try encoder.encode(manifest).write(to: staged.appendingPathComponent(SessionManifest.fileName))

        if fm.fileExists(atPath: folder.path) {
            _ = try fm.replaceItemAt(folder, withItemAt: staged)
        } else {
            try fm.moveItem(at: staged, to: folder)
        }
    }

    static func readManifest(in folder: URL) throws -> SessionManifest {
        let url = folder.appendingPathComponent(SessionManifest.fileName)
        guard let data = try? Data(contentsOf: url) else {
            throw SessionError.message("\(folder.lastPathComponent) isn't a Rounds session (no \(SessionManifest.fileName)).")
        }
        let manifest = try JSONDecoder().decode(SessionManifest.self, from: data)
        guard manifest.format == 1 else {
            throw SessionError.message("This session was saved by a newer version of Rounds.")
        }
        return manifest
    }

    static func writeWAV(_ samples: [Float], sampleRate: Double, to url: URL) throws {
        let settings: [String: Any] = [
            AVFormatIDKey: kAudioFormatLinearPCM,
            AVSampleRateKey: sampleRate,
            AVNumberOfChannelsKey: 1,
            AVLinearPCMBitDepthKey: 32,
            AVLinearPCMIsFloatKey: true,
            AVLinearPCMIsNonInterleaved: false,
        ]
        guard let buffer = buffer(samples, sampleRate: sampleRate) else { throw SessionError.message("Out of memory") }
        // AVAudioFile writes its last chunk when it is released; the pool makes
        // that happen before this returns, not whenever the thread drains.
        try autoreleasepool {
            let file = try AVAudioFile(forWriting: url, settings: settings, commonFormat: .pcmFormatFloat32, interleaved: false)
            try file.write(from: buffer)
        }
    }

    /// Reads a WAV/CAF as mono (channels averaged) at the file's own rate.
    static func readAudio(_ url: URL) throws -> (samples: [Float], sampleRate: Double) {
        let file = try AVAudioFile(forReading: url)
        let format = file.processingFormat
        let channels = Int(format.channelCount)
        guard let buffer = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: 65536) else {
            throw SessionError.message("Out of memory")
        }
        // One read() can return less than asked for, so read until the end.
        var mono: [Float] = []
        mono.reserveCapacity(Int(file.length))
        while file.framePosition < file.length {
            try file.read(into: buffer)
            let frames = Int(buffer.frameLength)
            guard frames > 0 else { break }
            guard let data = buffer.floatChannelData else { throw SessionError.message("Unsupported audio in \(url.lastPathComponent)") }
            for i in 0..<frames {
                var sum: Float = 0
                for c in 0..<channels { sum += data[c][i] }
                mono.append(sum / Float(channels))
            }
        }
        return (mono, format.sampleRate)
    }

    /// For a session saved at another sample rate than the interface runs at.
    /// The loop's start is appended to its end before converting, so the
    /// converter's tail covers the seam instead of leaving silence there.
    static func resample(_ samples: [Float], from: Double, to: Double) throws -> [Float] {
        guard from != to else { return samples }
        let wrapped = samples + samples.prefix(4096)
        guard let input = buffer(wrapped, sampleRate: from),
              let outFormat = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: to, channels: 1, interleaved: false),
              let converter = AVAudioConverter(from: input.format, to: outFormat),
              let output = AVAudioPCMBuffer(pcmFormat: outFormat,
                                            frameCapacity: AVAudioFrameCount(Double(wrapped.count) * to / from) + 4096)
        else { throw SessionError.message("Couldn't convert the sample rate") }
        converter.sampleRateConverterQuality = AVAudioQuality.max.rawValue
        var fed = false
        var error: NSError?
        converter.convert(to: output, error: &error) { _, status in
            if fed {
                status.pointee = .endOfStream
                return nil
            }
            fed = true
            status.pointee = .haveData
            return input
        }
        if let error { throw error }
        let length = min(Int(output.frameLength), Int((Double(samples.count) * to / from).rounded()))
        return Array(UnsafeBufferPointer(start: output.floatChannelData![0], count: length))
    }

    private static func buffer(_ samples: [Float], sampleRate: Double) -> AVAudioPCMBuffer? {
        guard let format = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: sampleRate, channels: 1, interleaved: false),
              let buffer = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: AVAudioFrameCount(max(1, samples.count)))
        else { return nil }
        buffer.frameLength = AVAudioFrameCount(samples.count)
        samples.withUnsafeBufferPointer { buffer.floatChannelData![0].update(from: $0.baseAddress!, count: samples.count) }
        return buffer
    }
}

// MARK: - Saving and opening

extension LooperModel {
    /// Saves to the current session folder, or asks where on the first save.
    func saveSession() {
        if let url = sessionURL { Task { await save(to: url) } } else { saveSessionAs() }
    }

    func saveSessionAs() {
        let panel = NSSavePanel()
        panel.title = "Save session"
        panel.prompt = "Save"
        panel.nameFieldLabel = "Session:"
        panel.nameFieldStringValue = sessionURL?.lastPathComponent ?? "Untitled"
        panel.directoryURL = sessionURL?.deletingLastPathComponent() ?? SessionFiles.defaultFolder
        try? FileManager.default.createDirectory(at: SessionFiles.defaultFolder, withIntermediateDirectories: true)
        panel.canCreateDirectories = true
        guard panel.runModal() == .OK, let url = panel.url else { return }
        Task { await save(to: url) }
    }

    func openSession() {
        let panel = NSOpenPanel()
        panel.title = "Open session"
        panel.message = "Choose a session folder"
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.directoryURL = SessionFiles.defaultFolder
        guard panel.runModal() == .OK, let url = panel.url else { return }
        Task { await open(url) }
    }

    /// Empties every loop. The next save asks for a new name.
    func newSession() {
        guard let engine, !sessionBusy else { return }
        for loop in 0..<Int(ROUNDS_MAX_LOOPS) { rounds_engine_send(engine, Int32(loop), ROUNDS_CMD_CLEAR, -1) }
        sessionURL = nil
        append("Session", "New session")
    }

    private func save(to url: URL) async {
        guard let engine, !sessionBusy else { return }
        sessionBusy = true
        defer { sessionBusy = false }
        let name = url.lastPathComponent
        append("Session", "Saving \(name)…")

        let capacity = rounds_engine_max_loop_samples(engine)
        var manifest = SessionManifest(sampleRate: sampleRate, baseLoop: baseLoop, selectedLoop: selectedLoop, loops: [])
        var audio: [Int: [Float]] = [:]
        for loop in loops {
            var entry = SessionManifest.Loop(slot: loop.id, gain: loop.gain)
            defer { manifest.loops.append(entry) }
            switch loop.state {
            case .empty: continue
            case .recording:
                append("Session", "Loop \(loop.id + 1) is still recording, so it wasn't saved")
                continue
            default: break
            }
            guard rounds_engine_begin_export(engine, Int32(loop.id)) == 1,
                  await finishTransfer(engine) == 2 else {
                guard self.engine == engine else { return sessionFailed("Audio stopped while saving; nothing was saved.") }
                append("Session", "Loop \(loop.id + 1) couldn't be saved (re-recorded or cleared while saving)")
                continue
            }
            var samples = [Float](repeating: 0, count: Int(capacity))
            let length = samples.withUnsafeMutableBufferPointer { rounds_engine_take_export(engine, $0.baseAddress, capacity) }
            guard length > 0 else { continue }
            samples.removeSubrange(Int(length)...)
            audio[loop.id] = samples
            entry.file = "loop-\(loop.id + 1).wav"
            entry.samples = length
        }
        if manifest.baseLoop >= 0 && audio[manifest.baseLoop] == nil {
            // The base wasn't saved: the first saved loop sets it on load.
            manifest.baseLoop = audio.keys.min() ?? -1
        }

        let toWrite = manifest
        let snapshot = audio
        do {
            try await Task.detached { try SessionFiles.write(toWrite, audio: snapshot, to: url) }.value
        } catch {
            return sessionFailed("Couldn't save \(name): \(error.localizedDescription)")
        }
        sessionURL = url
        sessionMessage = nil
        append("Session", "Saved \(name): \(audio.count) loop\(audio.count == 1 ? "" : "s") to \(url.path)")
    }

    private func open(_ url: URL) async {
        guard let engine, !sessionBusy else { return }
        sessionBusy = true
        defer { sessionBusy = false }
        let name = url.lastPathComponent
        let rate = sampleRate
        let capacity = rounds_engine_max_loop_samples(engine)
        append("Session", "Opening \(name)…")

        let manifest: SessionManifest
        let audio: [Int: [Float]]
        do {
            manifest = try SessionFiles.readManifest(in: url)
            audio = try await Task.detached { try Self.loadAudio(manifest, folder: url, sampleRate: rate) }.value
        } catch {
            return sessionFailed("Couldn't open \(name): \(error.localizedDescription)")
        }
        guard self.engine == engine else { return sessionFailed("Audio stopped while opening \(name).") }
        if manifest.sampleRate != rate {
            append("Session", String(format: "Saved at %.0f Hz, converting to %.0f Hz", manifest.sampleRate, rate))
        }

        // The base loop loads first so it becomes the base again.
        for loop in 0..<Int(ROUNDS_MAX_LOOPS) { rounds_engine_send(engine, Int32(loop), ROUNDS_CMD_CLEAR, -1) }
        let order = audio.keys.sorted { a, b in a == manifest.baseLoop || (b != manifest.baseLoop && a < b) }
        var loaded = 0
        for slot in order {
            guard let samples = audio[slot] else { continue }
            guard Int64(samples.count) <= capacity else {
                append("Session", "Loop \(slot + 1) is longer than Rounds' \(Int(maxLoopSeconds)) s limit and wasn't loaded")
                continue
            }
            let started = samples.withUnsafeBufferPointer {
                rounds_engine_begin_import(engine, Int32(slot), $0.baseAddress, Int64(samples.count))
            }
            guard started == 1, await finishTransfer(engine) == 2 else {
                guard self.engine == engine else { return sessionFailed("Audio stopped while opening \(name).") }
                append("Session", "Loop \(slot + 1) couldn't be loaded")
                continue
            }
            loaded += 1
        }
        for loop in manifest.loops where loop.slot >= 0 && loop.slot < Int(ROUNDS_MAX_LOOPS) {
            setGain(loop.slot, loop.gain)
        }
        if manifest.selectedLoop >= 0 && manifest.selectedLoop < Int(ROUNDS_MAX_LOOPS) { selectedLoop = manifest.selectedLoop }
        sessionURL = url
        sessionMessage = nil
        append("Session", "Opened \(name): \(loaded) loop\(loaded == 1 ? "" : "s"), stopped. Tap a loop to start.")
    }

    /// Reads every loop's WAV, converted to `sampleRate` and set to its exact
    /// length. When converting, lengths are scaled from the base so loops stay
    /// exact multiples of it.
    nonisolated private static func loadAudio(_ manifest: SessionManifest, folder: URL, sampleRate: Double) throws -> [Int: [Float]] {
        let ratio = sampleRate / manifest.sampleRate
        let baseSamples = manifest.loops.first { $0.slot == manifest.baseLoop }?.samples
        let newBase = baseSamples.map { Int64((Double($0) * ratio / 8).rounded()) * 8 }
        var audio: [Int: [Float]] = [:]
        for loop in manifest.loops {
            guard let file = loop.file, let samples = loop.samples, samples > 0,
                  loop.slot >= 0 && loop.slot < Int(ROUNDS_MAX_LOOPS) else { continue }
            var length = samples
            if ratio != 1 {
                if let baseSamples, let newBase {
                    length = Int64((Double(newBase) * Double(samples) / Double(baseSamples)).rounded())
                } else {
                    length = Int64((Double(samples) * ratio).rounded())
                }
            }
            let (raw, fileRate) = try SessionFiles.readAudio(folder.appendingPathComponent(file))
            var converted = try SessionFiles.resample(raw, from: fileRate, to: sampleRate)
            if converted.count < Int(length) {
                converted += [Float](repeating: 0, count: Int(length) - converted.count)
            } else {
                converted.removeSubrange(Int(length)...)
            }
            audio[loop.slot] = converted
        }
        return audio
    }

    /// The audio thread copies a slice per block; wait for it here.
    private func finishTransfer(_ engine: OpaquePointer) async -> Int32 {
        for _ in 0..<2000 {
            guard self.engine == engine else { return -1 }
            let status = rounds_engine_transfer_status(engine)
            if status != 1 { return status }
            try? await Task.sleep(for: .milliseconds(5))
        }
        return -1
    }

    private func sessionFailed(_ text: String) {
        sessionMessage = text
        append("Session", text)
    }
}
