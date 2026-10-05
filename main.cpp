// dxtcl_monitor: prints what DxTimingCaptureLibrary reports about where a process's D3D12 objects live.
//
//   dxtcl_monitor.exe <exe name>
//   dxtcl_monitor.exe --pid <pid>
//
// Waits until a process with that exe name runs (or takes the given pid), starts a real-time ETW
// session that feeds the library (etw_session.cpp), and once per second prints what the library's
// callbacks have reported so far: for the objects named RT_<index>, whether the library
// has them in video memory (Local), in system memory (NonLocal) or doesn't know (Unknown), and
// whether OnDemotedAllocations has named them. The format matches the vramtiming test app, which
// measures the same thing with GPU timing, so the two outputs can be compared line by line.
// Runs until the target exits (or Ctrl+C), then prints a final report.

#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include <set>
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

struct TrackedObject // a resource or a heap
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
                              public ResidencyEventCallbacks,
                              public DiagnosticsSink
{
public:
    explicit LibraryListener(DWORD targetPid) : m_targetPid(targetPid) {}

    std::mutex mutex;
    std::unordered_map<UINT64, TrackedObject> objects; // live resources and heaps, by object id
    std::map<std::wstring, double> counters;           // latest value of each memory counter of the target, in MB (10^6 bytes)
    UINT64 residencyOps[4] = {};                       // indexed by ResidencyOperationType
    UINT64 segmentGroupChanges = 0;
    UINT64 migrations = 0;

    // ---- ApiObjectCallbacks: resources and heaps ----

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

    HRESULT OnReservedResourceCreation(INT64 time, UINT64 device, UINT32 pid, UINT32 tid, const D3D12_RESOURCE_DESC* desc,
        const ReservedResourceInfo* reserved, UINT64* objectId) override
    {
        NoOpApiObjectCallbacks::OnReservedResourceCreation(time, device, pid, tid, desc, reserved, objectId);
        Add(*objectId, nullptr); // no memory of its own (tiles are mapped from heaps)
        return S_OK;
    }

    HRESULT OnHeapCreation(INT64 time, UINT64 device, UINT32 pid, UINT32 tid, const D3D12_HEAP_DESC* desc,
        const ObjectPlacementInfo* placement, UINT64* objectId) override
    {
        NoOpApiObjectCallbacks::OnHeapCreation(time, device, pid, tid, desc, placement, objectId);
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
        const size_t type = static_cast<size_t>(operation->OperationType);
        if (type < 4)
            ++residencyOps[type];
        return S_OK;
    }

    HRESULT OnAllocationMigrations(const AllocationMigration*, UINT32 count) override
    {
        std::lock_guard lock(mutex);
        migrations += count;
        return S_OK;
    }

    // ---- PixCounterCallbacks: the library reports per-process memory counters ("Local Budget",
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
        std::lock_guard lock(mutex);
        printf("  library diagnostic (%s, code %d): %.*ls\n", severity == DiagnosticSeverity::Error ? "error" : "warning",
            static_cast<int>(code), static_cast<int>(message.size()), message.data());
    }

private:
    void Add(UINT64 objectId, const ObjectPlacementInfo* placement)
    {
        std::lock_guard lock(mutex);
        TrackedObject& object = objects[objectId];
        if (placement)
        {
            object.bytes = placement->GpuVirtualSize;
            object.group = placement->ResidentSegmentGroup; // often Unknown at creation
        }
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

enum Location { Vram, SystemMemory, UnknownLocation, LocationCount };

Location ToLocation(MemorySegmentGroup group)
{
    switch (group)
    {
    case MemorySegmentGroup::Local:    return Vram;
    case MemorySegmentGroup::NonLocal: return SystemMemory;
    default:                           return UnknownLocation;
    }
}

// Object counts and sizes per location, plus the objects flagged as demoted.
struct Totals
{
    UINT64 count[LocationCount] = {};
    UINT64 bytes[LocationCount] = {};
    UINT64 demotedCount = 0;
    UINT64 demotedBytes = 0;

    void Add(const TrackedObject& object)
    {
        const Location location = ToLocation(object.group);
        ++count[location];
        bytes[location] += object.bytes;
        if (object.demoted)
        {
            ++demotedCount;
            demotedBytes += object.bytes;
        }
    }
};

UINT64 MiB(UINT64 bytes) { return bytes >> 20; } // "MB" in the output means MiB, as in vramtiming

// "RT_123" -> 123. False for any other name.
bool ParseRtIndex(const std::wstring& name, UINT32* index)
{
    if (name.compare(0, 3, L"RT_") != 0 || name.size() == 3)
        return false;
    wchar_t* end = nullptr;
    *index = wcstoul(name.c_str() + 3, &end, 10);
    return *end == L'\0';
}

// {0,1,2,3,7,9,10} -> "0-3, 7, 9-10" (the same format vramtiming uses). "none" if empty.
std::string FormatRanges(const std::set<UINT32>& indices)
{
    std::string text;
    for (auto it = indices.begin(); it != indices.end();)
    {
        const UINT32 first = *it;
        UINT32 last = first;
        while (++it != indices.end() && *it == last + 1)
            last = *it;
        text += text.empty() ? "" : ", ";
        text += last == first ? std::to_string(first) : std::to_string(first) + "-" + std::to_string(last);
    }
    return text.empty() ? "none" : text;
}

// Sum of the named counters in MiB (the library reports MB = 10^6 bytes), or "n/a" if none was reported yet.
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
    return seen ? std::to_string(static_cast<UINT64>(sum * 1e6 / (1024.0 * 1024.0) + 0.5)) : "n/a";
}

