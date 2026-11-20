// swift-tools-version: 5.9

import PackageDescription

let package = Package(
    name: "AgentKit",
    platforms: [
        .iOS(.v15),
    ],
    products: [
        .library(
            name: "AgentKit",
            targets: ["AgentKit"]
        ),
    ],
    targets: [
        .target(name: "AgentKit"),
    ]
)
