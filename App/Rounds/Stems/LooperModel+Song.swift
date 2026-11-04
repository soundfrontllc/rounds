import Accelerate
import Foundation

/// The whole song playing along with the loops, its stems in lanes 1-6.
extension LooperModel {
    /// Loads the split song in the import window as the song: each ticked stem
    /// goes in its own lane (drums 1, bass 2, other 3, vocals 4, guitar 5,
    /// piano 6) in every part. With loops already setting a tempo it's
    /// stretched to it; otherwise it sets it.
    func loadWholeSong() async {
        let stems = stemImport
        guard engine != nil, !sessionBusy, stems.phase == .ready, !stems.stems.isEmpty else { return }
        let lanes = stems.included.enumerated().map { $0.element ? $0.offset : -1 }
        let taken = lanes.filter { $0 >= 0 && laneHasLoops($0) }
        guard taken.isEmpty else {
            let names = taken.map { "\($0 + 1)" }.joined(separator: ", ")
            sessionFailed("Lane\(taken.count == 1 ? "" : "s") \(names) hold\(taken.count == 1 ? "s" : "") loops. "
                          + "Clear \(taken.count == 1 ? "it" : "them") (in every part), or untick "
                          + taken.map { StemSeparator.names[$0] }.joined(separator: ", ") + ".")
            return
        }
        sessionBusy = true
        defer { sessionBusy = false }
        await loadSong(name: stems.songName, stems: stems.stems, grid: stems.grid,
                       gains: [Float](repeating: 1, count: stems.stems.count),
                       muted: [Bool](repeating: false, count: stems.stems.count), lanes: lanes,
                       fitTo: loopsSetTempo ? barLength : nil)
    }

    /// A loop in this slot of any part.
    func laneHasLoops(_ slot: Int) -> Bool {
        guard let engine else { return false }
        return parts.filter(\.exists).contains { part in
            var status = RoundsLoopStatus()
            rounds_engine_get_pool_loop_status(engine, rounds_engine_part_loop(engine, Int32(part.id), Int32(slot)), &status)
            return status.state != ROUNDS_LOOP_EMPTY.rawValue
        }
    }

    /// Loops anywhere (not just a tempo from a song) mean the song must fit them.
    private var loopsSetTempo: Bool { parts.contains { $0.exists && $0.loops > 0 } && barLength > 0 }

    /// `lanes`: the lane each stem goes in (-1 for none). `fitTo`: the
    /// session's bar length to stretch the song to, or nil to let the song
    /// set the tempo.
    @discardableResult
    func loadSong(name: String, stems: [[Float]], grid: SongGrid, gains: [Float], muted: [Bool], lanes: [Int],
                  fitTo sessionBar: Int64?) async -> Bool {
        guard let engine else { return false }
        let rate = sampleRate
        if let sessionBar {
            append("Song", String(format: "Fitting %@ (%.2f BPM) to the session's %.1f BPM", name, grid.bpm,
                                  240 * rate / Double(sessionBar)))
        } else {
            append("Song", String(format: "Loading %@ at its own tempo, %.2f BPM", name, grid.bpm))
        }
        let began = Date()
        let prepared: PreparedSong
        let overviews: [[Float]]
        do {
            (prepared, overviews) = try await Task.detached(priority: .userInitiated) {
                let prepared = try WholeSong.prepare(stems, grid: grid, rate: rate, sessionBar: sessionBar) { _, _ in }
                return (prepared, prepared.stems.map { Self.overview($0, columns: 600) })
            }.value
        } catch {
            sessionFailed("Couldn't prepare the song: \(error.localizedDescription)")
            return false
        }
        guard self.engine == engine else { return false }
        let length = Int64(prepared.stems.map(\.count).min() ?? 0)
        let loaded = await Task.detached(priority: .userInitiated) {
            Self.withPointers(prepared.stems) { pointers in
                rounds_engine_load_song(engine, pointers, Int32(pointers.count), length, prepared.barLength, prepared.downbeat) == 1
            }
        }.value
        guard loaded else {
            sessionFailed("The song couldn't be loaded")
            return false
        }
        // The engine takes it at its next block.
        for _ in 0..<200 {
            var s = RoundsStatus()
            rounds_engine_get_status(engine, &s)
            if s.songLoaded != 0 && s.songLength == length { break }
            try? await Task.sleep(for: .milliseconds(5))
        }
        songSource = (stems, grid)
        songBarLength = prepared.barLength
        songDownbeat = prepared.downbeat
        songOverviews = overviews
        let names = Array(StemSeparator.names.prefix(stems.count))
        song = SongInfo(name: name, stems: names, bars: Int((length - prepared.downbeat) / prepared.barLength),
                        gains: gains, muted: muted, pans: [Float](repeating: 0, count: names.count))
        panKnobInControl = [Bool](repeating: false, count: panKnobInControl.count)
        for i in names.indices {
            applySongGain(i)
            rounds_engine_set_song_pan(engine, Int32(i), 0)
        }
        for (stem, lane) in lanes.enumerated() where stem < names.count && lane >= 0 && lane < loops.count {
            rounds_engine_set_song_lane(engine, Int32(lane), Int32(stem))
            knobsLetGo()
        }
        stemKnobInControl = [Bool](repeating: false, count: stemKnobInControl.count)
        let placed = lanes.enumerated().filter { $0.offset < names.count && $0.element >= 0 }
            .map { "\(names[$0.offset]) \($0.element + 1)" }.joined(separator: ", ")
        append("Song", String(format: "%@ loaded: %d bars at %.2f BPM (%.0f s to prepare). Lanes: %@. Play starts it from bar 1.",
                              name, song?.bars ?? 0, 240 * rate / Double(prepared.barLength), Date().timeIntervalSince(began),
                              placed.isEmpty ? "none" : placed))
        return true
    }