// The range lines printed last time; a range line is only printed again when it changes.
struct PrintedRanges
{
    std::string systemMemory, demoted, unknown;
};

void PrintIfChanged(const char* label, const std::set<UINT32>& indices, std::string& previous)
{
    std::string text = FormatRanges(indices);
    if (text != previous)
        printf("  library %s: %s\n", label, text.c_str());
    previous = text;
}

void PrintReport(LibraryListener& library, const std::string& when, PrintedRanges& printed)
{
    std::lock_guard lock(library.mutex);

    Totals rt, all;
    std::set<UINT32> rtInSystemMemory, rtDemoted, rtUnknown;
    for (const auto& [id, object] : library.objects)
    {
        all.Add(object);

        UINT32 index = 0;
        if (!ParseRtIndex(object.name, &index))
            continue;
        rt.Add(object);
        if (ToLocation(object.group) == SystemMemory)
            rtInSystemMemory.insert(index);
        if (ToLocation(object.group) == UnknownLocation)
            rtUnknown.insert(index);
        if (object.demoted)
            rtDemoted.insert(index);
    }

    printf("%s  RT_* objects (library): VRAM %llu MB [%llu] | system memory %llu MB [%llu] | unknown %llu MB [%llu] | flagged demoted %llu MB [%llu]\n",
        when.c_str(), MiB(rt.bytes[Vram]), rt.count[Vram], MiB(rt.bytes[SystemMemory]), rt.count[SystemMemory],
        MiB(rt.bytes[UnknownLocation]), rt.count[UnknownLocation], MiB(rt.demotedBytes), rt.demotedCount);

    PrintIfChanged("system memory", rtInSystemMemory, printed.systemMemory);
    PrintIfChanged("demoted", rtDemoted, printed.demoted);
    PrintIfChanged("unknown", rtUnknown, printed.unknown);

    const auto& counters = library.counters;
    printf("  library counters (MB): Local Budget %s | Local Resident %s | Non-Local Resident %s | demoted (Minimum..Maximum Priority) %s\n",
        CounterMiB(counters, { L"Local Budget" }).c_str(), CounterMiB(counters, { L"Local Resident" }).c_str(),
        CounterMiB(counters, { L"Non-Local Resident" }).c_str(),
        CounterMiB(counters, { L"Minimum Priority", L"Low Priority", L"Normal Priority", L"High Priority", L"Maximum Priority" }).c_str());

    // Note: a placed resource's bytes are also part of its heap's bytes.
    const UINT64* ops = library.residencyOps;
    printf("  all resources+heaps (library): VRAM %llu MB [%llu] | system memory %llu MB [%llu] | unknown %llu MB [%llu] | "
           "MakeResident %llu, Evict %llu, PageIn %llu, PageOut %llu, segment-group changes %llu, migrations %llu\n",
        MiB(all.bytes[Vram]), all.count[Vram], MiB(all.bytes[SystemMemory]), all.count[SystemMemory], MiB(all.bytes[UnknownLocation]),
        all.count[UnknownLocation], ops[0], ops[1], ops[2], ops[3], library.segmentGroupChanges, library.migrations);
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
    options.TrackGpuTiming = true; // the library only reports OnAllocationMigrations when this is set
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

    PrintedRanges printed;
    const ULONGLONG start = GetTickCount64();
    while (!g_ctrlC && WaitForSingleObject(target, 1000) == WAIT_TIMEOUT)
    {
        PrintReport(library, "t=" + std::to_string((GetTickCount64() - start + 500) / 1000) + "s", printed);
        if (const ULONG lost = session.EventsLost())
            printf("  WARNING: the ETW session has lost %lu events so far, the library's data is incomplete\n", lost);
    }

    printf("\ndxtcl_monitor: %s, stopping the ETW session\n", g_ctrlC ? "Ctrl+C" : "target exited");
    session.Stop(); // delivers the remaining events, then calls OnDataComplete

    printed = {}; // print all range lines in the final report
    PrintReport(library, "final", printed);

    CloseHandle(target);
    return 0;
}
