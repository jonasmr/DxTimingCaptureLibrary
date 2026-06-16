// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

// Umbrella header for DxEtwLibrary.
//
// Include this single header to use the library. It pulls in the public data
// types, the consumer callback interfaces, the diagnostics seam, and the
// DxEtwEventHandler entry point. See DxEtwEventHandler.h for the construction
// contract (all callback interfaces and the timestamp converter are required).

#include <DxEtwLibrary/Types.h>
#include <DxEtwLibrary/EtwCallbacks.h>
#include <DxEtwLibrary/EtwDiagnostics.h>
#include <DxEtwLibrary/DxEtwEventHandler.h>
