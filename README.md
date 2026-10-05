# DxTimingCaptureLibrary_Test

`dxtcl_monitor` is a small console program that shows, once per second, what
[DxTimingCaptureLibrary](https://github.com/microsoft/DxTimingCaptureLibrary) reports about **where each
D3D12 object's memory is**: video memory (`MemorySegmentGroup::Local`), system memory (`NonLocal`)
or unknown. It is meant to run against the `vramtiming` test app, which measures the same thing
independently with GPU timing and prints it as one line per second, too. Run both side by side to check the
library's per-object memory location and demotion data.

The library is used unmodified, as the git submodule `DxTimingCaptureLibrary/`.

## Files

| File | Content |
|---|---|
| `main.cpp` | the library callbacks, the per-second report, finding the target process |
| `etw_session.h/.cpp` | the real-time ETW session that feeds the library (providers, keywords, ProcessTrace thread) |
| `dxtcl_monitor.vcxproj` | builds `main.cpp`, `etw_session.cpp` and the library's `lib\*.cpp` into one exe |
| `Directory.Build.props` | puts all build output under `build\` |

## Build

Requirements: Visual Studio 2022 (v143, Desktop C++), Windows SDK 10.0.26100, and nuget.org access
(the library's `Directory.Build.props` pulls the `Microsoft.Direct3D.D3D12` package for `d3d12.h` and
`D3D12Events.h`).

```
git submodule update --init
msbuild dxtcl_monitor.sln /restore /p:Configuration=Release /p:Platform=x64
```

The exe is `build\bin\Release\dxtcl_monitor.exe`. Nothing is written into the submodule.

## Run

Starting the ETW session needs administrator rights or membership in the **Performance Log Users**
group.

In one console:

```
dxtcl_monitor.exe vramtiming.exe
```

then in another console:

```
vramtiming.exe 8192
```

`dxtcl_monitor` waits until a process named `vramtiming.exe` runs (the name is compared without
folder and case, `.exe` is optional), starts the ETW session for it and prints one line every second
until the target exits (close the vramtiming window) or Ctrl+C. vramtiming waits 2 seconds at startup,
so the session is up before it creates anything. Compare the two consoles: vramtiming's lines say
`allocated ... | VRAM (fast) ...`, the monitor's lines say `RT vram ...`.

`dxtcl_monitor.exe --pid <pid>` attaches to a running process directly. When the monitor attaches to
a process that is already running, it sees the objects created before that only through the ETW
rundown.

## What to look at

vramtiming (from GPU timing):

```
t=5s  allocated 8192 MB | VRAM (fast) 3664 MB [229 RTs] | system memory (slow) 4528 MB [283 RTs] | evicted (kernel) 4464 MB | frame 665.6 ms
```

dxtcl_monitor (from the library), one line per second:

```
t=5s  RT vram 3968MB/248 sys 2176MB/136 unk 0MB/0 dem 0MB/0 | ctr vramRes 3994 sysRes 2205 dem 2176 MB | pgIn 0 pgOut 0 segChg 0
```

| Field | Meaning |
|---|---|
| `t=5s` | seconds since the monitor attached; `final` for the report after the target exited |
| `RT vram / sys / unk <MB>MB/<n>` | live objects named `RT_<index>`, size and count, by the memory segment group the library last reported for them (`ObjectPlacementInfo::ResidentSegmentGroup` at creation, updated by `OnAllocationSegmentGroupChanges`): `Local`, `NonLocal`, `Unknown` |
| `dem <MB>MB/<n>` | `RT_<index>` objects named by `OnDemotedAllocations` at least once |
| `ctr vramRes / sysRes / dem` | the library's per-process memory counters (`PixCounterCallbacks`), latest values in MB: `Local Resident`, `Non-Local Resident`, and the sum of the five demoted-per-priority counters (`Minimum Priority` .. `Maximum Priority`); `-` until the library has reported one |
| `pgIn / pgOut` | `PageIn` / `PageOut` residency operations (`OnResidencyOperation`) reported so far |
| `segChg` | segment-group changes (`OnAllocationSegmentGroupChanges`) reported so far |
| `WARNING: the ETW session has lost N events` | printed only when ETW dropped (more) events; the library's data is then incomplete |

Library diagnostics (`DiagnosticsSink`) are printed as they arrive, clipped to 160 characters like every
other line. After the target exits, the monitor prints the ETW session statistics (events received
and lost), then the `final` line.

If the library is right, `RT vram` / `sys` match vramtiming's `VRAM (fast)` / `system memory (slow)`
and `unk` is 0. `dem` and the `ctr ... dem` counter
can be compared with vramtiming's `evicted (kernel)`.

"MB" means MiB everywhere, as in vramtiming. The library reports its counters in MB of 10^6 bytes;
the monitor converts them to MiB. The `t=` values of the two programs are not aligned: the
monitor counts from when it attached to the target, vramtiming from when it starts rendering.

## Library settings used

- `DxTimingCaptureLibraryOptions`: `TrackApiObjects = true`.
- Callbacks: `ApiObjectCallbacks`, `ResidencyEventCallbacks`, `PixCounterCallbacks`, `DiagnosticsSink`.
- ETW (`etw_session.cpp`): real-time session, `Wnode.ClientContext = 1` (QPC),
  `PROCESS_TRACE_MODE_RAW_TIMESTAMP`. DxgKrnl: `BASE | RESOURCE | ALLOCATIONS_REFERENCES | LONG_HAUL`
  at level VERBOSE. Direct3D12: `NAMES | DEVICES | OBJECT_LIFETIME | RESOURCES | APIS` at level 6.
  A rundown (`EVENT_CONTROL_CODE_CAPTURE_STATE`) is requested for both.
