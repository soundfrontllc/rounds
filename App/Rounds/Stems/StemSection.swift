import Foundation

/// Turns bars of a stem into a loop.
enum StemSection {
    /// The section of one stem as a loop: `target` samples at `rate`, made
    /// from exactly `bars` of the song (stretched if the tempo differs), with
    /// the audio just past the end cross-faded into the start so the seam
    /// doesn't click.
    static func loop(_ stem: [Float], start: Double, length: Double, target: Int, rate: Double,
                     fade: Int) throws -> [Float] {
        let margin = 0.3 * StemSeparator.sampleRate
        // The section plus margin either side (silence past the song's ends).
        let from = Int((start - margin).rounded(.down))
        let count = Int((length + 2 * margin).rounded(.up)) + 2
        var source = [Float](repeating: 0, count: count)
        for i in 0..<count where from + i >= 0 && from + i < stem.count { source[i] = stem[from + i] }
        var offset = start - Double(from)  // where the section starts in `source`, 44.1 kHz samples
        if rate != StemSeparator.sampleRate {
            source = try SessionFiles.resample(source, from: StemSeparator.sampleRate, to: rate)
            offset *= rate / StemSeparator.sampleRate
        }
        let natural = length * rate / StemSeparator.sampleRate  // the section at its own speed, in output samples
        let ratio = Double(target) / natural
        var out: [Float]
        if abs(ratio - 1) < 0.002 {
            // Within 0.2% (3 cents): just read it a touch faster or slower.
            out = (0..<(target + fade)).map { u in
                let t = offset + Double(u) / ratio
                let i = Int(t)
                guard i >= 0, i + 1 < source.count else { return 0 }
                let f = Float(t - Double(i))
                return source[i] * (1 - f) + source[i + 1] * f
            }
        } else {
            let stretched = try TimeStretch.stretch(source, sampleRate: rate, ratio: ratio)
            let first = Int((offset * ratio).rounded())
            out = (0..<(target + fade)).map { first + $0 < stretched.count ? stretched[first + $0] : 0 }
        }
        for k in 0..<fade {
            let g = Float(k) / Float(fade)
            out[k] = out[k] * g + out[target + k] * (1 - g)
        }
        return Array(out[0..<target])
    }
}
