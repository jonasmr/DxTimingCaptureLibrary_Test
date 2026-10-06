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

then start vramtiming, e.g. from another console (it opens its own console window for its log, or with
`-fullscreen` writes only `vramtiming.log` in its working directory):

```
vramtiming.exe 10 6144d 6144
```

This is the main repro: vramtiming allocates two blocks of render targets, A and B, renders A for
10 s, then frees A and renders B until it is closed, so the kernel can promote B back into video
memory. Each block should be about half the GPU's dedicated video memory, so that the two together
don't fit: 6144 MB on a 12 GB GPU as above, 3072 MB on a 6 GB GPU (`vramtiming.exe 10 3072d 3072`).

The simple case is one block, larger than video memory, rendered all the time (8192 on a 6 GB GPU,
16384 on a 12 GB GPU):

```
vramtiming.exe 10 16384
```

See vramtiming's README for the full command line.

`dxtcl_monitor` waits until a process named `vramtiming.exe` runs (the name is compared without
folder and case, `.exe` is optional), starts the ETW session for it and prints one line every second
until the target exits (close the vramtiming window) or Ctrl+C. vramtiming waits 2 seconds at startup,
so the session is up before it creates anything. Compare the two consoles (in windowed mode; with
`-fullscreen`, vramtiming's `vramtiming.log`): vramtiming's lines say
`block ... | alloc ... | vram(fast) ...`, the monitor's lines say `RT vram ...`.

`dxtcl_monitor.exe --pid <pid>` attaches to a running process directly. When the monitor attaches to
a process that is already running, it sees the objects created before that only through the ETW
rundown.

## What to look at

Excerpts from one run of `vramtiming.exe 10 6144d 6144` on an RTX 3060 12 GB. The two programs' `t=` clocks are offset (here by about 9 s), so match them by events, not by time.

vramtiming (from GPU timing), right after block A was freed and ~11 s later when B has been promoted:

```
t=11s  block B RT_384..767 | alloc 6144 MB | vram(fast) 2240MB/140 | sys(slow) 3904MB/244 | evicted(kernel) 3104MB | frame 1296.2 ms
t=22s  block B RT_384..767 | alloc 6144 MB | vram(fast) 6144MB/384 | sys(slow) 0MB/0 | evicted(kernel) 0MB | frame 40.5 ms
```

dxtcl_monitor (from the library), at the same two moments:

```
t=21s  RT vram 2336MB/146 sys 3808MB/238 unk 0MB/0 dem 3712MB/232 | ctr vramRes 3387 sysRes 2815 dem 2784 MB | pgIn 144 pgOut 196 segChg 119
t=30s  RT vram 2336MB/146 sys 3808MB/238 unk 0MB/0 dem 3712MB/232 | ctr vramRes 6166 sysRes 31 dem 0 MB | pgIn 318 pgOut 196 segChg 119
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

If the library is right, `RT vram` / `sys` match vramtiming's `vram(fast)` / `sys(slow)` and `unk`
is 0. `dem` and the `ctr ... dem` counter can be compared with vramtiming's `evicted(kernel)`.

What the run shows: after block A is freed, vramtiming's `sys(slow)` and `evicted(kernel)` fall to 0
within about 11 s as the kernel promotes B into video memory. The monitor's `ctr sysRes` / `dem` follow
to about 0 and `pgIn` rises from 144 to 318, but its `RT vram` / `sys` (2336MB/146 / 3808MB/238) and
`segChg` (119) do not change: the library reports no `OnAllocationSegmentGroupChanges` for the
promotion, although `Types.h` documents `AllocationSegmentGroupChange` as covering allocations "paged
out, or promoted back".

"MB" means MiB everywhere, as in vramtiming. The library reports its counters in MB of 10^6 bytes;
the monitor converts them to MiB. The `t=` values of the two programs are not aligned: the
monitor counts from when it attached to the target, vramtiming from when it starts rendering.

## Troubleshooting

- `EnableTraceEx2(D3D12) timed out (1460)`: a process that uses the D3D12 provider did not respond to
  the enable request in time. Retry, or close other D3D12 apps (browsers, streaming tools) and retry.
- Everything shows as `unk`, or events are lost: check the `events lost` count in the final `ETW session: ...` line, and look for
  stalled sessions with `logman query -ets`.

## Library settings used

- `DxTimingCaptureLibraryOptions`: `TrackApiObjects = true`.
- Callbacks: `ApiObjectCallbacks`, `ResidencyEventCallbacks`, `PixCounterCallbacks`, `DiagnosticsSink`.
- ETW (`etw_session.cpp`): real-time session, `Wnode.ClientContext = 1` (QPC),
  `PROCESS_TRACE_MODE_RAW_TIMESTAMP`. DxgKrnl: `BASE | RESOURCE | ALLOCATIONS_REFERENCES | LONG_HAUL`
  at level VERBOSE. Direct3D12: `NAMES | DEVICES | OBJECT_LIFETIME | RESOURCES | APIS` at level 6.
  A rundown (`EVENT_CONTROL_CODE_CAPTURE_STATE`) is requested for both.
