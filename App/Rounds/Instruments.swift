import Foundation

/// The engine's built-in sounds (RoundsInstrument). Each loop slot owns one.
enum Instrument: Int32, CaseIterable, Identifiable {
    case subBass, fingerBass, synthBass, ePiano, warmPad, monoLead, dryKit, electroKit

    var id: Int32 { rawValue }

    var name: String {
        switch self {
        case .subBass: return "Sub Bass"
        case .fingerBass: return "Finger Bass"
        case .synthBass: return "Synth Bass"
        case .ePiano: return "E-Piano"
        case .warmPad: return "Warm Pad"
        case .monoLead: return "Mono Lead"
        case .dryKit: return "Dry Kit"
        case .electroKit: return "Electro Kit"
        }
    }

    var isKit: Bool { self == .dryKit || self == .electroKit }

    /// What the keys browse (Track ◀ ▶) and the kits the pads browse.
    static let synths: [Instrument] = [.subBass, .fingerBass, .synthBass, .ePiano, .warmPad, .monoLead]
    static let kits: [Instrument] = [.dryKit, .electroKit]

    init?(name: String) {
        guard let match = Self.allCases.first(where: { $0.name == name }) else { return nil }
        self = match
    }
}

/// The bottom pad row: General MIDI drum notes (Ableton names, C1 = 36).
enum DrumPads {
    static let names = ["Kick", "Snare", "Clap", "Closed hat", "Open hat", "Low tom", "Shaker", "Rim"]
    static let notes: [Int32] = [36, 38, 39, 42, 46, 45, 58, 37]
    static let noteNames = ["C1", "D1", "D#1", "F#1", "A#1", "A1", "A#2", "C#1"]
}
