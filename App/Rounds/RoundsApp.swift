import SwiftUI

@main
struct RoundsApp: App {
    @StateObject private var model = LooperModel()

    var body: some Scene {
        Window("Rounds", id: "main") {
            ContentView()
                .environmentObject(model)
        }
        .windowResizability(.contentMinSize)
        .commands {
            CommandGroup(replacing: .newItem) {
                Button("New Session") { model.newSession() }
                    .keyboardShortcut("n")
                    .disabled(!model.audioRunning || model.sessionBusy)
                Button("Open Session…") { model.openSession() }
                    .keyboardShortcut("o")
                    .disabled(!model.audioRunning || model.sessionBusy)
            }
            CommandGroup(replacing: .saveItem) {
                Button("Save Session") { model.saveSession() }
                    .keyboardShortcut("s")
                    .disabled(!model.audioRunning || model.sessionBusy)
                Button("Save Session As…") { model.saveSessionAs() }
                    .keyboardShortcut("s", modifiers: [.command, .shift])
                    .disabled(!model.audioRunning || model.sessionBusy)
            }
        }
    }
}
