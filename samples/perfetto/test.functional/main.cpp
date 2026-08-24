// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"

#include <external/googletest/VisualStudioWorkarounds.h>

#include <chrono>

#include "GpuWorkload.h"
#include "SampleProcess.h"

int __cdecl main(int argc, char** argv)
{
    // Relaunched by the tests to produce GPU work from a second process.
    bool submitGpuWork = false;
    unsigned int submitForSeconds = 0;
    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], DxEtwLibraryTest::SubmitGpuWorkSwitch) == 0)
        {
            submitGpuWork = true;
        }
        else if (strcmp(argv[i], DxEtwLibraryTest::SubmitGpuWorkSecondsSwitch) == 0 && i + 1 < argc)
        {
            submitForSeconds = static_cast<unsigned int>(strtoul(argv[++i], nullptr, 10));
        }
    }

    if (submitGpuWork)
    {
        try
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(submitForSeconds);
            do
            {
                DxEtwLibraryTest::SubmitGpuWorkAndWait();
                Sleep(16);
            } while (std::chrono::steady_clock::now() < deadline);
        }
        catch (...)
        {
            return DxEtwLibraryTest::SubmitGpuWorkFailedExitCode;
        }
        return 0;
    }

    testing::InitGoogleTest(&argc, argv);
    if (IsDebuggerPresent())
    {
        testing::GTEST_FLAG(catch_exceptions) = false;
    }

    return RUN_ALL_TESTS();
}
