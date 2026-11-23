// swift-tools-version: 5.9
//
// SwiftPM package for InkEngine, so iOS apps built with SwiftPM-based tools
// (xtool on Linux/WSL, Xcode on macOS) can depend on the engine directly:
//
//   .package(url: "https://github.com/lemon7679/note-core.git", branch: "main")
//   .product(name: "InkEngine", package: "note-core")
//
// Swift sees only the C API (`engine/include/ink/ink_c.h`) through the bundled
// module.modulemap, so app code needs no C++ interop: `import InkEngine`.
import PackageDescription

let package = Package(
    name: "note-core",
    platforms: [
        .iOS(.v16),
        .macOS(.v13),
    ],
    products: [
        .library(name: "InkEngine", targets: ["InkEngine"]),
    ],
    targets: [
        .target(
            name: "InkEngine",
            path: "engine",
            exclude: [
                "CMakeLists.txt",
                "shaders",
                "tests",
                "tools",
            ],
            sources: ["src"],
            publicHeadersPath: "include",
            cxxSettings: [
                .headerSearchPath("include"),
                .define("NDEBUG", .when(configuration: .release)),
            ]
        ),
    ],
    cxxLanguageStandard: .cxx17
)
