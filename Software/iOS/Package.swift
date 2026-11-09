// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "WearableCore",
    platforms: [.iOS(.v17), .macOS(.v13)],
    products: [.library(name: "WearableCore", targets: ["WearableCore"])],
    targets: [
        .target(name: "WearableCore"),
        .testTarget(name: "WearableCoreTests", dependencies: ["WearableCore"], resources: [.copy("Fixtures")])
    ]
)
