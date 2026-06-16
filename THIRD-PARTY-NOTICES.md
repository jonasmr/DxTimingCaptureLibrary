# Third-Party Notices

DxEtwLibrary is licensed under the MIT License (see `LICENSE`). It incorporates
or depends on the following third-party material.

## WinPixEventRuntime decoder headers

Vendored under `third_party/PixEventDecoder/include/` (see its `README-PIX`):

- `DecodedPixEventTypes.h` — `PixEventType` / `PixCpuEvent` and related decoded
  types, used by `include/DxEtwLibrary/Types.h` and `EtwCallbacks.h` (part of the
  public API).
- `PixEventDecoder.h` — declares `PixEventDecoder::DecodeTimingBlock`.

Only the headers are vendored. The decoder implementation is **not** linked;
`DecodeTimingBlock` is stubbed in `lib/PixEventDecoderStub.cpp`.

- Project: WinPixEventRuntime — https://github.com/microsoft/pixevents
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
