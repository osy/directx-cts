# The report

`dxcts run` writes one JSON document (`--out`, or stdout). It separates **a case that
ran and failed** from **one that never ran**, which a log file cannot: the HLK
frameworks fail a case for setup and environment problems with the same verdict they
use for a genuine content mismatch. The distinction comes from the WTTLog proxy, which
sees the real `StartTest`/`EndTest` calls of both WGF generations, and from TAEF's and
gtest's own result output ([running.md](running.md#how-each-family-is-run)).

## Top level

| Key | Contents |
|---|---|
| `schema` | `1` |
| `target` | `machine`, `arch` (`x86`, `x64`, `arm64`), `platform` (`wine` or `windows`), `elevation` (`elevated`, `linked-token`, `limited` or `unknown`; see [running.md](running.md#on-wine)), and under Wine `wine_version` |
| `workarounds` | what was applied that can change what a test sees: `mtl-error-mode` (what `MTL_ERROR_MODE` was in the run's environment), and every workaround of the selected backend layer. Each has `name`, `active`, `what`, `why` and `alters_results` |
| `selection` | the run's options: `api`, `harness`, `binary`, `group`, `sample`, `adapter_type`, `src`, `graphics_finish_ms`, `query_timeout_ms`, `per_group`, `no_patches`, `d3d11on12`, `backend`, `exclusion` (the list's path) and `exclusion_entries` |
| `binaries` | one object per selected binary, below |
| `totals` | the counts below summed over every binary, plus the run-wide figures below |
| `stale_exclusions` | exclusion entries whose binary ran without them matching anything (`line`, `binary`, `harness`, `group`, `cases`); judged under `--per-group`, where the runner knows the group list |

`dxcts probe` writes `schema`, `target` and `workarounds` alone, with
`catalog_binaries`.

## Per binary

| Key | Contents |
|---|---|
| `binary`, `harness`, `api` | from the catalog |
| `exit_code` | the process's exit code (under `--per-group`, the last non-zero one) |
| `timed_out`, `wedged` | whether any process reached `--timeout`, or `--stall` |
| `exit_hung` | present when a process hung in its own exit after its summary |
| `seconds` | wall time |
| `slow` | present when a binary run as one process took longer than `--slow` |
| `cases_truncated` | present when the binary logged more than 4,194,304 cases |
| `crashes`, `crashed_cases` | for a binary run as one process: how many times it was resumed past a crash, and the case numbers |
| `excluded` | `reason` and `line` of the exclusion entry that kept the whole binary out |
| `exclusions` | what an exclusion list left out of a binary that ran: each `group`, with `cases` for a case set, its `reason` and `line` |
| `selectors_ignored` | what was asked for and could not be expressed to this family: `sample` (D3D9-era, TAEF, gtest - every case ran) and `exclusion` (a D3D9-era case set, or a WGF wildcard group in a whole-binary run) |
| `harness_reported_cases` | the case total TAEF or gtest reported itself, to check the records against |
| `failure_reasons` | for a binary run as one process: its dominant failure reasons (below) |
| `groups` | one object per group under `--per-group` (below) |
| `cases` | every case record (below) |
| `counts` | `cases`, `fixtures`, `failed_fixtures`, `executed`, `pass`, `fail`, `skip`, `blocked`, `warn`, `incomplete`, `not_run`; fixtures are not counted as cases |

### Per group

`name`, `cases`, `seconds`, `timed_out`, `wedged`, `exit_hung` (when it did), `exit_code`,
`altered_draws` (draws a backend layer dropped or changed; see
[backends.md](backends.md#knobs)), `host_validation_errors`, `slow`, `crashes` and
`crashed_cases` (when it was resumed past a crash), and `failure_reasons`.

`exit_code` non-zero without a timeout means the process crashed, which is a result
about the implementation; a timeout is not. A group killed at its timeout does not
report `altered_draws`, since its process never reached its own exit.

### Per case

| Key | Contents |
|---|---|
| `name` | the name the framework logged the case under (WGF cases share their group's name), `Class::Method` for TAEF, `Suite.Test` for gtest |
| `executed` | whether the case started |
| `result` | `pass`, `fail`, `skip`, `blocked`, `warn`, `incomplete` (started, never ended - a crash or a kill), `not-run`, or `unknown` (a result code outside the five) |
| `result_code` | the framework's own result code, when it gave one |
| `fixture` | present for a TAEF setup or cleanup fixture, which is recorded - a failed one is why the methods it guards did not run - but not counted as a case |

### Failure reasons

The event stream carries verdicts, not text, so after each group the runner reads the
group's own log and summarises why its cases failed: for every failing case, the last
line before its `End: Fail` that is not a parameter (`Format=R32_FLOAT`), noise (a
timestamp, a summary) or a generic wrapper when something more specific precedes it.
Up to eight distinct reasons per group are kept, each with `reason` and `cases`.

## Totals

The verdict counts summed over every binary, and `binaries`, `slow_groups`,
`crashed_cases_resumed`, `excluded_binaries`, `excluded_groups`, `excluded_case_sets`,
and two figures about the host:

* `host_validation_errors`: Metal validation failures the run survived because
  `MTL_ERROR_MODE` had Metal report rather than abort, counted out of the host log
  ([backends.md](backends.md#a-validation-failure-that-no-longer-aborts-is-still-a-failure));
* `pass_in_groups_with_host_errors`: passes recorded in a group where the host rejected
  something, which have not been shown to mean anything - a case expecting zeroes can
  pass on a resource the host refused to create.

## Tools

    ./tools/parity.py REFERENCE.json CANDIDATE.json [--binary NAME] [--all] [--reasons]
    ./tools/failure_report.py REPORT.json

`parity.py` compares two runs group by group and lists where the candidate falls short
of the reference - fewer passes, fewer cases enumerated, a group that timed out,
wedged, went missing or exited non-zero - which is how a change is shown to have cost
nothing. `--all` lists every group, `--reasons` adds each group's failure reasons.

`failure_report.py` lists a run's failing groups with the dominant reason the report
carries for each, and flags the reasons that point at something this suite supplies -
a shim, the results channel, module loading, adapter selection - rather than at the
implementation.
