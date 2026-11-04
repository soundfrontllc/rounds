import Accelerate
import CoreML
import CryptoKit
import Foundation
import OnnxRuntimeBindings

/// HT-Demucs 6-stem (Meta's model, MIT licence) exported to ONNX by StemSplit.
/// Downloaded once into Application Support and checked against its SHA-256.
enum StemModel {
    static let fileName = "htdemucs_6s_fp16weights.onnx"
    static let url = URL(string: "https://huggingface.co/StemSplitio/htdemucs-6s-onnx/resolve/"
                         + "49df9b6989cf2150840ea65b0bef77a2e471b678/htdemucs_6s_fp16weights.onnx")!
    static let sha256 = "7ce55792e2231c93fbf92de95f5fd5b3a5e6c89f7db690dfd693e8f1dce56869"
    static let bytes: Int64 = 136_428_532

    static var folder: URL {
        FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("Rounds/Models")
    }
    static var location: URL { folder.appendingPathComponent(fileName) }

    /// The same model converted to Core ML at FP16 (scripts/convert_stem_model.py):
    /// about 3.5x faster on the CPU, stems within 28-41 dB of the original.
    static var coreMLLocation: URL { folder.appendingPathComponent("HTDemucs6S_F16.mlpackage") }
    static var compiledLocation: URL { folder.appendingPathComponent("HTDemucs6S_F16.mlmodelc") }
    static var hasCoreML: Bool { FileManager.default.fileExists(atPath: coreMLLocation.path) }

    static var isInstalled: Bool { hasCoreML || FileManager.default.fileExists(atPath: location.path) }

    /// Downloads into place, reporting the fraction done; the file only
    /// lands at `location` once its hash matches.
    static func download(progress: @escaping @Sendable (Double) -> Void) async throws {
        let delegate = DownloadProgress(progress)
        let (temp, response) = try await URLSession.shared.download(from: url, delegate: delegate)
        defer { try? FileManager.default.removeItem(at: temp) }
        guard (response as? HTTPURLResponse)?.statusCode == 200 else {
            throw SessionError.message("The stem model couldn't be downloaded (HTTP \((response as? HTTPURLResponse)?.statusCode ?? 0))")
        }
        guard try hash(of: temp) == sha256 else {
            throw SessionError.message("The downloaded stem model didn't match its checksum, so it wasn't used")
        }
        let fm = FileManager.default
        try fm.createDirectory(at: location.deletingLastPathComponent(), withIntermediateDirectories: true)
        try? fm.removeItem(at: location)
        try fm.moveItem(at: temp, to: location)
    }

    private static func hash(of file: URL) throws -> String {
        let handle = try FileHandle(forReadingFrom: file)
        defer { try? handle.close() }
        var hasher = SHA256()
        while let chunk = try handle.read(upToCount: 1 << 22), !chunk.isEmpty { hasher.update(data: chunk) }
        return hasher.finalize().map { String(format: "%02x", $0) }.joined()
    }

    private final class DownloadProgress: NSObject, URLSessionTaskDelegate, @unchecked Sendable {
        let report: @Sendable (Double) -> Void
        var observation: NSKeyValueObservation?
        init(_ report: @escaping @Sendable (Double) -> Void) { self.report = report }
        func urlSession(_ session: URLSession, didCreateTask task: URLSessionTask) {
            observation = task.progress.observe(\.fractionCompleted) { [report] p, _ in report(p.fractionCompleted) }
        }
    }
}

/// Splits a song into drums, bass, other, vocals, guitar and piano. The model
/// takes 7.8 s of 44.1 kHz stereo at a time; segments overlap by a quarter
/// and are cross-faded, as in the model's reference inference.
final class StemSeparator {
    static let names = ["Drums", "Bass", "Other", "Vocals", "Guitar", "Piano"]
    static let sampleRate = 44100.0
    static let segment = 343_980

    private enum Backend {
        case onnx(ORTEnv, ORTSession)
        case coreML(MLModel)
    }
    private let backend: Backend
    var engineName: String {
        if case .coreML = backend { return "Core ML" }
        return "onnxruntime"
    }

    /// Core ML on the CPU when its model is installed. Not the Neural Engine
    /// (its FP16 maths throws some stems far off: 0-21 dB) or the GPU (it
    /// hangs on this model). Otherwise onnxruntime on the CPU.
    static func make() throws -> StemSeparator {
        if StemModel.hasCoreML { return try StemSeparator(coreML: StemModel.coreMLLocation) }
        return try StemSeparator(onnx: StemModel.location)
    }

    init(onnx modelURL: URL) throws {
        let env = try ORTEnv(loggingLevel: .warning)
        let options = try ORTSessionOptions()
        try options.setGraphOptimizationLevel(.all)
        try options.setIntraOpNumThreads(Int32(max(1, ProcessInfo.processInfo.activeProcessorCount - 2)))
        backend = .onnx(env, try ORTSession(env: env, modelPath: modelURL.path, sessionOptions: options))
    }

