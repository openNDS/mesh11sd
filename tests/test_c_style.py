#!/usr/bin/env python3
"""Header and indentation checks for the C sources.

Python, YAML, and Markdown are not tab-indented: tabs are a Python syntax
error, are not valid YAML indentation, and disturb Markdown layout.
"""
import hashlib
import re
from pathlib import Path

repository = Path(__file__).resolve().parent.parent
author = "@author Copyright (C) 2026 BlueWave Projects and Services <opennds@blue-wave.net>"
preserved = {"sha256.c", "sha256.h"}
sources = sorted(path for base in ("src", "tests") for path in (repository / base).rglob("*") if path.suffix in {".c", ".h"})
assert sources, "no C sources found"


def normalize(src):
    out = []
    i = 0
    n = len(src)
    while i < n:
        c = src[i]
        if c in "\"'":
            quote = c
            out.append(c)
            i += 1
            while i < n:
                out.append(src[i])
                if src[i] == "\\":
                    i += 1
                    if i < n:
                        out.append(src[i])
                        i += 1
                    continue
                if src[i] == quote:
                    i += 1
                    break
                i += 1
            continue
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            i += 2
            while i < n and src[i] != "\n":
                i += 1
            continue
        if c == "/" and i + 1 < n and src[i + 1] == "*":
            i += 2
            while i + 1 < n and not (src[i] == "*" and src[i + 1] == "/"):
                i += 1
            i = min(n, i + 2)
            continue
        if c.isspace():
            i += 1
            continue
        out.append(c)
        i += 1
    return "".join(out)


def token_digest():
    chunks = []
    for path in sources:
        rel = path.relative_to(repository).as_posix()
        chunks.append(rel.encode() + b"\n" + normalize(path.read_text()).encode() + b"\n")
    return hashlib.sha256(b"".join(chunks)).hexdigest()


checks = 0

def check(name, condition):
    global checks
    checks += 1
    assert condition, name
    print("ok", checks, "-", name)


for path in sources:
    text = path.read_text()
    rel = path.relative_to(repository).as_posix()
    check(rel + " names itself in @file", f"@file {path.name}" in text)
    brief = re.search(r"^ \* @brief \S.+$", text, re.M)
    check(rel + " has one @brief", brief is not None)
    if path.name in preserved:
        if path.name == "sha256.c":
            check(rel + " keeps its public-domain SHA-256 notice",
                  "Minimal public-domain style SHA-256 (FIPS 180-4). No external libs." in text)
        check(rel + " does not gain a new BlueWave author line", author not in text)
    else:
        check(rel + " carries the 2026 BlueWave author line", author in text)
    for number, line in enumerate(text.splitlines(), 1):
        if line.strip() == "":
            continue
        assert not re.match(r"^ {4,}", line), rel + ":" + str(number) + " still uses a four-space indent"
        assert re.match(r"^(?:\t*) {0,3}\S", line), rel + ":" + str(number) + " has mixed leading whitespace"
    check(rel + " uses tab indentation", True)

for path in list((repository / "scripts").glob("*.py")) + list((repository / "tests").glob("*.py")):
    for number, line in enumerate(path.read_text().splitlines(), 1):
        assert not line.startswith("\t"), str(path.relative_to(repository)) + ":" + str(number) + " uses a tab indent"
check("Python tests and scripts stay space-indented", True)

expected = (repository / "tests" / "c-token-digest.txt").read_text().strip()
check("normalized C tokens match the cosmetic baseline", token_digest() == expected)
print(str(checks) + " style checks; comments and indentation only")
