import Foundation

/// A whole split song, ready for the engine: stems at the engine's rate with
/// bars of exactly `barLength` samples.
struct PreparedSong {
    var stems: [[Float]]
    var barLength: Int64
    var downbeat: Int64   // sample of bar 1
}

/// The loaded song as the UI sees it.
struct SongInfo: Equatable {
    var name: String
    var stems: [String]
    var bars = 0
    var playing = false
    var bar = 1            // heard now, or where Play starts it
    var position: Double = 0   // 0...1
    var gains: [Float]
    var muted: [Bool]
    var pans: [Float] = []
}

enum WholeSong {
    /// The split stems (44.1 kHz mono) at `rate`, fitted so a bar is a whole
    /// number of samples: the session's bar when it has a tempo (stretched to
    /// it), else the song's own bar rounded to a sample (a change far too small
    /// to hear).
    static func prepare(_ stems: [[Float]], grid: SongGrid, rate: Double, sessionBar: Int64?,
                        progress: @escaping (Int, Int) -> Void) throws -> PreparedSong {
        let songBar = grid.barSeconds * rate
        let bar = sessionBar ?? Int64(songBar.rounded())
        let ratio = Double(bar) / songBar
        var converted = try stems.map { rate == StemSeparator.sampleRate ? $0 : try SessionFiles.resample($0, from: StemSeparator.sampleRate, to: rate) }
        if abs(ratio - 1) < 0.002 {
            // Within 0.2% (3 cents): read it a touch faster or slower.
            converted = converted.enumerated().map { index, stem in
                defer { progress(index + 1, stems.count) }
                return TimeStretch.read(stem, count: Int((Double(stem.count) * ratio).rounded()), start: 0, step: 1 / ratio)
            }
        } else {
            converted = try TimeStretch.stretch(converted, sampleRate: rate, ratio: ratio) { progress($0, stems.count) }
        }
        let downbeat = Int64((grid.downbeat * rate * ratio).rounded())
        return PreparedSong(stems: converted, barLength: bar, downbeat: downbeat)
    }
}