    func unloadSong() {
        guard let engine, song != nil else { return }
        rounds_engine_unload_song(engine)
        song = nil
        songSource = nil
        songOverviews = []
        knobsLetGo()
        append("Song", "Unloaded")
    }

    /// The lane each stem is in (-1 for none), as the engine last reported.
    var songLanes: [Int] {
        guard let song else { return [] }
        return song.stems.indices.map { stem in loops.first { $0.stem == stem }?.id ?? -1 }
    }

    func setSongGain(_ stem: Int, _ gain: Float) {
        guard song != nil else { return }
        song?.gains[stem] = gain
        stemKnobInControl[stem] = false
        applySongGain(stem)
    }

    func toggleSongMute(_ stem: Int) {
        guard let song else { return }
        self.song?.muted[stem].toggle()
        applySongGain(stem)
        append("Song", "\(song.stems[stem]) \(song.muted[stem] ? "on" : "muted")")
    }

    func setSongPan(_ stem: Int, _ pan: Float) {
        guard let engine, let song, stem < song.pans.count else { return }
        self.song?.pans[stem] = max(-1, min(1, pan))
        rounds_engine_set_song_pan(engine, Int32(stem), self.song?.pans[stem] ?? 0)
    }

    /// Where Play starts the song (or, while it plays, a jump at the next bar line).
    func setSongBar(_ bar: Int) {
        guard let engine, song != nil else { return }
        rounds_engine_set_song_bar(engine, Int32(max(1, bar)))
    }

    private func applySongGain(_ stem: Int) {
        guard let engine, let song else { return }
        rounds_engine_set_song_gain(engine, Int32(stem), song.muted[stem] ? 0 : song.gains[stem])
    }

    /// The knob on a stem's lane: like the loop knobs, it takes over once it
    /// reaches the stem's volume.
    func stemKnobMoved(_ knob: Int, stem: Int, from previous: Int, to value: Int) {
        guard let song, stem < song.stems.count else { return }
        guard let (gain, caughtUp) = Self.takeover(gain: song.gains[stem], inControl: stemKnobInControl[stem],
                                                   from: previous, to: value) else { return }
        if caughtUp && !stemKnobInControl[stem] {
            stemKnobInControl[stem] = true
            append("Launchkey", "Knob \(knob + 1) caught up with the \(song.stems[stem]) stem's volume")
        }
        self.song?.gains[stem] = gain
        applySongGain(stem)
    }

    func pollSong(_ s: RoundsStatus) {
        guard var info = song else { return }
        guard s.songLoaded != 0 else { return }
        info.playing = s.songPlaying != 0
        info.bar = Int(s.songBar)
        info.position = s.songLength > 0 ? Double(s.songPosition) / Double(s.songLength) : 0
        if info != song { song = info }
    }

    /// Peaks of a stem, one per column, for its lane.
    nonisolated static func overview(_ x: [Float], columns: Int) -> [Float] {
        guard !x.isEmpty else { return [] }
        let per = max(1, x.count / columns)
        return stride(from: 0, to: x.count, by: per).map { start in
            x.withUnsafeBufferPointer { vDSP.maximumMagnitude(UnsafeBufferPointer(rebasing: $0[start..<min(x.count, start + per)])) }
        }
    }

    /// Pointers to each array's storage, all valid at once.
    nonisolated static func withPointers<R>(_ arrays: [[Float]], _ body: ([UnsafePointer<Float>?]) -> R) -> R {
        var pointers: [UnsafePointer<Float>?] = []
        func next(_ i: Int) -> R {
            guard i < arrays.count else { return body(pointers) }
            return arrays[i].withUnsafeBufferPointer { buffer in
                pointers.append(buffer.baseAddress)
                return next(i + 1)
            }
        }
        return next(0)
    }
}
