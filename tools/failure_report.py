#!/usr/bin/env python3
"""
List a run's failing groups with each group's dominant reason, and judge whether that
reason could be the harness rather than the implementation.

    failure_report.py report.json

Written because "263511 failures" says nothing on its own, and because the only way to
be sure the suite is measuring the implementation is to walk every group and ask what
made it fail.  The reasons are the ones the runner read out of each group's log into
the report.  Harness suspicion is deliberately broad here: it is cheaper to inspect a
false positive than to ship a run whose failures are our own.
"""
import collections, json, re, sys

# A reason is harness-suspect if it points at something we supply: our shims, the
# results channel, module loading, or a device we handed the test.
HARNESS = [
    ("driver verifier",           re.compile(r'Driver Verifier DLL not loaded', re.I)),
    ("our WTTLog proxy",          re.compile(r'\bWTTLog\w*\b.*(fail|error|unable)', re.I)),
    ("our module rewrite",        re.compile(r'\b(dxc11|dxcg|dxc12|dxcts\w*)\b', re.I)),
    ("module could not load",     re.compile(r'(LoadLibrary|GetProcAddress|entry point)'
                                             r'.*(fail|not found)', re.I)),
    ("device we handed over",     re.compile(r'(D3D11CreateDevice|CreateDXGIFactory)'
                                             r'.*(fail|E_)', re.I)),
    ("adapter selection",         re.compile(r'(no adapter|adapter not found'
                                             r'|EnumAdapters.*fail|subset .* of size \(0\))',
                                             re.I)),
]


def main(report):
    rep = json.load(open(report))
    groups = []
    for b in rep['binaries']:
        fails = collections.Counter(c['name'] for c in b.get('cases', [])
                                    if c['result'] == 'fail')
        for g in b.get('groups', []):
            if fails.get(g['name']):
                groups.append((fails[g['name']], b['binary'], g['name'],
                               g.get('failure_reasons', [])))
        # A binary run as one process has no groups; its failures are bucketed as a whole.
        if not b.get('groups') and fails:
            groups.append((sum(fails.values()), b['binary'], None,
                           b.get('failure_reasons', [])))
    groups.sort(key=lambda g: -g[0])
    print(f"{len(groups)} groups have failures, {sum(g[0] for g in groups)} failures total\n")
    suspect = []
    for n, binary, group, reasons in groups:
        why = max(reasons, key=lambda r: r['cases'])['reason'] if reasons else '(no reason)'
        flags = [name for name, rx in HARNESS if rx.search(why)]
        if flags:
            suspect.append((n, binary, group, why, flags))
        print(f"{n:7} {binary[:-4]:22} {(group or '(whole binary)')[:34]:34} {why[:70]}")
    print(f"\n=== harness-suspect groups: {len(suspect)} ===")
    for n, b, g, why, flags in suspect:
        print(f"  {n:7} {b[:-4]:20} {(g or '(whole binary)')[:30]:30} {','.join(flags)}: {why[:60]}")


if __name__ == '__main__':
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    main(sys.argv[1])
