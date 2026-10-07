#!/usr/bin/env python3
"""
Bucket a run's failures by group, give each group its dominant reason, and judge whether
that reason could be the harness rather than the implementation.

Written because "263511 failures" says nothing on its own, and because the only way to
be sure the suite is measuring the implementation is to walk every group and ask what
made it fail.  Harness suspicion is deliberately broad here: it is cheaper to inspect a
false positive than to ship a run whose failures are our own.
"""
import collections, json, os, re, sys

NOISE = re.compile(r'^(Summary:|Start:|End:|SKIPPING|\d+$|[\d:]+ \d+/\d+/\d+$'
                   r'|Failure Breakdown|Error Breakdown|Device string|WTTLogger'
                   r'|Restoring |Test group|\xff\xfe)')
PARAM = re.compile(r'^[A-Za-z_][\w \[\]#.]*=')
WRAP  = re.compile(r'^(SetupTestCase failed\.|Failure Info:|.*\bfailed\.$)$')

# A reason is harness-suspect if it points at something we supply: our shims, our
# stubs, the results channel, module loading, or a device we handed the test.
HARNESS = [
    ("our driver-verifier stub",  re.compile(r'Driver Verifier DLL not loaded', re.I)),
    ("our WTTLog proxy",          re.compile(r'\bWTTLog\w*\b.*(fail|error|unable)', re.I)),
    ("our module rewrite",        re.compile(r'\b(dxc11|dxcg|dxc12|dxcts\w*)\b', re.I)),
    ("module could not load",     re.compile(r'(LoadLibrary|GetProcAddress|entry point)'
                                             r'.*(fail|not found)', re.I)),
    ("device we handed over",     re.compile(r'(D3D11CreateDevice|CreateDXGIFactory)'
                                             r'.*(fail|E_)', re.I)),
    ("adapter selection",         re.compile(r'(no adapter|adapter not found'
                                             r'|EnumAdapters.*fail)', re.I)),
    ("a draw we suppressed",      re.compile(r'draw (dropped|suppressed|skipped by)', re.I)),
]

def dominant_reasons(path):
    """(reason -> count) for the failing cases in one group log"""
    out = collections.Counter()
    try:
        d = open(path, 'rb').read().replace(b'\x00', b'').decode('latin1')
    except OSError:
        return out
    cur = []
    for line in d.splitlines():
        t = line.strip()
        if not t:
            continue
        if t.startswith('Start:'):
            cur = []
        elif t.startswith('End: Fail'):
            body = [m for m in cur if not NOISE.match(m) and not PARAM.match(m)]
            pick = next((c for c in reversed(body) if not WRAP.match(c)),
                        body[-1] if body else None)
            out[re.sub(r'-?\d+\.?\d*', 'N', pick)[:100] if pick else '(no log line)'] += 1
            cur = []
        else:
            cur.append(t)
    return out

def tag(binary, group):
    """log file name for a group, or for the whole binary when it ran as one process"""
    if group is None:
        return f"{binary[:-4]}.log"
    g = re.sub(r'[\\/ :*?"<>|]', '_', group)
    return f"{binary[:-4]}-{g}.log"

def main(report, logdir):
    rep = json.load(open(report))
    groups = []
    for b in rep['binaries']:
        fails = collections.Counter()
        for c in b.get('cases', []):
            if c['result'] == 'fail':
                fails[c['name']] += 1
        for g in b.get('groups', []):
            n = fails.get(g['name'], 0)
            if n:
                groups.append((n, b['binary'], g['name'], g.get('host_validation_errors', 0)))
        # A binary run as one process has no groups; its failures are bucketed as a whole.
        if not b.get('groups') and sum(fails.values()):
            groups.append((sum(fails.values()), b['binary'], None,
                           b.get('host_validation_errors', 0)))
    groups.sort(reverse=True)
    print(f"{len(groups)} groups have failures, {sum(g[0] for g in groups)} failures total\n")
    suspect = []
    for n, binary, group, herr in groups:
        reasons = dominant_reasons(os.path.join(logdir, tag(binary, group)))
        top = reasons.most_common(1)
        why = top[0][0] if top else '(no log)'
        flags = [name for name, rx in HARNESS if rx.search(why)]
        if flags:
            suspect.append((n, binary, group, why, flags))
        print(f"{n:7} {binary[:-4]:22} {(group or '(whole binary)')[:34]:34} {why[:70]}")
    print(f"\n=== harness-suspect groups: {len(suspect)} ===")
    for n, b, g, why, flags in suspect:
        print(f"  {n:7} {b[:-4]:20} {(g or '(whole binary)')[:30]:30} {','.join(flags)}: {why[:60]}")

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
