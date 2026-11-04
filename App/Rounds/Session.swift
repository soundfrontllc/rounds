import AppKit
import AVFoundation
import Foundation

/// A saved session is a folder: `session.json` plus one mono 32-bit float WAV
/// per recorded audio loop (`loop-1.wav` …). MIDI loops keep their notes in
/// the manifest. Each loop is saved with its overdubs merged (undo history
/// isn't kept), rotated so every loop starts at the base loop's top: the
/// loops line up when the files are laid side by side.
///
/// Loops are numbered by the engine's loop ID: part p's own loops are
/// p * 8 + slot, so a session from before parts (one part) reads the same.
struct SessionManifest: Codable {
    struct Loop: Codable {
        var slot: Int          // loop ID
        var gain: Float
        var shared: Bool?
        var file: String?      // audio loops
        var samples: Int64?    // loop length at `sampleRate`
        var instrument: String?   // MIDI loops: instrument name
        var notes: [[Int64]]?     // MIDI loops: [start, length, note, velocity], samples at `sampleRate`
    }

    var format = 1
    var sampleRate: Double
    var baseLoop: Int
    var bars: Int?             // bars in the base loop (tempo); nil = guessed again on load
    var selectedLoop: Int
    var loops: [Loop]
    var parts: [Part]?         // nil: one part, its slots playing loops 1-8
    var part: Int?             // the part that was playing
    var song: Song?            // the whole song playing along, if any

    /// Saved as the split stems (44.1 kHz mono, lossless ALAC) and their grid,
    /// fitted to the session again on load.
    struct Song: Codable {
        var name: String
        var files: [String]    // one per stem, in StemSeparator.names order
        var bpm: Double
        var downbeat: Double   // seconds
        var gains: [Float]
        var muted: [Bool]
        var lanes: [Int]?      // the lane each stem plays in (-1 none); nil: stem i in lane i
    }

