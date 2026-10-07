#!/usr/bin/env python3
"""
Compare two dxcts reports group by group and say where the second falls short of the
first: fewer passing cases, fewer cases enumerated, or a group that timed out or wedged.

    parity.py REFERENCE.json CANDIDATE.json [--binary NAME] [--all] [--reasons]

The reference is treated as fixed; only the candidate's shortfalls are listed. A group
is at parity when the candidate passes at least as many cases as the reference and
enumerates at least as many; the pass count is what matters, since a case the reference
skips and the candidate fails is still a case the reference does not pass.
"""
import collections, json, sys


def load(path):
    d = json.load(open(path))
    out = {}
    for b in d["binaries"]:
        name = b["binary"]
        groups = {}
        if "groups" in b:
            for g in b["groups"]:
                groups[g["name"]] = dict(g)
                groups[g["name"]].setdefault("counts", collections.Counter())
        # per-case verdicts, bucketed by group name (a case record's name IS its group)
        for c in b.get("cases", []):
            g = groups.setdefault(c["name"], {"name": c["name"], "counts": collections.Counter()})
            r = c["result"] if c.get("executed") else "not_run"
            g["counts"][r] += 1
            g["counts"]["cases"] += 1
        out[name] = {"binary": b, "groups": groups}
    return d, out


def cnt(g, k):
    return g.get("counts", {}).get(k, 0)


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    ref_d, ref = load(argv[1])
    cand_d, cand = load(argv[2])
    only = None
    show_all = "--all" in argv
    reasons = "--reasons" in argv
    if "--binary" in argv:
        only = argv[argv.index("--binary") + 1]

    tot_gap = 0
    tot_groups = 0
    rows = []
    for bname, r in ref.items():
        if only and bname != only:
            continue
        c = cand.get(bname)
        for gname, rg in r["groups"].items():
            rp = cnt(rg, "pass")
            rc = cnt(rg, "cases")
            cg = c["groups"].get(gname) if c else None
            cp = cnt(cg, "pass") if cg else 0
            cc = cnt(cg, "cases") if cg else 0
            cf = cnt(cg, "fail") if cg else 0
            cs = cnt(cg, "skip") if cg else 0
            gap = rp - cp
            flags = []
            if cg is None:
                flags.append("MISSING")
            else:
                if cg.get("timed_out"):
                    flags.append("TIMEOUT")
                if cg.get("wedged"):
                    flags.append("WEDGED")
                if cg.get("exit_code") not in (None, 0):
                    flags.append("exit=%s" % cg.get("exit_code"))
            if gap > 0 or cc < rc or flags or show_all:
                rows.append((gap, bname, gname, rp, rc, cp, cc, cf, cs, " ".join(flags), cg))
            if gap > 0:
                tot_gap += gap
                tot_groups += 1

    rows.sort(key=lambda x: -x[0])
    print("%-22s %-40s %8s %8s %8s %8s %6s %6s  %s" % (
        "binary", "group", "ref_pass", "ref_case", "cnd_pass", "cnd_case", "fail", "skip", "flags"))
    for gap, b, g, rp, rc, cp, cc, cf, cs, fl, cg in rows:
        print("%-22s %-40s %8d %8d %8d %8d %6d %6d  %s" % (
            b.replace(".exe", ""), g[:40], rp, rc, cp, cc, cf, cs, fl))
        if reasons and cg and cg.get("failure_reasons"):
            for fr in cg["failure_reasons"][:4]:
                print("    %6d  %s" % (fr.get("cases", 0), fr.get("reason", "")[:110]))
    rt, ct = ref_d["totals"], cand_d["totals"]
    print()
    print("totals   ref: cases %d pass %d fail %d skip %d" % (rt["cases"], rt["pass"], rt["fail"], rt["skip"]))
    print("totals  cand: cases %d pass %d fail %d skip %d timeouts? %s" % (
        ct["cases"], ct["pass"], ct["fail"], ct["skip"],
        sum(1 for b in cand_d["binaries"] for g in b.get("groups", []) if g.get("timed_out") or g.get("wedged"))))
    print("pass gap: %d cases short across %d groups" % (tot_gap, tot_groups))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
