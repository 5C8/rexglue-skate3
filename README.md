# rexglue-skate3

ReXGlue SDK snapshot used by the [retail](https://github.com/5C8/skate3recomp)
and [milestone](https://github.com/5C8/sk83_na_m) Skate 3 projects.

Based on [mchughalex/rexglue-skate3](https://github.com/mchughalex/rexglue-skate3)
and [ReXGlue](https://github.com/rexglue/rexglue-sdk), with the working Skate 3
runtime fixes retained. This SDK converts Xbox 360 PowerPC executables to C++
and provides the runtime, graphics, input, audio, UI and system services.
No original game files or generated game implementation are included.

## Download and use

Download **rexglue-skate3-Windows-x64.zip** from
[Releases](https://github.com/5C8/rexglue-skate3/releases/latest) and extract it.
The package contains `bin/rexglue.exe`, `bin/rexruntime.dll`, headers, libraries,
CMake packages and dependency notices. Pass its extracted directory as
`-DCMAKE_PREFIX_PATH=C:/Dev/rex-sdk` when configuring either game project.
Keep the SDK folder structure intact. This package targets x86-64-v3 CPUs
(AVX2/BMI2) on Windows. It is a developer SDK, not the game download.

## Included integration changes

- Native rendering interfaces and D3D12/Vulkan integration, texture/fetch and
  shader translation fixes used by the custom renderer.
- Controller assignment across game processes, SDL/XInput support and input
  focus handling for the Windows split-screen launcher.
- Separate profile identity handling and XAM/network/socket behavior used by
  community online play and multiple local game processes.
- UI/presenter/window fixes, configuration handling and graphics diagnostics.
- PowerPC code-generation discovery/scanning improvements needed by these builds.

## Build from source

On Windows, use a short checkout path such as `C:/Dev/rexglue-skate3` to avoid
object-file path limits in the shader-tool dependencies.

Requires Git, CMake 3.25+, Ninja, Clang 18+ (the published Windows build uses
23.1.0), Visual Studio C++ build tools and a Windows SDK. From a developer shell:

```powershell
git clone --recursive https://github.com/5C8/rexglue-skate3.git
cd rexglue-skate3
cmake --preset win-amd64
cmake --build --preset win-amd64-release --parallel 4
cmake --install out/build/win-amd64 --config Release --prefix C:/Dev/rex-sdk
```

If dependencies were omitted, run `git submodule update --init --recursive`.
The pinned Tracy dependency receives the checked-in Windows compatibility patch
at configure time. The source snapshot pins SDK version 0.8.2.94 so source ZIPs
and Git checkouts expose the same consumer package version.

`REXGLUE_BUILD_TESTS` defaults to OFF. Instruction tests additionally require
external **VMX128-capable PowerPC binutils**, passed as `PPC_TOOLS_DIR`; these
prebuilt investigation/test tools are not bundled with the clean source.
The complete runtime/codegen sources and build templates remain included.

The inherited Linux/ARM64 presets require their platform dependencies and their
own validation. This snapshot publishes Windows x64 binaries only. See
[upstream documentation and credits](docs/UPSTREAM_README.md) for the original
project context, and [LICENSE](LICENSE) for the SDK's BSD 3-Clause terms.
Dependency licenses are included with the binary package.
