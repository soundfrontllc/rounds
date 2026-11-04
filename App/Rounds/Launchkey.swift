import Foundation

/// The Launchkey 37 MK3's surface in DAW mode, per Novation's "Launchkey [MK3]
/// Programmer's Reference Guide" v1. Pure protocol: turns raw MIDI from the
/// DAW Out port into controls, and builds the messages sent to DAW In.
///
/// In DAW mode the keys, wheels and sustain pedal (the FC5) stay on the MIDI
/// port; pads, pots and buttons report on the DAW port only.
enum Launchkey {
    /// Endpoint names (Core MIDI kMIDIPropertyName), shared by every MK3 size.
    static let dawInName = "LKMK3 DAW In"
    static let dawOutName = "LKMK3 DAW Out"

    enum Control: Equatable {
        case loopPad(Int, down: Bool)            // top row, 0...7
        case drumPad(Int, velocity: Int)         // bottom row, 0...7; velocity 0 = release
        case knob(Int, value: Int)               // 0...7, absolute 0...127
        case shift(down: Bool)
        case play(down: Bool)
        case record(down: Bool)
        case trackLeft(down: Bool)
        case trackRight(down: Bool)
        case dawMode(on: Bool)                   // the Launchkey echoes DAW mode changes back
        case padPressure                         // polyphonic aftertouch while a pad is held
        case padMode(Int)                        // reported when the user changes it
        case potMode(Int)
        case other(String)
    }

    enum LEDMode: UInt8 {
        case solid = 0, flashing = 1, pulsing = 2   // MIDI channels 1, 2, 3
    }

    /// Palette indices (velocity) from the guide's colour table.
    enum Color {
        static let off: UInt8 = 0
        static let white: UInt8 = 3
        static let dimWhite: UInt8 = 1
        /// The dim shade of a palette colour (each colour has 4 shades, brightest first).
        static func dim(_ color: UInt8) -> UInt8 { color == white ? dimWhite : color + 2 }
        static let red: UInt8 = 5
        static let orange: UInt8 = 9
        static let yellow: UInt8 = 13
        static let green: UInt8 = 21
        static let cyan: UInt8 = 37
        static let blue: UInt8 = 45
        static let purple: UInt8 = 49
        static let pink: UInt8 = 53
    }

    // Session mode pad notes (channel 1): top row 60h-67h, bottom row 70h-77h.
    private static let topPadNote: UInt8 = 0x60
    private static let bottomPadNote: UInt8 = 0x70
    // Pots: CC 15h-1Ch on channel 16.
    private static let firstKnobCC: UInt8 = 0x15

    // MARK: Host -> Launchkey (DAW In)

    static let enterDAWMode: [UInt8] = [0x9F, 0x0C, 0x7F]
    static let exitDAWMode: [UInt8] = [0x9F, 0x0C, 0x00]
    static let padModeSession: [UInt8] = [0xBF, 0x03, 0x02]
    static let potModeVolume: [UInt8] = [0xBF, 0x09, 0x01]

    static func loopPadLED(_ pad: Int, color: UInt8, mode: LEDMode = .solid) -> [UInt8] {
        [0x90 | mode.rawValue, topPadNote + UInt8(pad), color]
    }

    static func drumPadLED(_ pad: Int, color: UInt8, mode: LEDMode = .solid) -> [UInt8] {
        [0x90 | mode.rawValue, bottomPadNote + UInt8(pad), color]
    }

    // MARK: Launchkey (DAW Out) -> host

    static func parse(_ bytes: [UInt8]) -> Control? {
        guard bytes.count == 3 else { return nil }
        let status = bytes[0] & 0xF0
        let channel = bytes[0] & 0x0F
        let data1 = bytes[1]
        let data2 = Int(bytes[2])

        switch (status, channel) {
        case (0x90, 0), (0x80, 0):
            let velocity = status == 0x80 ? 0 : data2
            if (topPadNote..<topPadNote + 8).contains(data1) {
                return .loopPad(Int(data1 - topPadNote), down: velocity > 0)
            }
            if (bottomPadNote..<bottomPadNote + 8).contains(data1) {
                return .drumPad(Int(data1 - bottomPadNote), velocity: velocity)
            }
        case (0xB0, 15):
            if (firstKnobCC..<firstKnobCC + 8).contains(data1) {
                return .knob(Int(data1 - firstKnobCC), value: data2)
            }
            switch data1 {
            case 0x03: return .padMode(data2)
            case 0x09: return .potMode(data2)
            case 0x66: return .trackLeft(down: data2 > 0)
            case 0x67: return .trackRight(down: data2 > 0)
            case 0x73: return .play(down: data2 > 0)
            case 0x75: return .record(down: data2 > 0)
            default: break
            }
        case (0xB0, 0):
            if data1 == 0x6C { return .shift(down: data2 > 0) }
        case (0xA0, 0):
            return .padPressure
        case (0x90, 15), (0x80, 15):
            if data1 == 0x0C { return .dawMode(on: status == 0x90 && data2 > 0) }
        default:
            break
        }
        return .other(bytes.map { String(format: "%02X", $0) }.joined(separator: " "))
    }
}

extension Launchkey.Control {
    var logText: String {
        switch self {
        case let .loopPad(i, down): return "Loop pad \(i + 1) \(down ? "down" : "up")"
        case let .drumPad(i, velocity): return velocity > 0 ? "Drum pad \(i + 1) hit, velocity \(velocity)" : "Drum pad \(i + 1) up"
        case let .knob(i, value): return "Knob \(i + 1) → \(value)"
        case let .shift(down): return "Shift \(down ? "down" : "up")"
        case let .play(down): return "Play \(down ? "down" : "up")"
        case let .record(down): return "Record \(down ? "down" : "up")"
        case let .trackLeft(down): return "Track ◀ \(down ? "down" : "up")"
        case let .trackRight(down): return "Track ▶ \(down ? "down" : "up")"
        case let .dawMode(on): return "DAW mode \(on ? "on" : "off")"
        case .padPressure: return "Pad pressure"
        case let .padMode(mode): return "Pad mode → \(mode)\(mode == 2 ? " (Session)" : "")"
        case let .potMode(mode): return "Pot mode → \(mode)\(mode == 1 ? " (Volume)" : "")"
        case let .other(hex): return "Unmapped: \(hex)"
        }
    }
}
