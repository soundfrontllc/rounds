import Foundation

extension LooperModel {
    /// Makes the selected section of the split song a new part: one loop per
    /// ticked stem, in slots 1, 2 … (skipping lanes that hold the whole
    /// song's stems). With no tempo yet the song sets it;
    /// otherwise the section is stretched to the session's bars.
    func addStemsAsPart() async {
        let stems = stemImport
        guard let engine, !sessionBusy, stems.phase == .ready else { return }
        guard let part = parts.first(where: { !$0.exists })?.id else {
            return sessionFailed("All 8 parts are used, so the stems weren't added")
        }
        let chosen = stems.included.indices.filter { stems.included[$0] }
        guard !chosen.isEmpty else { return }
        // Lanes holding a song stem only play the song.
        let lanes = self.loops.filter { $0.stem < 0 }.map(\.id)
        guard chosen.count <= lanes.count else {
            return sessionFailed("Only \(lanes.count) lanes are free of the song's stems; untick \(chosen.count - lanes.count) stem\(chosen.count - lanes.count == 1 ? "" : "s")")
        }
        let bars = stems.bars
        let rate = sampleRate
        let hadTempo = barLength > 0
        // Whole bars of the session; or of the song, kept a multiple of 8
        // samples so the engine can take it as `bars` bars.
        let target = hadTempo
            ? Int(barLength) * bars
            : Int((stems.sectionLength * rate / StemImport.rate / 8).rounded()) * 8
        guard Double(target) / rate <= maxLoopSeconds else {
            return sessionFailed("\(bars) bars is longer than Rounds' \(Int(maxLoopSeconds)) s loop limit")
        }
        sessionBusy = true
        defer { sessionBusy = false }
        let name = "\(stems.songName) · bar \(stems.startBar)"
        if hadTempo {
            append("Stems", String(format: "Fitting %@ (%.1f BPM) to %.1f BPM", name, stems.grid.bpm, bpm))
        }

        // Cut and stretch off the main thread.
        let start = stems.sectionStart
        let length = stems.sectionLength
        let sources = chosen.map { stems.stems[$0] }
        let fade = max(32, Int(rate * 0.005))
        let loops: [[Float]]
        do {
            loops = try await Task.detached(priority: .userInitiated) {
                try sources.map { try StemSection.loop($0, start: start, length: length, target: target, rate: rate, fade: fade) }
            }.value
        } catch {
            return sessionFailed("Couldn't cut the stems: \(error.localizedDescription)")
        }
        guard self.engine == engine else { return }

        emptyPartRequested = part
        rounds_engine_add_empty_part(engine, Int32(part))
        for _ in 0..<200 where !parts[part].exists {
            var status = RoundsPartStatus()
            rounds_engine_get_part_status(engine, Int32(part), &status)
            if status.exists != 0 { break }
            try? await Task.sleep(for: .milliseconds(5))
        }
        partNames[part] = name
        var added = 0
        for (index, samples) in loops.enumerated() {
            let slot = lanes[index]
            let id = rounds_engine_part_loop(engine, Int32(part), Int32(slot))
            let started = samples.withUnsafeBufferPointer {
                rounds_engine_begin_import(engine, id, $0.baseAddress, Int64(samples.count))
            }
            guard started == 1, await finishTransfer(engine) == 2 else {
                guard self.engine == engine else { return sessionFailed("Audio stopped while adding the stems.") }
                append("Stems", "\(StemSeparator.names[chosen[index]]) couldn't be added")
                continue
            }
            append("Stems", "\(partTitle(part)) loop \(slot + 1): \(StemSeparator.names[chosen[index]])")
            added += 1
        }
        // With no tempo before, the song's bars set it.
        if !hadTempo { setBars(bars) }
        sessionMessage = nil
        if running {
            append("Stems", "Added \(partTitle(part)) (\(added) stems, stopped). Switch to it when you're ready.")
        } else {
            rounds_engine_switch_part(engine, Int32(part))
            append("Stems", "Added \(partTitle(part)) (\(added) stems). Press Play to start it.")
        }
    }
}
