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
    }
}
