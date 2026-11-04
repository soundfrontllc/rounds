import Accelerate
import AVFoundation
import Foundation

/// Stereo audio for stem import. Everything here runs off the audio thread.
struct StereoAudio {
    var left: [Float]
    var right: [Float]
    var sampleRate: Double

    var count: Int { left.count }
    var seconds: Double { Double(count) / sampleRate }

    var mono: [Float] {
        var out = [Float](repeating: 0, count: count)
        vDSP_vasm(left, 1, right, 1, [Float(0.5)], &out, 1, vDSP_Length(count))
        return out
    }

    /// Linear peak and RMS over both channels.
    var peak: Float { max(vDSP.maximumMagnitude(left), vDSP.maximumMagnitude(right)) }
    var rms: Float { count == 0 ? 0 : (vDSP.rootMeanSquare(left) + vDSP.rootMeanSquare(right)) / 2 }

    /// Reads any file AVAudioFile can open (WAV, AIFF, CAF, MP3, M4A, FLAC) as
    /// stereo at `sampleRate`: mono is doubled, channels past two are dropped.
    static func read(_ url: URL, sampleRate: Double) throws -> StereoAudio {
        let file = try AVAudioFile(forReading: url)
        let format = file.processingFormat
        let channels = Int(format.channelCount)
        guard channels > 0, let buffer = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: 65536) else {
            throw SessionError.message("\(url.lastPathComponent) has no audio")
        }
        var left: [Float] = []
        var right: [Float] = []
        left.reserveCapacity(Int(file.length))
        right.reserveCapacity(Int(file.length))
        while file.framePosition < file.length {
            try file.read(into: buffer)
            let frames = Int(buffer.frameLength)
            guard frames > 0 else { break }
            guard let data = buffer.floatChannelData else {
                throw SessionError.message("Unsupported audio in \(url.lastPathComponent)")
            }
            left.append(contentsOf: UnsafeBufferPointer(start: data[0], count: frames))
            right.append(contentsOf: UnsafeBufferPointer(start: data[min(1, channels - 1)], count: frames))
        }
        return try StereoAudio(left: left, right: right, sampleRate: format.sampleRate).converted(to: sampleRate)
    }

    func converted(to rate: Double) throws -> StereoAudio {
        guard rate != sampleRate else { return self }
        guard let inFormat = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: sampleRate, channels: 2, interleaved: false),
              let outFormat = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: rate, channels: 2, interleaved: false),
              let input = AVAudioPCMBuffer(pcmFormat: inFormat, frameCapacity: AVAudioFrameCount(max(1, count))),
              let converter = AVAudioConverter(from: inFormat, to: outFormat),
              let output = AVAudioPCMBuffer(pcmFormat: outFormat,
                                            frameCapacity: AVAudioFrameCount(Double(count) * rate / sampleRate) + 8192)
        else { throw SessionError.message("Couldn't convert the sample rate") }
        input.frameLength = AVAudioFrameCount(count)
        left.withUnsafeBufferPointer { input.floatChannelData![0].update(from: $0.baseAddress!, count: count) }
        right.withUnsafeBufferPointer { input.floatChannelData![1].update(from: $0.baseAddress!, count: count) }
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
        let length = min(Int(output.frameLength), Int((Double(count) * rate / sampleRate).rounded()))
        return StereoAudio(left: Array(UnsafeBufferPointer(start: output.floatChannelData![0], count: length)),
                           right: Array(UnsafeBufferPointer(start: output.floatChannelData![1], count: length)),
                           sampleRate: rate)
    }
}
