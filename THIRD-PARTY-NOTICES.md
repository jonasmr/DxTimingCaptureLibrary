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

## DirectX ETW manifest headers (`third_party/etw-manifest/`)

The library decodes events from the Microsoft-Windows-D3D12 and DirectX
graphics-kernel (DXGK / LDDMCore) ETW providers, identified by manifest headers:

- `D3D12Events.h` — a verbatim copy of the mc.exe-generated
  Microsoft-Windows-D3D12 manifest header. Copyright (c) Microsoft Corporation.
- `lddmcoreeventdefs.h` — the DXGK provider GUID, keyword flags, and event-id
  constants the library uses, taken from the DXGK ETW manifest `LDDMCore.man`
  that ships with GPUView (see its `README-PIX`).

For an in-tree PIX build these are not on the include path; the real Windows/PIX
headers are used instead.

## Dear ImGui (`third_party/imgui/`)

The `sample.memorymap` GUI is built with Dear ImGui, which is included under
`third_party/imgui/` (see its `README`):

- The core library (`imgui.cpp`, `imgui_draw.cpp`, `imgui_tables.cpp`,
  `imgui_widgets.cpp`) plus the Win32 and DirectX 11 backends under `backends/`.

- Project: Dear ImGui — https://github.com/ocornut/imgui (v1.91.8)
- License: MIT

## Other Windows headers consumed at build time

The library also compiles against `d3d12.h` (DirectX Agility SDK), `d3d12umddi.h`
(WDK), and `d3dkmthk.h`, supplied by the Windows SDK / WDK build toolchain and not
redistributed here.

> NOTE: In-tree these remaining headers are reached through the surrounding PIX
> source tree (`<external/wdk/...>`, `<external/d3d/...>`). Repointing them at the
> public Windows SDK/WDK and DirectX-Headers (or vendoring copies with confirmed
> provenance) is still tracked and should be resolved before a public release.
