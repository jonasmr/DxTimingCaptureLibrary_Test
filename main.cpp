// dxtcl_monitor: prints what DxTimingCaptureLibrary reports about where a process's D3D12 objects live.
//
//   dxtcl_monitor.exe <exe name>
//   dxtcl_monitor.exe --pid <pid>
//
// Waits until a process with that exe name runs (or takes the given pid), starts a real-time ETW
// session that feeds the library (etw_session.cpp), and once per second prints one line of what the
// library's callbacks have reported so far: for the objects named RT_<index>, whether the library
// has them in video memory (Local), in system memory (NonLocal) or doesn't know (Unknown), and
// whether OnDemotedAllocations has named them, plus a few counters (see README.md). vramtiming
// measures the same thing with GPU timing. Runs until the target exits (or Ctrl+C), then prints a
// final report.

#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>

#include <DxTimingCaptureLibrary/DxTimingCaptureEventHandler.h>
#include <DxTimingCaptureLibrary/NoOpCallbacks.h>

#include "etw_session.h"

using namespace DirectX::Etw;

namespace
{

// ------------------------------------------------------------------------------------------------
// The library's callbacks, and what they have reported so far
// ------------------------------------------------------------------------------------------------

struct TrackedObject // a committed or placed resource
{
    std::wstring name;
    UINT64 bytes = 0;
    MemorySegmentGroup group = MemorySegmentGroup::Unknown;
    bool demoted = false; // named by OnDemotedAllocations at least once
};

// The callbacks run on the ETW (ProcessTrace) thread, the report is printed on the main thread:
// everything is guarded by `mutex`. Object and counter ids are handed out by the NoOp base classes
// (a counter each, unique and non-zero).
class LibraryListener final : public NoOpApiObjectCallbacks,
                              public NoOpPixCounterCallbacks,
                              public NoOpResidencyEventCallbacks,
                              public DiagnosticsSink
{
public:
    explicit LibraryListener(DWORD targetPid) : m_targetPid(targetPid) {}

    std::mutex mutex;
    std::unordered_map<UINT64, TrackedObject> objects; // live resources, by object id
    std::map<std::wstring, double> counters;           // latest value of each memory counter of the target, in MB (10^6 bytes)
    UINT64 pageIns = 0, pageOuts = 0;                  // PageIn / PageOut residency operations so far
    UINT64 segmentGroupChanges = 0;

    // ---- ApiObjectCallbacks: resources (vramtiming's RT_<index> objects are committed resources) ----

    HRESULT OnCommittedResourceCreation(INT64 time, UINT64 device, UINT32 pid, UINT32 tid, const D3D12_RESOURCE_DESC* desc,
        const ObjectPlacementInfo* placement, const D3D12_HEAP_PROPERTIES* heapProperties, D3D12_HEAP_FLAGS heapFlags,
        UINT64* objectId) override
    {
        NoOpApiObjectCallbacks::OnCommittedResourceCreation(time, device, pid, tid, desc, placement, heapProperties, heapFlags, objectId);
        Add(*objectId, placement);
        return S_OK;
    }

    HRESULT OnPlacedResourceCreation(INT64 time, UINT64 device, UINT32 pid, UINT32 tid, const D3D12_RESOURCE_DESC* desc,
        const ObjectPlacementInfo* placement, UINT64* objectId) override
    {
        NoOpApiObjectCallbacks::OnPlacedResourceCreation(time, device, pid, tid, desc, placement, objectId);
        Add(*objectId, placement);
        return S_OK;
    }

    HRESULT OnApiObjectName(INT64, ApiObjectType, UINT64 objectId, std::wstring_view name) override
    {
        std::lock_guard lock(mutex);
        if (TrackedObject* object = Find(objectId))
            object->name = name;
        return S_OK;
    }

    HRESULT OnObjectDestruction(INT64, ApiObjectType, UINT64 objectId) override
    {
        std::lock_guard lock(mutex);
        objects.erase(objectId);
        return S_OK;
    }

    // ---- ResidencyEventCallbacks ----

    HRESULT OnAllocationSegmentGroupChanges(const AllocationSegmentGroupChange* changes, UINT32 count) override
    {
        std::lock_guard lock(mutex);
        for (UINT32 i = 0; i < count; ++i)
        {
            ++segmentGroupChanges;
            if (TrackedObject* object = Find(changes[i].ObjectId))
                object->group = changes[i].SegmentGroup;
        }
        return S_OK;
    }

    HRESULT OnDemotedAllocations(const DemotedAllocation* demoted, UINT32 count) override
    {
        std::lock_guard lock(mutex);
        for (UINT32 i = 0; i < count; ++i)
        {
            if (TrackedObject* object = Find(demoted[i].ObjectId))
                object->demoted = true;
        }
        return S_OK;
    }

    HRESULT OnResidencyOperation(const ResidencyOperation* operation) override
    {
        std::lock_guard lock(mutex);
        pageIns += operation->OperationType == ResidencyOperationType::PageIn;
        pageOuts += operation->OperationType == ResidencyOperationType::PageOut;
        return S_OK;
    }

    // ---- PixCounterCallbacks: the library reports per-process memory counters ("Local Resident",
    //      "Non-Local Resident", "High Priority", ...) through these, one counter group per adapter. ----

    HRESULT OnPixCounterInfo(UINT64 groupId, UINT32 processId, PCWSTR name, PCWSTR description, PCWSTR units, CounterFlags flags,
        double min, double max, UINT64* counterId) override
    {
        NoOpPixCounterCallbacks::OnPixCounterInfo(groupId, processId, name, description, units, flags, min, max, counterId);
        std::lock_guard lock(mutex);
        if (processId == m_targetPid && name)
            m_counterNames[*counterId] = name;
        return S_OK;
    }

    HRESULT OnPixCounterData(const CounterDataPoint* dataPoints, UINT32 count) override
    {
        std::lock_guard lock(mutex);
        for (UINT32 i = 0; i < count; ++i)
        {
            auto name = m_counterNames.find(dataPoints[i].CounterId);
            if (name != m_counterNames.end())
                counters[name->second] = dataPoints[i].Value;
        }
        return S_OK;
    }

    // ---- DiagnosticsSink ----

    void OnDiagnostic(DiagnosticSeverity severity, DiagnosticCode code, std::wstring_view message) override
    {
        char line[161]; // clipped to 160 characters, like every other output line
        const int length = snprintf(line, sizeof(line), "  library diagnostic (%s, code %d): %.*ls",
            severity == DiagnosticSeverity::Error ? "error" : "warning", static_cast<int>(code), static_cast<int>(message.size()), message.data());
        if (length >= static_cast<int>(sizeof(line)))
            memcpy(line + sizeof(line) - 4, "...", 4);
        std::lock_guard lock(mutex);
        printf("%s\n", line);
    }

private:
    void Add(UINT64 objectId, const ObjectPlacementInfo* placement)
    {
        std::lock_guard lock(mutex);
        TrackedObject& object = objects[objectId];
        object.bytes = placement->GpuVirtualSize;
        object.group = placement->ResidentSegmentGroup; // often Unknown at creation
    }

    TrackedObject* Find(UINT64 objectId)
    {
        auto it = objects.find(objectId);
        return it == objects.end() ? nullptr : &it->second;
    }

    DWORD m_targetPid;
    std::unordered_map<UINT64, std::wstring> m_counterNames; // counters of the target process, by counter id
};

// ------------------------------------------------------------------------------------------------
// The report
// ------------------------------------------------------------------------------------------------

UINT64 MiB(UINT64 bytes) { return bytes >> 20; } // "MB" in the output means MiB, as in vramtiming

// True for "RT_<digits>" (vramtiming's render targets), false for any other name.
bool IsRtName(const std::wstring& name)
{
    return name.size() > 3 && name.compare(0, 3, L"RT_") == 0 && name.find_first_not_of(L"0123456789", 3) == std::wstring::npos;
}

// Sum of the named counters in MiB (the library reports MB = 10^6 bytes), or "-" if none was reported yet.
std::string CounterMiB(const std::map<std::wstring, double>& counters, std::initializer_list<const wchar_t*> names)
{
    double sum = 0;
    bool seen = false;
    for (const wchar_t* name : names)
    {
        auto it = counters.find(name);
        if (it != counters.end())
        {
            sum += it->second;
            seen = true;
        }
    }
    return seen ? std::to_string(static_cast<UINT64>(sum * 1e6 / (1024.0 * 1024.0) + 0.5)) : "-";
}

// The one report line.
void PrintReport(LibraryListener& library, const char* when)
{
    std::lock_guard lock(library.mutex);

    enum { Vram, Sys, Unk, Dem };
    UINT64 count[4] = {}, bytes[4] = {};
    for (const auto& [id, object] : library.objects)
    {
        if (!IsRtName(object.name))
            continue;
        const int location = object.group == MemorySegmentGroup::Local ? Vram : object.group == MemorySegmentGroup::NonLocal ? Sys : Unk;
        ++count[location];
        bytes[location] += object.bytes;
        if (object.demoted)
        {
            ++count[Dem];
            bytes[Dem] += object.bytes;
        }
    }

    const auto& counters = library.counters;
    printf("%s  RT vram %lluMB/%llu sys %lluMB/%llu unk %lluMB/%llu dem %lluMB/%llu | ctr vramRes %s sysRes %s dem %s MB | "
           "pgIn %llu pgOut %llu segChg %llu\n",
        when, MiB(bytes[Vram]), count[Vram], MiB(bytes[Sys]), count[Sys], MiB(bytes[Unk]), count[Unk], MiB(bytes[Dem]), count[Dem],
        CounterMiB(counters, { L"Local Resident" }).c_str(), CounterMiB(counters, { L"Non-Local Resident" }).c_str(),
        CounterMiB(counters, { L"Minimum Priority", L"Low Priority", L"Normal Priority", L"High Priority", L"Maximum Priority" }).c_str(),
        library.pageIns, library.pageOuts, library.segmentGroupChanges);
}

// ------------------------------------------------------------------------------------------------
// The target process
// ------------------------------------------------------------------------------------------------

// "C:\path\VramTiming.exe" -> "VramTiming": the file name without folder and without ".exe".
std::wstring ExeBaseName(const wchar_t* path)
{
    std::wstring name = path;
    name.erase(0, name.find_last_of(L"\\/") + 1);
    if (name.size() > 4 && _wcsicmp(name.c_str() + name.size() - 4, L".exe") == 0)
        name.resize(name.size() - 4);
    return name;
}

// The pid of a running process with the same exe name as `exe` (see ExeBaseName, case-insensitive), or 0 if there is none.
DWORD FindProcess(const wchar_t* exe)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;
    const std::wstring name = ExeBaseName(exe);
    DWORD pid = 0;
    PROCESSENTRY32W entry = { sizeof(entry) };
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok && pid == 0; ok = Process32NextW(snapshot, &entry))
    {
        if (_wcsicmp(ExeBaseName(entry.szExeFile).c_str(), name.c_str()) == 0)
            pid = entry.th32ProcessID;
    }
    CloseHandle(snapshot);
    return pid;
}

