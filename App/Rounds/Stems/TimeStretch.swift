import Accelerate
import AVFoundation
import Foundation

/// Offline time-stretch (same pitch) with Apple's TimePitch unit, for fitting
/// a song section to the session's tempo.
enum TimeStretch {
    /// `samples` (mono) made `ratio` times as long, with sample k of the
    /// input landing at k * ratio (to within about a millisecond).
    ///
    /// TimePitch's delay isn't exactly what it reports and its rate is off by
    /// up to ~0.1%, so a click track of the same length goes through it too;
    /// where its clicks come out gives the real timing (a straight line),
    /// and the output is read back along that line.
    static func stretch(_ samples: [Float], sampleRate: Double, ratio: Double) throws -> [Float] {
        try stretch([samples], sampleRate: sampleRate, ratio: ratio)[0]
    }

    /// Several channels of the same length, sharing one timing probe.
    static func stretch(_ channels: [[Float]], sampleRate: Double, ratio: Double,
                        progress: ((Int) -> Void)? = nil) throws -> [[Float]] {
        guard abs(ratio - 1) > 1e-9, let samples = channels.first else { return channels }
        let wanted = Int((Double(samples.count) * ratio).rounded())
        let margin = Int(0.2 * sampleRate)
        let spacing = Int(0.25 * sampleRate)
        var clicks = [Float](repeating: 0, count: samples.count)
        for at in stride(from: spacing / 2, to: samples.count - 256, by: spacing) {
            for j in 0..<64 { clicks[at + j] = sin(Float(j) * 0.7) * (1 - Float(j) / 64) }
        }
        let probe = try render(clicks, sampleRate: sampleRate, ratio: ratio, length: wanted + margin)
        // Least-squares line through (input time, output time) of each click.
        var points: [(Double, Double)] = []
        let reach = Int(0.05 * sampleRate)
        for at in stride(from: spacing / 2, to: samples.count - 256, by: spacing) {
            let expected = Int((Double(at) * ratio).rounded())
            guard expected + reach < probe.count else { break }
            var best = (at: -1, level: Float(0.05))
            for t in max(0, expected - reach)..<(expected + reach) where abs(probe[t]) > best.level { best = (t, abs(probe[t])) }
            if best.at >= 0 { points.append((Double(at), Double(best.at))) }
        }
        var a = 0.0, b = ratio
        if points.count >= 4 {
            let n = Double(points.count)
            let mx = points.map(\.0).reduce(0, +) / n, my = points.map(\.1).reduce(0, +) / n
            let sxy = points.map { ($0.0 - mx) * ($0.1 - my) }.reduce(0, +)
            let sxx = points.map { ($0.0 - mx) * ($0.0 - mx) }.reduce(0, +)
            b = sxy / sxx
            a = my - b * mx
        }
        return try channels.enumerated().map { index, channel in
            let raw = try render(channel, sampleRate: sampleRate, ratio: ratio, length: wanted + margin)
            progress?(index + 1)
            return Self.read(raw, count: wanted, start: a, step: b / ratio)
        }
    }

    /// `count` samples of `x` read at start, start + step, … (linear
    /// interpolation; silence where that falls outside `x`).
    static func read(_ x: [Float], count: Int, start: Double, step: Double) -> [Float] {
        var out = [Float](repeating: 0, count: count)
        guard x.count > 1, step > 0 else { return out }
        let first = max(0, Int((-start / step).rounded(.up)))
        let last = min(count, Int(((Double(x.count - 1) - start) / step).rounded(.up)))
        guard first < last else { return out }
        // vDSP takes the read positions as floats: go in pieces from a whole
        // sample so they stay exact.
        let piece = 1 << 16
        var i = first
        var positions = [Float](repeating: 0, count: piece)
        x.withUnsafeBufferPointer { src in
            out.withUnsafeMutableBufferPointer { dst in
                while i < last {
                    let n = min(piece, last - i)
                    let base = start + Double(i) * step
                    let whole = Int(base.rounded(.down))
                    var offset = Float(base - Double(whole))
                    var increment = Float(step)
                    vDSP_vramp(&offset, &increment, &positions, 1, vDSP_Length(n))
                    vDSP_vlint(src.baseAddress! + whole, positions, 1, dst.baseAddress! + i, 1, vDSP_Length(n),
                               vDSP_Length(src.count - whole))
                    i += n
                }
            }
        }
        return out
    }

    /// TimePitch output with its reported delay removed, `length` samples.
    private static func render(_ samples: [Float], sampleRate: Double, ratio: Double, length: Int) throws -> [Float] {
        guard let format = AVAudioFormat(standardFormatWithSampleRate: sampleRate, channels: 1),
              let input = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: AVAudioFrameCount(max(1, samples.count)))
        else { throw SessionError.message("Couldn't set up the time-stretch") }
        input.frameLength = AVAudioFrameCount(samples.count)
        samples.withUnsafeBufferPointer { input.floatChannelData![0].update(from: $0.baseAddress!, count: samples.count) }

        let engine = AVAudioEngine()
        let player = AVAudioPlayerNode()
        let timePitch = AVAudioUnitTimePitch()
        timePitch.rate = Float(1 / ratio)
        timePitch.overlap = 16  // smoother for sustained parts (default 8)
        // Offline first: touching the mixer before this would open the
        // hardware output (and can block on it).
        let block: AVAudioFrameCount = 4096
        try engine.enableManualRenderingMode(.offline, format: format, maximumFrameCount: block)
        engine.attach(player)
        engine.attach(timePitch)
        engine.connect(player, to: timePitch, format: format)
        engine.connect(timePitch, to: engine.mainMixerNode, format: format)
        try engine.start()
        defer { engine.stop() }
        player.scheduleBuffer(input, completionHandler: nil)
        player.play()

        let latency = Int((timePitch.auAudioUnit.latency * sampleRate).rounded())
        let wanted = length
        guard let output = AVAudioPCMBuffer(pcmFormat: engine.manualRenderingFormat, frameCapacity: block) else {
            throw SessionError.message("Couldn't set up the time-stretch")
        }
        var result: [Float] = []
        result.reserveCapacity(wanted + latency + Int(block))
        while result.count < wanted + latency {
            switch try engine.renderOffline(block, to: output) {
            case .success:
                result.append(contentsOf: UnsafeBufferPointer(start: output.floatChannelData![0], count: Int(output.frameLength)))
            case .insufficientDataFromInputNode, .cannotDoInCurrentContext:
                continue
            case .error:
                throw SessionError.message("The time-stretch failed")
            @unknown default:
                throw SessionError.message("The time-stretch failed")
            }
        }
        return Array(result[latency..<(latency + wanted)])
    }
}
