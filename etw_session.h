#pragma once

#include <atomic>
#include <thread>

#include <DxTimingCaptureLibrary/DxTimingCaptureEventHandler.h>

// A real-time ETW session with the providers DxTimingCaptureLibrary needs (set up as the library
// README and its memorymap sample describe). Every event is passed to the handler on the session's
// own ProcessTrace thread, so the library's callbacks run on that thread.
class EtwSession
{
public:
    ~EtwSession() { Stop(); }

    // Starts the session, enables the D3D12 and DxgKrnl providers, requests their rundown and starts
    // delivering events to `handler`. Prints the reason and returns false on failure.
    bool Start(DirectX::Etw::DxTimingCaptureEventHandler* handler);

    // Stops the session. Waits until the remaining events have been delivered, then calls
    // handler->OnDataComplete() (on the ProcessTrace thread) and prints the session statistics.
    void Stop();

    // Number of events the session has dropped so far (0 if it is not running).
    ULONG EventsLost() const;

private:
    static void WINAPI OnEvent(EVENT_RECORD* record);
    static ULONG WINAPI OnBuffer(EVENT_TRACE_LOGFILEW* logfile);

    DirectX::Etw::DxTimingCaptureEventHandler* m_handler = nullptr;
    TRACEHANDLE m_session = 0;
    TRACEHANDLE m_trace = INVALID_PROCESSTRACE_HANDLE;
    EVENT_TRACE_LOGFILEW m_logfile = {};
    std::thread m_consumer;
    std::atomic<UINT64> m_events{ 0 };
    std::atomic<UINT64> m_handlerExceptions{ 0 };
};