    /// Compiles the package the first time (kept beside it), then loads that.
    init(coreML package: URL) throws {
        let compiled = package.deletingPathExtension().appendingPathExtension("mlmodelc")
        let fm = FileManager.default
        if !fm.fileExists(atPath: compiled.path) {
            let temp = try MLModel.compileModel(at: package)
            try? fm.removeItem(at: compiled)
            try fm.moveItem(at: temp, to: compiled)
        }
        let config = MLModelConfiguration()
        config.computeUnits = .cpuOnly
        backend = .coreML(try MLModel(contentsOf: compiled, configuration: config))
    }

    /// Runs one segment (`x`: 2 x n) and hands `body` the stems (12 x n).
    private func infer(_ x: UnsafeMutablePointer<Float>, body: (UnsafePointer<Float>) -> Void) throws {
        let n = Self.segment
        let count = Self.names.count * 2 * n
        switch backend {
        case let .onnx(_, session):
            let input = NSMutableData(bytesNoCopy: x, length: 2 * n * MemoryLayout<Float>.size, freeWhenDone: false)
            let mix = try ORTValue(tensorData: input, elementType: .float, shape: [1, 2, NSNumber(value: n)])
            let result = try session.run(withInputs: ["mix": mix], outputNames: ["stems"], runOptions: nil)
            guard let data = try result["stems"]?.tensorData(), data.length == count * MemoryLayout<Float>.size else {
                throw SessionError.message("The stem model returned an unexpected shape")
            }
            body(data.bytes.assumingMemoryBound(to: Float.self))
        case let .coreML(model):
            let input = try MLMultiArray(dataPointer: x, shape: [1, 2, NSNumber(value: n)], dataType: .float32,
                                         strides: [NSNumber(value: 2 * n), NSNumber(value: n), 1], deallocator: nil)
            let result = try model.prediction(from: MLDictionaryFeatureProvider(dictionary: ["mix": input]))
            guard let stems = result.featureValue(for: "stems")?.multiArrayValue, stems.count == count,
                  stems.strides.last == 1, stems.strides.first?.intValue == count else {
                throw SessionError.message("The stem model returned an unexpected shape")
            }
            switch stems.dataType {
            case .float32:
                stems.withUnsafeBufferPointer(ofType: Float.self) { body($0.baseAddress!) }
            case .float16:
                var floats = [Float](repeating: 0, count: count)
                floats.withUnsafeMutableBufferPointer { f in
                    var source = vImage_Buffer(data: stems.dataPointer, height: 1, width: vImagePixelCount(count), rowBytes: count * 2)
                    var dest = vImage_Buffer(data: f.baseAddress!, height: 1, width: vImagePixelCount(count), rowBytes: count * 4)
                    vImageConvert_Planar16FtoPlanarF(&source, &dest, vImage_Flags(kvImageNoFlags))
                }
                floats.withUnsafeBufferPointer { body($0.baseAddress!) }
            default:
                throw SessionError.message("The stem model returned an unexpected type")
            }
        }
    }

    /// `song` must be at 44.1 kHz. `progress(done, total)` runs after each
    /// segment; returning false stops (and throws CancellationError).
    func separate(_ song: StereoAudio, progress: (Int, Int) -> Bool) throws -> [StereoAudio] {
        precondition(song.sampleRate == Self.sampleRate)
        let n = Self.segment
        let overlap = n / 4
        let stride = n - overlap
        let total = song.count
        let chunks = max(1, (total + stride - 1) / stride)
        var window = [Float](repeating: 1, count: n)
        for i in 0..<overlap {
            let fade = Float(i) / Float(overlap - 1)
            window[i] = fade
            window[n - 1 - i] = fade
        }
        let stems = Self.names.count
        var out = [[Float]](repeating: [Float](repeating: 0, count: total), count: stems * 2)
        var weight = [Float](repeating: 0, count: total)
        let input = UnsafeMutablePointer<Float>.allocate(capacity: 2 * n)
        defer { input.deallocate() }

        for chunk in 0..<chunks {
            let start = chunk * stride
            let length = min(n, total - start)
            guard length > 0 else { break }
            input.update(repeating: 0, count: 2 * n)
            song.left.withUnsafeBufferPointer { input.update(from: $0.baseAddress! + start, count: length) }
            song.right.withUnsafeBufferPointer { (input + n).update(from: $0.baseAddress! + start, count: length) }
            try infer(input) { y in
                // out += y * window, a stem channel at a time (vDSP: fast in Debug builds too).
                for c in 0..<(stems * 2) {
                    out[c].withUnsafeMutableBufferPointer { dest in
                        vDSP_vma(y + c * n, 1, window, 1, dest.baseAddress! + start, 1, dest.baseAddress! + start, 1, vDSP_Length(length))
                    }
                }
            }
            weight.withUnsafeMutableBufferPointer { w in
                vDSP_vadd(w.baseAddress! + start, 1, window, 1, w.baseAddress! + start, 1, vDSP_Length(length))
            }
            guard progress(chunk + 1, chunks) else { throw CancellationError() }
        }
        var floor: Float = 1e-8
        vDSP_vthr(weight, 1, &floor, &weight, 1, vDSP_Length(total))
        weight = vDSP.divide(1, weight)
        return (0..<stems).map { s in
            StereoAudio(left: vDSP.multiply(out[2 * s], weight), right: vDSP.multiply(out[2 * s + 1], weight),
                        sampleRate: Self.sampleRate)
        }
    }
}
