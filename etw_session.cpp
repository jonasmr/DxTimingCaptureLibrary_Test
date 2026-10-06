#include "etw_session.h"

#include <cstddef>
#include <cstdio>

#include <DxTimingCaptureLibrary/EtwProviders.h>

namespace
{

const wchar_t kSessionName[] = L"dxtcl_monitor";

// EVENT_TRACE_PROPERTIES followed by the space ETW needs for the session name.
struct SessionProperties
{
    EVENT_TRACE_PROPERTIES properties;
    wchar_t name[256];

    SessionProperties()
    {
        ZeroMemory(this, sizeof(*this));
        properties.Wnode.BufferSize = sizeof(*this);
        properties.Wnode.Flags = WNODE_FLAG_TRACED_GUID;
        properties.Wnode.ClientContext = 1; // QPC timestamps (required by the library)
        properties.LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
        properties.BufferSize = 1024;       // KB; buffer settings as in the library's memorymap sample
        properties.MinimumBuffers = 64;
        properties.MaximumBuffers = 1290;
        properties.FlushTimer = 1;          // deliver events to us at least once per second
        properties.LoggerNameOffset = offsetof(SessionProperties, name);
    }
};

bool EnableProvider(TRACEHANDLE session, const GUID& provider, ULONGLONG keywords, UCHAR level, const char* name)
{
    ENABLE_TRACE_PARAMETERS parameters = { ENABLE_TRACE_PARAMETERS_VERSION_2 };
    ULONG status = EnableTraceEx2(session, &provider, EVENT_CONTROL_CODE_ENABLE_PROVIDER, level, keywords, 0, 5000, &parameters);
    if (status != ERROR_SUCCESS)
    {
        if (status == ERROR_TIMEOUT)
            printf("EnableTraceEx2(%s) timed out (%lu): a process using that provider did not respond in time.\n"
                   "Retry, or close other D3D12 apps (browsers, streaming tools) and retry.\n", name, status);
        else
            printf("EnableTraceEx2(%s) failed: %lu\n", name, status);
        return false;
    }

    // Rundown: ask the provider to log the state that already exists (adapters, segments, ...).
    status = EnableTraceEx2(session, &provider, EVENT_CONTROL_CODE_CAPTURE_STATE, TRACE_LEVEL_VERBOSE, 0, 0, 10000, nullptr);
    if (status != ERROR_SUCCESS)
        printf("warning: rundown request for %s failed: %lu\n", name, status);
    return true;
}

} // namespace

bool EtwSession::Start(DirectX::Etw::DxTimingCaptureEventHandler* handler)
{
    m_handler = handler;

    // Stop a session with our name that a previous run (killed before it could stop it) left behind.
    SessionProperties stale;
    ControlTraceW(0, kSessionName, &stale.properties, EVENT_TRACE_CONTROL_STOP);

    SessionProperties properties;
    ULONG status = StartTraceW(&m_session, kSessionName, &properties.properties);
    if (status != ERROR_SUCCESS)
    {
        m_session = 0;
        if (status == ERROR_ACCESS_DENIED)
            printf("StartTrace: access denied. Run as administrator, or add your account to the 'Performance Log Users'\n"
                   "group (then sign out and in again).\n");
        else
            printf("StartTrace failed: %lu\n", status);
        return false;
    }

    const ULONGLONG dxgkKeywords = DXGK_KEYWORD_LOG_FLAGS_BASE | DXGK_KEYWORD_LOG_FLAGS_RESOURCE |
                                   DXGK_KEYWORD_LOG_FLAGS_ALLOCATIONS_REFERENCES | DXGK_KEYWORD_LOG_FLAGS_LONG_HAUL;
    const ULONGLONG d3d12Keywords = D3D12_ETW_LOG_FLAGS_NAMES | D3D12_ETW_LOG_FLAGS_DEVICES | D3D12_ETW_LOG_FLAGS_OBJECT_LIFETIME |
                                    D3D12_ETW_LOG_FLAGS_RESOURCES | D3D12_ETW_LOG_APIS;
    if (!EnableProvider(m_session, DxgkControlGuid, dxgkKeywords, TRACE_LEVEL_VERBOSE, "DxgKrnl") ||
        !EnableProvider(m_session, Direct3D12EtwProviderGuid, d3d12Keywords, TRACE_LEVEL_RESERVED6, "D3D12"))
    {
        Stop();
        return false;
    }

    m_logfile.LoggerName = const_cast<LPWSTR>(kSessionName);
    m_logfile.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP;
    m_logfile.EventRecordCallback = OnEvent;
    m_logfile.BufferCallback = OnBuffer;
    m_logfile.Context = this;
    m_trace = OpenTraceW(&m_logfile);
    if (m_trace == INVALID_PROCESSTRACE_HANDLE)
    {
        printf("OpenTrace failed: %lu\n", GetLastError());
        Stop();
        return false;
    }

    m_consumer = std::thread([this] {
        const ULONG result = ProcessTrace(&m_trace, 1, nullptr, nullptr); // returns once the session is stopped
        if (result != ERROR_SUCCESS)
            printf("ProcessTrace returned %lu\n", result);
        try
        {
            m_handler->OnDataComplete();
        }
        catch (...)
        {
            ++m_handlerExceptions;
        }
    });
    return true;
}

void EtwSession::Stop()
{
    if (m_session == 0)
        return;

    SessionProperties properties; // ControlTrace fills in the final statistics
    ControlTraceW(m_session, nullptr, &properties.properties, EVENT_TRACE_CONTROL_STOP);
    m_session = 0;

    if (m_consumer.joinable())
        m_consumer.join();
    if (m_trace != INVALID_PROCESSTRACE_HANDLE)
        CloseTrace(m_trace);
    m_trace = INVALID_PROCESSTRACE_HANDLE;

    printf("ETW session: %llu events received, %lu events lost, %lu real-time buffers lost, %llu exceptions from the library\n",
        m_events.load(), properties.properties.EventsLost, properties.properties.RealTimeBuffersLost, m_handlerExceptions.load());
}

ULONG EtwSession::EventsLost() const
{
    SessionProperties properties;
    if (m_session == 0 || ControlTraceW(m_session, nullptr, &properties.properties, EVENT_TRACE_CONTROL_QUERY) != ERROR_SUCCESS)
        return 0;
    return properties.properties.EventsLost;
}

void WINAPI EtwSession::OnEvent(EVENT_RECORD* record)
{
    EtwSession* self = static_cast<EtwSession*>(record->UserContext);
    ++self->m_events;
    try
    {
        self->m_handler->HandleEventRecord(record);
    }
    catch (...) // an exception must never unwind into ETW
    {
        ++self->m_handlerExceptions;
    }
}

ULONG WINAPI EtwSession::OnBuffer(EVENT_TRACE_LOGFILEW* logfile)
{
    EtwSession* self = static_cast<EtwSession*>(logfile->Context);
    try
    {
        self->m_handler->ReportTraceStatistics(*logfile); // reports lost events to the DiagnosticsSink
    }
    catch (...)
    {
        ++self->m_handlerExceptions;
    }
    return TRUE; // keep processing
}
