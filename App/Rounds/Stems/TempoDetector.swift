import Accelerate
import Foundation

/// A song's beat grid: a steady tempo and where bar 1 falls. Found from the
/// drums stem, and adjustable in the import window.
struct SongGrid: Equatable {
    var bpm: Double
    /// Seconds from the start of the song to the first downbeat (0 ..< one bar).
    var downbeat: Double

    var barSeconds: Double { 240 / bpm }
    var beatSeconds: Double { 60 / bpm }

    /// Where bar `bar` (from 1) starts, in seconds.
    func barStart(_ bar: Int) -> Double { downbeat + Double(bar - 1) * barSeconds }

    func bars(in seconds: Double) -> Int { max(1, Int(((seconds - downbeat) / barSeconds).rounded(.down))) }

    /// Moves bar 1 by `beats` beats (keeping it within the first bar).
    func shifted(beats: Int) -> SongGrid {
        var grid = self
        grid.downbeat = (downbeat + Double(beats) * beatSeconds).truncatingRemainder(dividingBy: barSeconds)
        if grid.downbeat < 0 { grid.downbeat += barSeconds }
        return grid
    }
}

/// Tempo from onsets: a spectral-flux envelope (10 ms frames), its
/// autocorrelation for the rough tempo (60-200 BPM, favouring ~120), then a
/// comb over the whole song for the exact tempo and the beat. The downbeat is
/// the beat of the bar with the most low-end (kick) onsets.
enum TempoDetector {
    static let hop = 441          // 10 ms at 44.1 kHz
    static let frameSize = 2048

    static func detect(_ samples: [Float], sampleRate: Double) -> SongGrid? {
        let hopSeconds = Double(hop) / sampleRate
        let (all, low) = flux(samples, sampleRate: sampleRate)
        guard all.count > 400 else { return nil }  // under 4 s
        let envelope = normalized(all)
        let kicks = normalized(low)

        // Rough tempo from the autocorrelation.
        var best = (score: -Double.infinity, lag: 0)
        let minLag = Int(60 / 200 / hopSeconds), maxLag = Int(60 / 60 / hopSeconds)
        for lag in minLag...maxLag {
            var sum = 0.0
            for i in lag..<envelope.count { sum += Double(envelope[i] * envelope[i - lag]) }
            let bpm = 60 / (Double(lag) * hopSeconds)
            let prior = exp(-0.5 * pow(log2(bpm / 120) / 0.9, 2))
            let score = sum / Double(envelope.count - lag) * prior
            if score > best.score { best = (score, lag) }
        }
        guard best.lag > 0 else { return nil }

        // Exact tempo and beat phase: the period (in frames, fractional) and
        // offset whose beats land on the most onset energy across the song.
        // Over a whole song this peak is very narrow (a hundredth of a frame
        // per beat adds up), so search finely, on a slightly smoothed envelope.
        // Three minutes from the middle are plenty for the tempo (and keep a
        // long live recording quick); the beat's phase is still from frame 0.
        let smooth = smoothed(envelope)
        let span = min(smooth.count, 18_000)
        let window = (smooth.count - span) / 2..<((smooth.count - span) / 2 + span)
        var period = Double(best.lag)
        var phase = 0.0
        var bestScore = -Double.infinity
        var p = Double(best.lag) - 2
        while p <= Double(best.lag) + 2 {
            let (s, ph) = comb(smooth, period: p, in: window)
            if s > bestScore { (bestScore, period, phase) = (s, p, ph) }
            p += 0.02
        }
        let beat = period * hopSeconds
        // Downbeat: of the four beats, the one with the most kick.
        var downbeat = 0
        var kickBest = -Double.infinity
        for b in 0..<4 {
            var sum = 0.0
            var t = phase + Double(b) * period
            while t < Double(kicks.count - 1) {
                sum += sample(kicks, t)
                t += 4 * period
            }
            if sum > kickBest { (kickBest, downbeat) = (sum, b) }
        }
        let first = (phase + Double(downbeat) * period) * hopSeconds
        let grid = SongGrid(bpm: 60 / beat, downbeat: first.truncatingRemainder(dividingBy: 4 * beat))
        return snapped(grid, to: samples, sampleRate: sampleRate)
    }