    struct Part: Codable {
        var index: Int
        var name: String
        var loops: [Int]       // loop ID in each slot
    }

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
    static func write(_ manifest: SessionManifest, audio: [Int: [Float]], song: [[Float]] = [], to folder: URL) throws {
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
        for (file, samples) in zip(manifest.song?.files ?? [], song) {
            try writeLossless(samples, sampleRate: StemSeparator.sampleRate, to: staged.appendingPathComponent(file))
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

    /// Apple Lossless, 24-bit, in a CAF: a song's stems at about half the size of WAV.
    static func writeLossless(_ samples: [Float], sampleRate: Double, to url: URL) throws {
        let settings: [String: Any] = [
            AVFormatIDKey: kAudioFormatAppleLossless,
            AVSampleRateKey: sampleRate,
            AVNumberOfChannelsKey: 1,
            AVEncoderBitDepthHintKey: 24,
        ]
        guard let buffer = buffer(samples, sampleRate: sampleRate) else { throw SessionError.message("Out of memory") }
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

    /// Empties every loop and part. The next save asks for a new name.
    func newSession() {
        guard let engine, !sessionBusy else { return }
        rounds_engine_reset(engine)
        unloadSong()
        partNames = Self.defaultPartNames
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
        var status = RoundsStatus()
        rounds_engine_get_status(engine, &status)
        var manifest = SessionManifest(sampleRate: sampleRate, baseLoop: Int(status.baseLoopID), bars: bars > 0 ? bars : nil,
                                       selectedLoop: selectedLoop, loops: [], parts: [], part: currentPart)
        // Every loop some part plays, each once (a shared loop is in several).
        var ids: [Int] = []
        for part in parts where part.exists {
            let slots = (0..<Int(ROUNDS_MAX_LOOPS)).map { Int(rounds_engine_part_loop(engine, Int32(part.id), Int32($0))) }
            manifest.parts?.append(SessionManifest.Part(index: part.id, name: partNames[part.id], loops: slots))
            ids += slots.filter { !ids.contains($0) }
        }
        var audio: [Int: [Float]] = [:]
        var midiSaved: Set<Int> = []
        let noteCapacity = rounds_engine_max_notes(engine)
        for id in ids {
            var loop = RoundsLoopStatus()
            rounds_engine_get_pool_loop_status(engine, Int32(id), &loop)
            let name = loopName(id)
            var entry = SessionManifest.Loop(slot: id, gain: loop.gain, shared: loop.shared != 0 ? true : nil)
            defer { manifest.loops.append(entry) }
            switch LoopState(rawValue: loop.state) ?? .empty {
            case .empty: continue
            case .recording:
                append("Session", "\(name) is still recording, so it wasn't saved")
                continue
            default: break
            }
            guard rounds_engine_begin_export(engine, Int32(id)) == 1,
                  await finishTransfer(engine) == 2 else {
                guard self.engine == engine else { return sessionFailed("Audio stopped while saving; nothing was saved.") }
                append("Session", "\(name) couldn't be saved (re-recorded or cleared while saving)")
                continue
            }
            if loop.kind == Int32(ROUNDS_KIND_MIDI.rawValue) {
                var notes = [RoundsNote](repeating: RoundsNote(), count: Int(noteCapacity))
                var length: Int64 = 0
                var instrument: Int32 = 0
                let count = notes.withUnsafeMutableBufferPointer {
                    rounds_engine_take_export_notes(engine, $0.baseAddress, noteCapacity, &length, &instrument)
                }
                guard count >= 0, length > 0 else { continue }
                entry.samples = length
                entry.instrument = (Instrument(rawValue: instrument) ?? .subBass).name
                entry.notes = notes.prefix(Int(count)).map { [$0.start, $0.length, Int64($0.note), Int64($0.velocity)] }
                midiSaved.insert(id)
                continue
            }
            var samples = [Float](repeating: 0, count: Int(capacity))
            let length = samples.withUnsafeMutableBufferPointer { rounds_engine_take_export(engine, $0.baseAddress, capacity) }
            guard length > 0 else { continue }
            samples.removeSubrange(Int(length)...)
            audio[id] = samples
            entry.file = "loop-\(id + 1).wav"
            entry.samples = length
        }
        let saved = Set(audio.keys).union(midiSaved)
        if manifest.baseLoop >= 0 && !saved.contains(manifest.baseLoop) {
            // The base wasn't saved: the first saved loop sets the tempo on load.
            manifest.baseLoop = saved.min() ?? -1
            manifest.bars = nil
        }

        var songStems: [[Float]] = []
        if let song, let source = songSource {
            manifest.song = SessionManifest.Song(name: song.name, files: song.stems.map { "song-\($0.lowercased()).caf" },
                                                 bpm: source.grid.bpm, downbeat: source.grid.downbeat,
                                                 gains: song.gains, muted: song.muted, lanes: songLanes)
            songStems = source.stems
        }
        let toWrite = manifest
        let snapshot = audio
        let songSnapshot = songStems
        do {
            try await Task.detached { try SessionFiles.write(toWrite, audio: snapshot, song: songSnapshot, to: url) }.value
        } catch {
            return sessionFailed("Couldn't save \(name): \(error.localizedDescription)")
        }
        sessionURL = url
        sessionMessage = nil
        append("Session", "Saved \(name): \(saved.count) loop\(saved.count == 1 ? "" : "s") to \(url.path)")
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
        let midi: [Int: MIDILoop]
        do {
            manifest = try SessionFiles.readManifest(in: url)
            audio = try await Task.detached { try Self.loadAudio(manifest, folder: url, sampleRate: rate) }.value
            midi = Self.loadNotes(manifest, sampleRate: rate)
        } catch {
            return sessionFailed("Couldn't open \(name): \(error.localizedDescription)")
        }
        guard self.engine == engine else { return sessionFailed("Audio stopped while opening \(name).") }
        if manifest.sampleRate != rate {
            append("Session", String(format: "Saved at %.0f Hz, converting to %.0f Hz", manifest.sampleRate, rate))
        }

        // Parts first, then the loops: the base loop first so it becomes the base again.
        rounds_engine_reset(engine)
        partNames = Self.defaultPartNames
        for part in manifest.parts ?? [] where part.index >= 0 && part.index < Int(ROUNDS_MAX_PARTS) {
            for (slot, id) in part.loops.enumerated() where slot < Int(ROUNDS_MAX_LOOPS) && Self.validLoopID(id) {
                rounds_engine_set_part_loop(engine, Int32(part.index), Int32(slot), Int32(id))
            }
            partNames[part.index] = part.name
        }
        // The song first: it sets the tempo the loops were made at.
        unloadSong()
        if let entry = manifest.song {
            do {
                let stems = try await Task.detached {
                    try entry.files.map { file -> [Float] in
                        let (samples, rate) = try SessionFiles.readAudio(url.appendingPathComponent(file))
                        return try SessionFiles.resample(samples, from: rate, to: StemSeparator.sampleRate)
                    }
                }.value
                let grid = SongGrid(bpm: entry.bpm, downbeat: entry.downbeat)
                // Loaded before any loop, so it sets the tempo again.
                await loadSong(name: entry.name, stems: stems, grid: grid, gains: entry.gains, muted: entry.muted,
                               lanes: entry.lanes ?? Array(stems.indices), fitTo: nil)
            } catch {
                append("Session", "The song couldn't be loaded: \(error.localizedDescription)")
            }
        }
        let slots = Set(audio.keys).union(midi.keys)
        let order = slots.sorted { a, b in a == manifest.baseLoop || (b != manifest.baseLoop && a < b) }
        var loaded = 0
        for slot in order {
            let name = loopName(slot)
            if let loop = midi[slot] {
                let started = loop.notes.withUnsafeBufferPointer {
                    rounds_engine_begin_import_notes(engine, Int32(slot), $0.baseAddress, Int32(loop.notes.count),
                                                     loop.length, loop.instrument.rawValue)
                }
                guard started == 1, await finishTransfer(engine) == 2 else {
                    guard self.engine == engine else { return sessionFailed("Audio stopped while opening \(name).") }
                    append("Session", "\(name) couldn't be loaded")
                    continue
                }
                loaded += 1
                continue
            }
            guard let samples = audio[slot] else { continue }
            guard Int64(samples.count) <= capacity else {
                append("Session", "\(name) is longer than Rounds' \(Int(maxLoopSeconds)) s limit and wasn't loaded")
                continue
            }
            let started = samples.withUnsafeBufferPointer {
                rounds_engine_begin_import(engine, Int32(slot), $0.baseAddress, Int64(samples.count))
            }
            guard started == 1, await finishTransfer(engine) == 2 else {
                guard self.engine == engine else { return sessionFailed("Audio stopped while opening \(name).") }
                append("Session", "\(name) couldn't be loaded")
                continue
            }
            loaded += 1
        }
        for loop in manifest.loops where Self.validLoopID(loop.slot) {
            rounds_engine_set_loop_gain(engine, Int32(loop.slot), loop.gain)
            if loop.shared == true { rounds_engine_set_shared(engine, Int32(loop.slot), 1) }
        }
        knobsLetGo()
        if let part = manifest.part, part > 0, part < Int(ROUNDS_MAX_PARTS) { rounds_engine_switch_part(engine, Int32(part)) }
        if let savedBars = manifest.bars { setBars(savedBars) }
        if manifest.selectedLoop >= 0 && manifest.selectedLoop < Int(ROUNDS_MAX_LOOPS) { selectedLoop = manifest.selectedLoop }
        sessionURL = url
        sessionMessage = nil
        let partCount = manifest.parts?.count ?? 1
        append("Session", "Opened \(name): \(loaded) loop\(loaded == 1 ? "" : "s") in \(partCount) part\(partCount == 1 ? "" : "s"), "
               + "stopped. Tap a loop to start.")
    }

    nonisolated static func validLoopID(_ id: Int) -> Bool { id >= 0 && id < Int(ROUNDS_POOL_LOOPS) }

    /// "Loop 3", or "Chorus loop 3" for a loop that only another part plays.
    private func loopName(_ id: Int) -> String {
        let part = id / Int(ROUNDS_MAX_LOOPS)
        let slot = id % Int(ROUNDS_MAX_LOOPS)
        return part == 0 ? "Loop \(slot + 1)" : "\(partNames[part]) loop \(slot + 1)"
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
                  validLoopID(loop.slot) else { continue }
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

    struct MIDILoop {
        var length: Int64
        var instrument: Instrument
        var notes: [RoundsNote]
    }

    /// MIDI loops from the manifest, with times scaled like audio lengths
    /// when the session was saved at another sample rate.
    nonisolated private static func loadNotes(_ manifest: SessionManifest, sampleRate: Double) -> [Int: MIDILoop] {
        let ratio = sampleRate / manifest.sampleRate
        let baseSamples = manifest.loops.first { $0.slot == manifest.baseLoop }?.samples
        let newBase = baseSamples.map { Int64((Double($0) * ratio / 8).rounded()) * 8 }
        var result: [Int: MIDILoop] = [:]
        for loop in manifest.loops {
            guard let notes = loop.notes, let samples = loop.samples, samples > 1,
                  validLoopID(loop.slot) else { continue }
            var length = samples
            if ratio != 1 {
                if let baseSamples, let newBase {
                    length = Int64((Double(newBase) * Double(samples) / Double(baseSamples)).rounded())
                } else {
                    length = Int64((Double(samples) * ratio).rounded())
                }
            }
            let scale = Double(length) / Double(samples)
            let converted: [RoundsNote] = notes.compactMap { n in
                guard n.count == 4 else { return nil }
                return RoundsNote(start: Int64((Double(n[0]) * scale).rounded()) % length,
                                  length: max(1, Int64((Double(n[1]) * scale).rounded())),
                                  note: Int32(clamping: n[2]), velocity: Int32(clamping: n[3]))
            }
            let instrument = loop.instrument.flatMap(Instrument.init(name:)) ?? .subBass
            result[loop.slot] = MIDILoop(length: length, instrument: instrument, notes: converted)
        }
        return result
    }

    /// The audio thread copies a slice per block; wait for it here.
    func finishTransfer(_ engine: OpaquePointer) async -> Int32 {
        for _ in 0..<2000 {
            guard self.engine == engine else { return -1 }
            let status = rounds_engine_transfer_status(engine)
            if status != 1 { return status }
            try? await Task.sleep(for: .milliseconds(5))
        }
        return -1
    }

    func sessionFailed(_ text: String) {
        sessionMessage = text
        append("Session", text)
    }
}
