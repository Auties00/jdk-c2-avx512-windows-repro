#!/usr/bin/env python3
"""Rewrites the measured-results section of README.md from the summary of a windows-matrix run.

Usage: results-to-readme.py <summary.tsv> <run URL> [README.md]
The section is the text between the two marker comments in the README. MIT License.
"""
import re
import sys

BEGIN = "<!-- windows-matrix:begin -->"
END = "<!-- windows-matrix:end -->"
ARCH = {"x64": "x64 (KVM guests)", "arm64": "Arm64 (emulated guests, SVE)"}


def version_key(text):
    return [int(part) for part in re.findall(r"\d+", text)]


def main():
    summary, run = sys.argv[1], sys.argv[2]
    readme = sys.argv[3] if len(sys.argv) > 3 else "README.md"
    rows = [line.rstrip("\n").split("\t") for line in open(summary, encoding="utf-8")][1:]
    out = [BEGIN, "", "Measured in CI ([run %s](%s)); each medium was verified before it booted:" % (run.rsplit("/", 1)[-1], run), ""]
    for arch in ("x64", "arm64"):
        mine = [r for r in rows if len(r) >= 8 and r[1] == arch]
        if not mine:
            continue
        out.append("%s:" % ARCH[arch])
        mine.sort(key=lambda r: (version_key(r[2]) or [0], r[4]), reverse=True)
        for name, _, kernel, reported, configuration, _, _, result in mine:
            line = "- Kernel %s, %s" % (kernel, configuration)
            if reported not in ("not reported", "") and not reported.endswith("." + kernel):
                line += " (the system reports %s)" % reported
            out.append("%s: %s. Medium: %s." % (line, result, name))
        out.append("")
    out.append(END)
    text = open(readme, encoding="utf-8").read()
    block = "\n".join(out)
    if BEGIN in text:
        text = text[:text.index(BEGIN)] + block + text[text.index(END) + len(END):]
    else:
        text = text.rstrip("\n") + "\n\n" + block + "\n"
    open(readme, "w", encoding="utf-8", newline="\n").write(text)


if __name__ == "__main__":
    main()
