"""Aggregates the results.tsv files of every job into one markdown table (stdout)."""
import collections
import pathlib
import sys

FIELDS = ["os", "cpu", "build", "isa", "jdk", "test", "flags", "seed", "result", "detail"]

rows = []
for path in sorted(pathlib.Path(sys.argv[1]).rglob("results.tsv")):
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        parts = line.split("\t")
        if len(parts) == len(FIELDS):
            row = dict(zip(FIELDS, parts))
            row["job"] = path.parent.name
            rows.append(row)

jobs = sorted({(r["job"], r["os"], r["cpu"], r["build"], r["isa"]) for r in rows})
print("## Hosts\n")
print("| job | os | cpu | os build | isa |")
print("|---|---|---|---|---|")
for job in jobs:
    print("| " + " | ".join(job) + " |")

groups = collections.OrderedDict()
for r in rows:
    key = (r["os"], r["cpu"], r["isa"], r["jdk"], r["test"], r["flags"])
    g = groups.setdefault(key, {"runs": 0, "bad": 0, "jobs": set(), "bad_jobs": set(), "kinds": collections.Counter()})
    g["runs"] += 1
    g["jobs"].add(r["job"])
    if r["result"] != "ok":
        g["bad"] += 1
        g["bad_jobs"].add(r["job"])
        g["kinds"][r["result"]] += 1

print("\n## Results (failed runs / runs; a run is one seed or one probe invocation)\n")
print("| os | cpu | isa | jdk | test | flags | failed/runs | hosts failing/hosts | kinds |")
print("|---|---|---|---|---|---|---|---|---|")
for key in sorted(groups):
    g = groups[key]
    kinds = ", ".join(f"{k} {v}" for k, v in g["kinds"].items())
    print(f"| {' | '.join(key)} | {g['bad']}/{g['runs']} | {len(g['bad_jobs'])}/{len(g['jobs'])} | {kinds} |")

print("\n## Failure details\n")
for r in rows:
    if r["result"] != "ok":
        print(f"- {r['job']} {r['cpu']} {r['jdk']} {r['test']} [{r['flags']}] seed={r['seed']}: {r['result']} {r['detail']}")
