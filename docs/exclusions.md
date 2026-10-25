# Exclusion lists

What still wedges, crashes or otherwise breaks a run on a given implementation after
everything here has been fixed is kept out of it by an **exclusion list**, one per
implementation, in `suites/exclusions/<name>.jsonl`. With it, one command runs every
test that can run and gives the same answer twice:

| List | Implementation | Run as |
|---|---|---|
| `warp` | WARP, on Windows with no other GPU driver | `dxcts run --per-group --src WARP --exclusion warp` |
| `d3dmetal` | D3DMetal behind the Neptune VM's guest driver | `dxcts run --per-group --backend d3dmetal --exclusion d3dmetal` |
| `crossover` | CrossOver: D3DMetal 3.0 for D3D10-12 and DXGI, wined3d for D3D9 | `./dxcts run --per-group --backend d3dmetal --exclusion crossover` |
| `dxmt` | DXMT behind the Neptune VM's guest driver | `dxcts run --per-group --exclusion dxmt` |
| `crossover-dxmt` | the older DXMT CrossOver bundles, in the bottle | `CX_ENV=CX_GRAPHICS_BACKEND=dxmt ./dxcts run --per-group --exclusion crossover-dxmt` |

The two DXMT lists differ because the DXMT versions do.

## Format

A bare name is a list staged beside `dxcts` (staging copies `suites/` whole); anything
with a separator or extension is a path. Each line is one flat JSON object, like the
catalog:

    {"binary":"WGF11Foo.exe","group":"Foo\\Bar","cases":"12-15,40","reason":"...","evidence":"..."}

* `binary` alone excludes the binary; `harness` (`wgf`, `wgf9`, `taef`, `gtest`) in
  place of or alongside it names a whole framework family, for the case where one
  reason covers every binary of it. `group` narrows it to a group — and, for WGF, the
  subtree under it, as the framework's own `!Group` does. `cases` narrows it further:
  WGF case numbers, as the binary's own `-GenerateCSV` listing numbers them, or the
  TAEF method / gtest test names within the group, comma-separated, with `*` allowed in
  a name. `binary` and `group` take `*` and `?`, matched without regard to case.
* **`reason` is required**, and a line without one is refused, as is any key the runner
  does not know (a misspelt `group` would otherwise widen the entry to the whole
  binary). `evidence` is free text describing the run, log or stack that showed it, in
  the entry's own words rather than as a pointer to files outside the repository, so
  the list can be read on its own.
* A list holds at most 512 entries, and a line at most 16 KiB; a longer line, or a
  value over 4,095 characters, is refused rather than cut. Keep `binary` under 128
  characters, `group` under 256 and `reason` under 2,048: those are stored at that
  size.
* Fix the harness first. A line belongs here only when the fault is the
  implementation's and cannot be survived — a timeout, a wedge, a process death that
  takes the group with it. A case that fails is a result, never an exclusion.

## How each family is told

WGF takes `!Group` and `!Group:1-3,7` as positionals and
neither runs nor logs what they name, so the counts are not inflated by skips; TAEF gets
a `/select` query; gtest a negative `--gtest_filter`. Under `--per-group` an excluded
group is simply never spawned. A wgf9 (D3D9-era) binary's framework can select cases
but never leave any out, so a `cases` entry for one is reported under
`selectors_ignored` as `exclusion` rather than quietly run, and so is a WGF wildcard
group in a whole-binary run, since the framework has no wildcards of its own.

## In the report

The report says what was left out and why: `selection.exclusion` names the list, each
binary carries `excluded` (the whole binary) or an `exclusions` array (groups not
spawned, case sets skipped, each with its reason and line), and `totals` counts
`excluded_binaries`/`excluded_groups`/`excluded_case_sets`. `stale_exclusions` lists the
entries whose binary ran without them matching anything — a group renamed or gone —
so the list only ever shrinks. (Staleness is judged under `--per-group`, where the
runner sees the group list; a whole-binary run hands entries to the framework blind.)

The catalog's `skip` is different: it marks a binary that cannot run on *any*
implementation (it needs a second adapter, a capture device), and stays there.