std::atomic<bool> g_ctrlC{ false };

BOOL WINAPI OnCtrlC(DWORD type)
{
    if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT)
        return FALSE;
    g_ctrlC = true;
    return TRUE;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    const bool byPid = argc == 3 && wcscmp(argv[1], L"--pid") == 0;
    if (argc != 2 && !byPid)
    {
        printf("Usage: dxtcl_monitor <exe name> | --pid <pid>       e.g. dxtcl_monitor vramtiming.exe\n"
               "  Waits for the process and prints once per second where DxTimingCaptureLibrary says its D3D12 objects are.\n");
        return 1;
    }
    setvbuf(stdout, nullptr, _IONBF, 0); // every line appears immediately, also when redirected to a file

    const wchar_t* name = byPid ? L"process" : argv[1];
    DWORD pid = byPid ? wcstoul(argv[2], nullptr, 10) : FindProcess(argv[1]);
    if (pid != 0)
        printf("dxtcl_monitor: note: attaching to a running process, objects it created before that are only seen via the ETW rundown\n");
    else if (!byPid)
    {
        printf("dxtcl_monitor: waiting for %ls to start...\n", name);
        while ((pid = FindProcess(argv[1])) == 0)
            Sleep(5);
    }

    HANDLE target = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid); // to wait for its exit
    if (!target)
    {
        printf("cannot open pid %lu: error %lu\n", pid, GetLastError());
        return 1;
    }

    LibraryListener library(pid);
    DxTimingCaptureLibraryOptions options;
    options.TrackApiObjects = true;
    DxTimingCaptureEventCallbacks callbacks;
    callbacks.ApiObjectCallbacks = &library;
    callbacks.ResidencyEventCallbacks = &library;
    callbacks.PixCounterCallbacks = &library;
    callbacks.DiagnosticsSink = &library;
    auto handler = DxTimingCaptureEventHandler::Create(pid, options, callbacks);

    EtwSession session;
    if (!session.Start(handler.get()))
        return 1;

    SetConsoleCtrlHandler(OnCtrlC, TRUE);
    printf("dxtcl_monitor: attached to %ls (pid %lu), ETW session running\n", name, pid);

    ULONG printedLost = 0;
    const ULONGLONG start = GetTickCount64();
    while (!g_ctrlC && WaitForSingleObject(target, 1000) == WAIT_TIMEOUT)
    {
        PrintReport(library, ("t=" + std::to_string((GetTickCount64() - start + 500) / 1000) + "s").c_str());
        const ULONG lost = session.EventsLost();
        if (lost != printedLost) // printed when the count grows, not every second
            printf("  WARNING: the ETW session has lost %lu events so far, the library's data is incomplete\n", lost);
        printedLost = lost;
    }

    printf("\ndxtcl_monitor: %s, stopping the ETW session\n", g_ctrlC ? "Ctrl+C" : "target exited");
    session.Stop(); // delivers the remaining events, then calls OnDataComplete; prints the session statistics

    PrintReport(library, "final");

    CloseHandle(target);
    return 0;
}
