# Third-Party Notices

DxEtwLibrary is licensed under the MIT License (see `LICENSE`). It incorporates
or depends on the following third-party material.

## DecodedPixEventTypes.h (WinPixEventRuntime)

`include/DxEtwLibrary/Types.h` includes `<DecodedPixEventTypes.h>` for the
`PixEventType` and `PixCpuEvent` definitions. This header ships with the
WinPixEventRuntime and is distributed under the MIT License.

- Project: WinPixEventRuntime — https://github.com/microsoft/PixEvents
- License: MIT

## Windows headers consumed at build time

The library parses ETW payloads defined by Windows and therefore compiles
against several Windows headers (for example `d3d12umddi.h`,
`lddmcoreeventdefs.h`, `D3D12Events.h`, and `d3dkmthk.h`) supplied by the
Windows SDK and the Windows Driver Kit (WDK). These are part of the build
toolchain, not redistributed by this repository.

> NOTE: In-tree, some of these headers are currently reached through the
> surrounding PIX source tree (`<external/wdk/...>`, `<external/d3d/...>`).
> Replacing those paths with the public Windows SDK/WDK headers (or vendoring
> redistributable copies with confirmed provenance) is tracked as part of making
> the library buildable standalone, and must be resolved before a public
> source release.
