// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include <exception>
#include <thread>

// Include ApiObjectCallbacks's full definition so `std::unique_ptr<ApiObjectCallbacks>` works
// under clang-cl (clang's STL eagerly instantiates the deleter, which requires a complete type).
#include <DxEtwLibrary/EtwCallbacks.h>
#include <DxEtwLibrary/EtwDiagnostics.h>

class DxEtwEventHandler;
struct _EVENT_RECORD;

// Drives a real-time ETW session end to end for the functional tests: it starts a
// trace, enables the D3D12 and DXGK providers, pumps event records into a
// DxEtwEventHandler on a consumer thread, and (on End) flushes deferred entries and
// records ETW data-loss totals. State is per-instance (no globals), so each test owns
// an isolated session.
class SimpleEtwSession
{
    std::thread m_thread;
    bool m_running = false;

    std::wstring m_sessionName;
    unsigned long long m_sessionHandle = 0; // TRACEHANDLE, kept opaque in the header
    std::unique_ptr<DxEtwEventHandler> m_handler;

    std::unique_ptr<ApiObjectCallbacks> m_apiObjectCallbacks;
    std::unique_ptr<RuntimeFailureCallbacks> m_runtimeFailureCallbacks;

    // Captures the first exception thrown out of HandleEventRecord on the consumer
    // thread (for example when a callback returns a failure HRESULT, which the
    // library turns into a throw). A test observes it via RethrowConsumerException.
    std::exception_ptr m_consumerException;

    // ETW data-loss totals for the session, populated from the trace's logfile
    // header after ProcessTrace returns.
    DirectX::Etw::LostEventStatistics m_lostEventStatistics{};

    void ConsumerThread();

public:
    // Routes a single ETW record to the handler, capturing the first exception that
    // escapes (see m_consumerException). Public only so the C-style ETW callback can
    // reach it; not intended to be called directly by tests.
    void OnEventRecord(_EVENT_RECORD* record);

    ~SimpleEtwSession()
    {
        if (m_running)
        {
            End();
        }
    }

    // Supplies the ApiObjectCallbacks the handler will drive. Optional: if not set,
    // the session installs a no-op implementation.
    void SetApiObjectCallbacks(std::unique_ptr<ApiObjectCallbacks> callbacks)
    {
        m_apiObjectCallbacks = std::move(callbacks);
    }

    // Supplies the RuntimeFailureCallbacks the handler will drive. Optional: if not
    // set, the handler receives null and journal-entry events are ignored.
    void SetRuntimeFailureCallbacks(std::unique_ptr<RuntimeFailureCallbacks> callbacks)
    {
        m_runtimeFailureCallbacks = std::move(callbacks);
    }

    void Begin();

    // Stops the trace, joins the consumer thread, flushes any deferred entries
    // (OnDataComplete) and records the final data-loss totals. After End returns the
    // consumer thread has joined, so every callback has run and the recorded data is
    // safe to read without further synchronization.
    void End();

    // ETW data-loss totals last seen for this session. Zero under normal load.
    DirectX::Etw::LostEventStatistics GetLostEventStatistics() const
    {
        return m_lostEventStatistics;
    }

    // Rethrows the first exception (if any) that propagated out of the handler on
    // the consumer thread, so a test can assert on the library's failure behavior.
    // No-op if the consumer thread completed cleanly.
    void RethrowConsumerException() const
    {
        if (m_consumerException)
        {
            std::rethrow_exception(m_consumerException);
        }
    }

    bool HadConsumerException() const
    {
        return static_cast<bool>(m_consumerException);
    }
};
