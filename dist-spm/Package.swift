// swift-tools-version:5.7
import PackageDescription

let package = Package(
    name: "MapLibre",
    products: [
        .library(name: "MapLibre", targets: ["MapLibre"])
    ],
    targets: [
        .binaryTarget(name: "MapLibre", path: "MapLibre.xcframework")
    ]
)