    /// The flux frames only place beats to within a frame or two, so move the
    /// grid onto the attacks themselves: for every beat, where the sharpest
    /// rise in 1.5 ms energy falls within 40 ms of it. A straight line through
    /// those offsets (after dropping strays) corrects the start and the tempo,
    /// which matters over a long song: 0.005% is 40 ms by the end of 15 minutes.
    private static func snapped(_ grid: SongGrid, to x: [Float], sampleRate: Double) -> SongGrid {
        let block = 64
        var energy = [Float](repeating: 0, count: x.count / block)
        x.withUnsafeBufferPointer { p in
            for i in energy.indices { energy[i] = vDSP.sumOfSquares(UnsafeBufferPointer(rebasing: p[(i * block)..<((i + 1) * block)])) }
        }
        let reach = Int(0.04 * sampleRate) / block
        var offsets: [(beat: Double, offset: Double)] = []
        var beat = 0.0
        var t = grid.downbeat
        while t * sampleRate / Double(block) + Double(reach) + 1 < Double(energy.count) {
            let center = Int((t * sampleRate / Double(block)).rounded())
            var best = (rise: Float(0), at: center)
            for i in max(1, center - reach)...(center + reach) {
                let rise = energy[i] - energy[i - 1]
                if rise > best.rise { best = (rise, i) }
            }
            if best.rise > 0 { offsets.append((beat, Double(best.at - center) * Double(block) / sampleRate)) }
            t += grid.beatSeconds
            beat += 1
        }
        guard offsets.count >= 8 else { return grid }
        // Start from the median (no slope), then fit twice, each time keeping
        // the beats within 8 ms of the line.
        var line = (intercept: offsets.map(\.offset).sorted()[offsets.count / 2], slope: 0.0)
        for _ in 0..<2 {
            let kept = offsets.filter { abs($0.offset - (line.intercept + line.slope * $0.beat)) < 0.008 }
            guard kept.count >= 8 else { break }
            let n = Double(kept.count)
            let mb = kept.map(\.beat).reduce(0, +) / n
            let mo = kept.map(\.offset).reduce(0, +) / n
            let sbb = kept.map { ($0.beat - mb) * ($0.beat - mb) }.reduce(0, +)
            let sbo = kept.map { ($0.beat - mb) * ($0.offset - mo) }.reduce(0, +)
            let slope = sbb > 0 ? sbo / sbb : 0
            line = (mo - slope * mb, slope)
        }
        var moved = grid
        moved.bpm = 60 / (grid.beatSeconds + line.slope)
        moved.downbeat += line.intercept
        return moved.shifted(beats: 0)
    }

    /// Mean of the envelope on a beat grid within `range` (frames), best over
    /// all phases (measured from frame 0).
    private static func comb(_ env: [Float], period: Double, in range: Range<Int>) -> (Double, Double) {
        var best = (-Double.infinity, 0.0)
        var phase = 0.0
        let end = Double(min(range.upperBound, env.count - 1))
        while phase < period {
            var sum = 0.0
            var count = 0
            var t = phase + (Double(range.lowerBound) - phase > 0 ? ((Double(range.lowerBound) - phase) / period).rounded(.up) * period : 0)
            while t < end {
                sum += sample(env, t)
                count += 1
                t += period
            }
            let score = sum / Double(max(1, count))
            if score > best.0 { best = (score, phase) }
            phase += 0.25
        }
        return best
    }

    private static func sample(_ x: [Float], _ t: Double) -> Double {
        let i = Int(t)
        let f = t - Double(i)
        return Double(x[i]) * (1 - f) + Double(x[i + 1]) * f
    }

    /// Onset strength: positive change in log magnitude per frame, over all
    /// bins and below 150 Hz.
    private static func flux(_ x: [Float], sampleRate: Double) -> ([Float], [Float]) {
        let n = frameSize
        let log2n = vDSP_Length(log2(Double(n)))
        guard x.count > n, let fft = vDSP.FFT(log2n: log2n, radix: .radix2, ofType: DSPSplitComplex.self) else { return ([], []) }
        let window = vDSP.window(ofType: Float.self, usingSequence: .hanningDenormalized, count: n, isHalfWindow: false)
        let lowBins = Int(150 / (sampleRate / Double(n)))
        var previous = [Float](repeating: 0, count: n / 2)
        var all: [Float] = []
        var low: [Float] = []
        var real = [Float](repeating: 0, count: n / 2)
        var imag = [Float](repeating: 0, count: n / 2)
        var frame = [Float](repeating: 0, count: n)
        var magnitude = [Float](repeating: 0, count: n / 2)
        var start = 0
        while start + n <= x.count {
            x.withUnsafeBufferPointer { frame = vDSP.multiply(Array($0[start..<start + n]), window) }
            real.withUnsafeMutableBufferPointer { r in
                imag.withUnsafeMutableBufferPointer { i in
                    var split = DSPSplitComplex(realp: r.baseAddress!, imagp: i.baseAddress!)
                    frame.withUnsafeBytes { vDSP_ctoz($0.bindMemory(to: DSPComplex.self).baseAddress!, 2, &split, 1, vDSP_Length(n / 2)) }
                    fft.forward(input: split, output: &split)
                    vDSP.absolute(split, result: &magnitude)
                }
            }
            var sumAll: Float = 0
            var sumLow: Float = 0
            for k in 1..<(n / 2) {
                let m = log1p(100 * magnitude[k])
                let d = max(0, m - previous[k])
                sumAll += d
                if k <= lowBins { sumLow += d }
                previous[k] = m
            }
            all.append(sumAll)
            low.append(sumLow)
            start += hop
        }
        return (all, low)
    }

    /// A 5-frame Gaussian blur (σ ≈ 1 frame).
    private static func smoothed(_ x: [Float]) -> [Float] {
        let k: [Float] = [0.054, 0.242, 0.4, 0.242, 0.054]
        var out = [Float](repeating: 0, count: x.count)
        for i in 2..<max(2, x.count - 2) {
            out[i] = k[0] * x[i - 2] + k[1] * x[i - 1] + k[2] * x[i] + k[3] * x[i + 1] + k[4] * x[i + 2]
        }
        return out
    }

    /// Minus a moving average (about half a second), rectified.
    private static func normalized(_ x: [Float]) -> [Float] {
        let w = 25
        var out = [Float](repeating: 0, count: x.count)
        var sum: Float = 0
        for i in 0..<x.count {
            sum += x[i]
            if i >= 2 * w + 1 { sum -= x[i - 2 * w - 1] }
            let center = i - w
            if center >= 0 { out[center] = max(0, x[center] - sum / Float(min(i + 1, 2 * w + 1))) }
        }
        return out
    }
}
