// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

// Error-handling helpers for DxEtwLibrary.
//
// The library reports unrecoverable parsing/state errors by throwing. It exposes
// the throwing helpers as Errors::ThrowToolException / ThrowFailure / ThrowIf.
//
// There are two implementations, selected by the UsePixUtility build setting:
//
//  * When UsePixUtility is defined (set by in-tree PIX builds, see
//    Directory.Build.props), the helpers defer to PIX's own WinPix::ToolException
//    machinery (callstack capture + journal logging) so the library integrates
//    with PIX's diagnostics, exactly as it did before it was made
//    open-source-ready.
//
//  * Otherwise (a standalone build of the library), the helpers throw a small
//    self-contained EtwException that derives from std::exception, so a consuming
//    application can catch it generically (catch (const std::exception&)) with no
//    dependency on PIX.
//
// The two branches expose the same Errors::* signatures, so nothing else in the
// library needs to know which one is active.

#ifdef UsePixUtility

#include <PixUtility/include/PixUtility/Exception.h>

namespace DirectX::Etw::Errors
{
    using ::WinPix::ThrowToolException;
    using ::WinPix::ThrowFailure;
    using ::WinPix::ThrowIf;
}

#else

#include <exception>
#include <string>

#include <winerror.h> // HRESULT, FAILED, E_FAIL

namespace DirectX::Etw
{
    // Carries an HRESULT plus an optional human-readable message.
    class EtwException : public std::exception
    {
    public:
        explicit EtwException(HRESULT errorCode, const wchar_t* errorMessage = nullptr)
            : m_errorCode(errorCode)
        {
            if (errorMessage != nullptr)
            {
                m_errorMessage = errorMessage;
            }
        }

        HRESULT GetErrorCode() const noexcept
        {
            return m_errorCode;
        }

        const wchar_t* GetErrorMessage() const noexcept
        {
            return m_errorMessage.c_str();
        }

        const char* what() const noexcept override
        {
            return "DirectX::Etw::EtwException";
        }

    private:
        HRESULT m_errorCode;
        std::wstring m_errorMessage;
    };

    // The throwing helpers live in a nested namespace so that a consumer that
    // opens the library namespace with `using namespace DirectX::Etw` does not
    // pull these names into scope, where they would collide with a host
    // application's own ThrowToolException/ThrowFailure/ThrowIf (for example
    // PIX's WinPix:: helpers of the same name). Public headers reference them
    // as Errors::ThrowToolException(...); the library and its tests bring them
    // into scope explicitly via using-declarations in their precompiled headers.
    namespace Errors
    {
        [[noreturn]] inline void ThrowToolException(HRESULT errorCode, const wchar_t* errorMessage = nullptr)
        {
            throw EtwException(errorCode, errorMessage);
        }

        inline void ThrowFailure(HRESULT errorCode, const wchar_t* errorMessage = nullptr)
        {
            if (FAILED(errorCode))
            {
                ThrowToolException(errorCode, errorMessage);
            }
        }

        inline void ThrowIf(bool condition, HRESULT errorCode = E_FAIL, const wchar_t* errorMessage = nullptr)
        {
            if (condition)
            {
                ThrowToolException(errorCode, errorMessage);
            }
        }
    }
}

#endif
