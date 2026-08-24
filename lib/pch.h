// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include <algorithm>
#include <assert.h>
#include <codecvt>
#include <format>
#include <functional>
#include <locale>
#include <map>
#include <queue>
#include <set>
#include <stdint.h>
#include <sstream>
#include <string>
#include <vector>

#include <external/d3d/d3d12.h>
#include <external/d3d/D3D12Events.h>

#include <Windows.h>
#include <initguid.h>
#include <evntcons.h>

#include <wrl\client.h>
#include <wrl\implements.h>
#include <wrl\wrappers\corewrappers.h>
using Microsoft::WRL::ComPtr;

#include <DxEtwLibrary/EtwDiagnostics.h>
#include <DxEtwLibrary/EtwException.h>

using DirectX::Etw::Errors::ThrowToolException;
using DirectX::Etw::Errors::ThrowFailure;
using DirectX::Etw::Errors::ThrowIf;

#include <DxEtwLibrary/EtwDispatcher.h>

using namespace DirectX::Etw;
