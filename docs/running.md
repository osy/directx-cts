# Running

`dxcts.exe` sits in the staging directory and is run from there, inside the bottle or
on the Windows machine. It reads `suites/graphics.jsonl`, runs every selected binary as
a child process, reads back what each reported, and writes one report
([report.md](report.md)). Tests are spawned rather than hosted, so a crashing or wedged
one costs only its own result. Every child runs in a kill-on-close job, so nothing it
spawns outlives it, and at normal priority (a scheduled task would otherwise leave it
backgrounded, which Chromium's gtest launcher refuses).

## On Wine

**Run it through the `dxcts` launcher beside it**, not `dxcts.exe` directly. Staging
generates it, and it supplies what the run needs before Wine starts:

* `MTL_ERROR_MODE=nslog`, so a Metal validation failure is reported rather than
  aborting the process ([backends.md](backends.md#mtl_error_mode)). Metal reads it from
  the Unix environment, which a PE cannot change: it has to be set when Wine starts.
  The runner warns on its progress stream when it is missing and records what it saw in
  the report either way;
* `DXCTS_HOST_LOG`, the file the host's own stderr is appended to, where the runner
  counts Metal validation errors (default `dxcts-out/host-stderr.log`);
* inside a CrossOver bottle (`.../Bottles/<name>/drive_c/...`), the bottle name and the
  `C:` path of the staging directory, derived from where it was copied to.

Wine's stderr - the runner's own messages included - goes to that host log, so when
the runner exits with an error the launcher repeats the runner's `dxcts:` lines on the
terminal.

`WINE` overrides the wine it uses; outside a bottle it is `wine` on `PATH`, with
whatever `WINEPREFIX` is set. CrossOver's own graphics-backend switch applies as usual,
for instance `CX_ENV=CX_GRAPHICS_BACKEND=dxmt ./dxcts run ...` for DXMT.

Wine (10 and later) starts every process with a limited administrator token, and some tests refuse
one ("This test must be run elevated." - DXGIMultiPresent, and the ETW listeners behind
the DXGI present tests). When its own token is limited the runner starts every test
under the token's linked full token instead; Windows denies that to an unprivileged
caller, and there the tests run under whatever token the runner has. The report's
`target.elevation` says which: `elevated`, `linked-token` or `limited`.

## On Windows

* **Run from the interactive console session.** Over SSH, tests land on a service
  window station and a WGF binary hangs after creating its device rather than failing.
  Drive them with `schtasks /it` pointed at a `.cmd` wrapper - not at the command
  directly: `schtasks /tr` mangles embedded quotes, and the task then silently does
  nothing.
* This applies to any Windows install - a real machine or a VM, x64 or arm64. Stage
  with `--target windows` and the architecture the tests should run as: Windows
  provides the modules the Wine shims replace, and only the shims needed everywhere are
  staged ([shims.md](shims.md#on-windows)).
* The WTTLog proxy is staged there too: it is the results channel, and without it a run
  reports no cases at all while the framework's own log shows them passing.

## The D3D9-era binaries

The `wgf9` binaries exist in every architecture's kit, but their framework needs
`d3dx8d.dll`, which only the x86 kit ships, so run them from an x86 staging on every
target - x86 or x64 Wine, x86 or 64-bit Windows, or an arm64 one that runs x86
binaries under emulation:

    ./scripts/stage.py --arch x86               # Wine
    ./scripts/stage.py --arch x86 --target windows

## How each family is run

The runner drives four framework families, as the catalog's `harness` names them.

**`wgf`** - the standalone WGF framework (WGF11\*, 10L9\*, DXGI\*). The group is a
positional backslash path; the runner passes `-TextLog -LogAll -LogClean`, the log path
and file name, `-AdapterType`, `-Src`, `-11on12`, `-GraphicsFinishTime`, `-QueryTimeout`
and, with `--sample`, `-Random -Seed`. `-LogAll` matters: without it a passing case
produces no log record and no WTTLog call, so passes would be invisible. The framework's
option parser does not strip quotes, so switch values are passed bare. Cases are
recorded from the WTTLog proxy's event stream.

**`wgf9`** - the same framework's D3D9-era generation. Its groups are switches
(`-Key`, `-Key:1-50,90-` for case numbers within one). It writes its text log to
`%USERPROFILE%\dxlogs\<binary>.log` whatever it is told, overwriting it each time, so
each process's log is moved beside the rest of its output.

**`taef`** - a TAEF test DLL, run through `TE.exe`; a group is a class. Under Wine it
runs `/inproc`: TAEF's out-of-process host deadlocks there (`TE.exe` and
`te.processhost.exe` both idle after the ncalrpc handshake, no test ever starting). That
costs coverage - a class whose metadata needs a fresh host is refused in-process ("TAEF
would need to start a second test host") and its methods are blocked - so it is
Wine-only. TAEF's cases are read from `TE.exe`'s console output, one
`StartGroup:`/`EndGroup:` pair per method, because `Wex.Logger` never calls the WTTLog
entry points the proxy observes. A method TAEF reports "Not Run" stays `executed: false`,
setup and cleanup fixtures are recorded but not counted as cases, and TAEF's own
`Summary: Total=` is kept as `harness_reported_cases`. TAEF buffers its console output
until the process exits, so a TAEF process killed at its timeout yields no case records.

**`gtest`** - a GoogleTest binary (Dawn's end-to-end tests); a group is a suite,
`Suite.*`. It is read from the report it is asked to write with `--gtest_output=json:`,
which Dawn's own launcher writes as gtest XML; both are read. One process per suite
matters more here than anywhere: Dawn's launcher gives up on the whole binary when a
batch child dies.

### --per-group

`--per-group` runs each group as its own process, listed first from the binary itself.
It costs a process launch per group, but a group that wedges the host or kills its
process then costs only itself instead of everything after it in that binary. Listing:

* **wgf**: the `Test groups:` tree `-?` prints, the only listing that keeps the paths
  intact; parents are dropped, since running one reruns everything under it. A binary
  whose windowed shell shows that tree in a dialog and never returns (DXGISwap) is
  listed from the files `-GenerateCSV` writes, one per group - but only when no name
  has an `_` in it, since the file names flatten `\` into `_` and a guessed path would
  run the whole binary.
* **wgf9**: the help dialog `-?` opens, which waits to be closed. The runner reads its
  rich-edit control a line at a time (Wine's rich edit flattens line breaks in
  `WM_GETTEXT`), takes the `-Key:` lines at the margin as groups (a test's own options
  are indented under them), and kills the process.
* **taef**: `TE.exe <dll> /list`, one class per group.
* **gtest**: `--gtest_list_tests`, one suite per group.

A listing gets 60 s. A WGF `-?` that does not finish falls back to `-GenerateCSV`, as
above; any other listing that does not finish, comes back empty or is cut off at 512
groups runs the binary whole instead.

### wgf9 case ranges

A D3D9-era group runs in ranges of 25,000 case numbers, one process each, until a range
reaches the group's last case (`-Key:1-25000`, then `25001-50000`, ...): StretchRect's
first group alone is past 255,000 cases. The framework repeats a group once per device
mode and numbers each repetition from 1, and a range applies to every repetition, so
the ranges partition the group. A range whose process dies costs the rest of that range,
the case it died in is named, and the next range runs; each range's passing
`SetupTest`/`CleanupTest` pair is counted once. The D3D9-era framework has no sampling
option, so `--sample` does not apply to it: every case runs, and the report records the
sample as ignored.

## Timeouts, stalls and hangs

Liveness is any growth in what the process writes - the WTTLog event stream, its
stdout and stderr - and, on Windows only, CPU time used by its job: WARP can compute for
many minutes with nothing to print, while under Wine a host fault re-entering the signal
handler burns CPU just as steadily as real work.

* `--timeout` (default 7200 s) bounds each process; the catalog's `timeout_s` raises
  it for the binaries measured to need more, unless `--timeout` is given. A process
  that reaches it is reported `timed_out`.
* `--stall` (default 600 s): a process whose output has not grown for that long is
  ended and reported `wedged` - a host fault Wine cannot raise as an exception leaves a
  process spinning with no further cases, and waiting out the whole timeout would cost
  hours and say only "slow".
* A process whose log already holds its closing `Summary: Total=` and then sits quiet
  for 30 s is stuck in its own exit: it is ended, its verdicts are kept, and it is
  reported `exit_hung` with exit code 0.
* `--slow` (default 60 s) names a group that takes longer, without failing it, so a new
  host-side stall shows up as a number long before it reaches the timeout.

## Crashes

Under Wine an unhandled exception would hand the process to `winedbg --auto`, which
attaches without ever signalling, so a crash would look like a hang. Every shim sets
`SEM_NOGPFAULTERRORBOX` when it loads, which short-circuits that filter: ntdll
terminates the process instead, and the runner sees a crash (`src/common/shim.h`). On
Windows the same flag suppresses the WER dialog that would stall an unattended run.

That only works when the faulting thread is one Wine created. A fault on a libdispatch
worker - where D3DMetal compiles pipelines - re-enters Wine's signal handler without
ever raising a Windows exception, and the process wedges; that is what `--stall` and
`MTL_ERROR_MODE` ([backends.md](backends.md#mtl_error_mode)) are for.

**A WGF group whose process crashes is resumed past the crashing case** (not under
`--sample`, which every process draws afresh). The case that died leaves no trace -
the framework logs a case only when it finishes - so the runner takes the last case the
log recorded as finished, finds the next one in the group's own `-GenerateCSV` listing,
records it as `incomplete`, names it in the group's `crashed_cases`, and starts the
group again with every case up to it skipped (`"!Group:1-N"`), up to 256 times. A crash
inside the implementation under test then costs that one case, not the rest of the
group.

## Deployed data

A binary that reads data from a fixed path outside the staging tree - one the HLK job
would deploy before running it - names it in the catalog with `deploy_from` and
`deploy_to`, and the runner copies it there first. `DirectML.Test.ModelTests` reads its
models from `%TEMP%\directml.test.modeltests.0.0.4` and fails every method with
"Invalid model path" otherwise.

## Debugging a timeout

On Windows, attach `cdb` and read the stacks; it is the fastest way to tell a spin from
a wait, and it names the call:

    cdb -pn TE.exe -c "~*k 30;q"

Run it from a `.cmd` in the console session, giving the test time to reach the hang.
That is how `D3DConf_12_0_MeshShaders` was settled on WARP: its test thread sits in
`MeshShadersBase::FlushAndFinish` under `D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU`,
blocked on a GPU fence rather than spinning, so WARP is slow rather than stuck - and
the catalog gives that binary the time it needs.

Under Wine it is harder: `sample(1)` attributes every PE frame to one address inside
Wine's unix-side ntdll, the last native frame before emulated code, and `winedbg`'s
`bt all` walks every process in the bottle; attach and take the backtrace in one
session, since detaching kills the target. Reproduce on Windows where you can, and fall
back to instrumenting a shim - a recorder in the device vtable plus a watchdog that
writes while wedged - for what only happens under Wine.
