# Third-Party Notices

DxTimingCaptureLibrary is licensed under the MIT License (see `LICENSE`). It incorporates
or depends on the following third-party material.

## WinPixEventRuntime decoder headers

Vendored under `third_party/PixEventDecoder/include/` (see its `README-PIX`):

- `DecodedPixEventTypes.h` — `PixEventType` / `PixCpuEvent` and related decoded
  types, used by `include/DxTimingCaptureLibrary/Types.h` and `EtwCallbacks.h` (part of the
  public API).
- `PixEventDecoder.h` — declares `PixEventDecoder::DecodeTimingBlock`.

Only the headers are vendored. The decoder implementation is **not** linked;
`DecodeTimingBlock` is stubbed in `lib/PixEventDecoderStub.cpp`.

- Project: WinPixEventRuntime — https://github.com/microsoft/pixevents
- License: MIT

## DXGK ETW manifest header (`third_party/etw-manifest/`)

The library decodes events from the DirectX graphics-kernel (DXGK / LDDMCore) ETW
provider, identified by a manifest header:

- `lddmcoreeventdefs.h` — the DXGK provider GUID, keyword flags, and event-id
  constants the library uses, taken from the DXGK ETW manifest `LDDMCore.man`
  that ships with GPUView (see its `README-PIX`).

For an in-tree PIX build this is not on the include path; the real Windows/PIX
header is used instead.

## DirectStorage ETW header (`third_party/directstorage/`)

- `dstorage_etw.h` — a verbatim copy of the DirectStorage ETW event definitions.
  Copyright (c) Microsoft Corporation. It is not published in the
  `Microsoft.Direct3D.DirectStorage` NuGet package, which carries only
  `dstorage.h` and `dstorageerr.h`, so it is vendored here.

For an in-tree PIX build this is not on the include path; the real header is used
instead.

## Dear ImGui (`third_party/imgui/`)

The `sample.memorymap` GUI is built with Dear ImGui, which is included under
`third_party/imgui/` (see its `README`):

- The core library (`imgui.cpp`, `imgui_draw.cpp`, `imgui_tables.cpp`,
  `imgui_widgets.cpp`) plus the Win32 and DirectX 11 backends under `backends/`.

- Project: Dear ImGui — https://github.com/ocornut/imgui (v1.91.8)
- License: MIT

## Other Windows headers consumed at build time

The library also compiles against the DirectX Agility SDK headers (`d3d12.h`,
`D3D12Events.h` and friends), restored from the `Microsoft.Direct3D.D3D12` NuGet
package, and against `d3dkmthk.h` and other headers from the Windows SDK. Neither
set is redistributed here.
