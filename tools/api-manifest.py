#!/usr/bin/env python3
# Copyright © 2026 Kyle J. Cardoza.
# SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Generate libagora-audio's API manifest from a macOS SDK.

    tools/api-manifest.py --sdk /path/to/MacOSX10.12.sdk \
        --out tests/api/manifest-10.12.txt \
        CoreAudio/CoreAudioTypes.h CoreAudio/HostTime.h

The manifest is the FACTS the headers in this repository are written from and checked against:
record sizes, alignments and field offsets, the value of every enumeration constant, every
typedef, and the qualified type of every function. It is committed. The SDK is not — an SDK is
Apple's text, a manifest is measurements.

WHY THIS RUNS ON LINUX: clang parses Apple's headers given three shims (`tools/refshim/`) and a
copy of the header tree with Darwin's nullability keywords spelled the way clang spells them
everywhere else. That copy is made in a scratch directory; the SDK is only ever read.

TWO PASSES, BECAUSE CLANG'S LAYOUT DUMP IS DEMAND-DRIVEN: a record's layout is dumped when
something needs it, so a translation unit that only includes headers dumps nothing. The first pass
records the declarations from the AST; the second declares one variable of every record found and
asks clang for the layouts of those.
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

CLANG = os.environ.get("CLANG", "/usr/bin/clang-19")
FRAMEWORKS = ("CoreAudio", "AudioToolbox", "AudioUnit", "CoreAudioTypes", "CoreMIDI")

NULLABILITY = (
    (re.compile(r"\b__nonnull\b"), "_Nonnull"),
    (re.compile(r"\b__nullable\b"), "_Nullable"),
    (re.compile(r"\b__null_unspecified\b"), "_Null_unspecified"),
)


def stage(sdk, dest):
    """Copy the SDK's audio framework headers into `dest/<Framework>/...`."""
    copied = 0
    for framework in FRAMEWORKS:
        for candidate in (
            os.path.join(sdk, "System/Library/Frameworks", framework + ".framework/Versions/A/Headers"),
            os.path.join(sdk, "System/Library/Frameworks", framework + ".framework/Headers"),
        ):
            if os.path.isdir(candidate):
                target = os.path.join(dest, framework)
                os.makedirs(target, exist_ok=True)
                for entry in sorted(os.listdir(candidate)):
                    source = os.path.join(candidate, entry)
                    if os.path.isfile(source) and entry.endswith(".h"):
                        with open(source, "r", errors="surrogateescape") as handle:
                            text = handle.read()
                        for pattern, replacement in NULLABILITY:
                            text = pattern.sub(replacement, text)
                        with open(os.path.join(target, entry), "w", errors="surrogateescape") as handle:
                            handle.write(text)
                        copied += 1
                break
    if copied == 0:
        sys.exit("api-manifest: no framework headers under %s" % sdk)
    return copied


def run(args):
    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode != 0:
        sys.stderr.write(result.stderr)
        sys.exit("api-manifest: %s failed" % args[0])
    return result


def records(layout_text):
    """Parse `-fdump-record-layouts-complete` into {name: (size, align, [(offset, type, name)])}."""
    out = {}
    current = None
    for line in layout_text.splitlines():
        start = re.match(r"^\s*0 \| (?:struct|union|class) (\w+)$", line)
        if start:
            current = start.group(1)
            out[current] = {"size": None, "align": None, "fields": []}
            continue
        if current is None:
            continue
        size = re.search(r"\[sizeof=(\d+), align=(\d+)\]", line)
        if size:
            out[current]["size"] = int(size.group(1))
            out[current]["align"] = int(size.group(2))
            current = None
            continue
        field = re.match(r"^\s*(\d+) \|\s{3}(.+)$", line)
        if field:
            type_and_name = field.group(2).strip()
            parts = type_and_name.rsplit(" ", 1)
            if len(parts) == 2 and re.match(r"^\w+$", parts[1]):
                out[current]["fields"].append((int(field.group(1)), parts[0], parts[1]))
    return {name: value for name, value in out.items() if value["size"] is not None}


def loc_file(loc):
    """The file a node came from. A declaration produced by a macro — and every CF_ENUM enum
    constant is — carries its location nested under expansionLoc/spellingLoc, not at the top."""
    if not isinstance(loc, dict):
        return None
    if isinstance(loc.get("file"), str):
        return loc["file"]
    for key in ("expansionLoc", "spellingLoc", "presumedFile"):
        if key == "presumedFile" and isinstance(loc.get(key), str):
            return loc[key]
        found = loc_file(loc.get(key))
        if found:
            return found
    return None


def walk(tree, root):
    """Yield (file, node) in source order. clang's JSON omits `file` when it repeats the PREVIOUS
    node's file (not the parent's), so the running file has to be carried across the whole walk."""
    state = {"file": None}
    stack = [tree]
    while stack:
        node = stack.pop()
        found = loc_file(node.get("loc"))
        if found:
            state["file"] = found
        yield state["file"], node
        for child in reversed(node.get("inner") or []):
            stack.append(child)


def constant_value(node):
    """The numeric value of an EnumConstantDecl. clang records it on a ConstantExpr that may sit
    under an ImplicitCastExpr and a ParenExpr — `(1U << 2)` is three nodes deep — so search by
    breadth: the shallowest node carrying a value is the one that folds the whole expression."""
    queue = list(node.get("inner") or [])
    while queue:
        current = queue.pop(0)
        if "value" in current:
            return current["value"]
        queue.extend(current.get("inner") or [])
    return "0"


def ast_facts(tree, prefix):
    constants, typedefs, functions, records_seen = {}, {}, {}, []
    for path, node in walk(tree, None):
        if path is None or not path.startswith(prefix):
            continue
        kind = node.get("kind")
        if kind == "EnumConstantDecl":
            constants[node["name"]] = constant_value(node)
        elif kind == "TypedefDecl":
            typedefs[node["name"]] = node.get("type", {}).get("qualType", "")
        elif kind == "FunctionDecl" and not node.get("isImplicit"):
            functions[node.get("name", "")] = node.get("type", {}).get("qualType", "")
        elif kind == "RecordDecl" and node.get("completeDefinition") and node.get("name"):
            records_seen.append((node.get("tagUsed", "struct"), node["name"]))
    return constants, typedefs, functions, records_seen


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", required=True)
    parser.add_argument("--out", default="-")
    parser.add_argument("--refshim", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "refshim"))
    parser.add_argument("headers", nargs="+")
    args = parser.parse_args()

    work = tempfile.mkdtemp(prefix="agora-api-")
    try:
        stage_dir = os.path.join(work, "stage")
        os.makedirs(stage_dir)
        count = stage(args.sdk, stage_dir)
        prefix = os.path.join(stage_dir, "")  # paths in the AST are as clang was given them

        unit = os.path.join(work, "unit.c")
        with open(unit, "w") as handle:
            for header in args.headers:
                handle.write("#include <%s>\n" % header)

        common = [CLANG, "-fsyntax-only", "-fblocks", "-std=gnu11", "-I", args.refshim, "-I", stage_dir]
        tree = json.loads(run(common + ["-Xclang", "-ast-dump=json", unit]).stdout)
        constants, typedefs, functions, seen = ast_facts(tree, prefix)

        # Pass two: name one variable of every record the headers declare, so clang lays them out.
        layout_unit = os.path.join(work, "layout.c")
        with open(layout_unit, "w") as handle:
            for header in args.headers:
                handle.write("#include <%s>\n" % header)
            handle.write("\n")
            for index, (tag, name) in enumerate(sorted(set(seen))):
                handle.write("%s %s agora_layout_%d;\n" % (tag, name, index))
        dumped = run(common + ["-Xclang", "-fdump-record-layouts-complete", layout_unit])
        layout_text = dumped.stdout + dumped.stderr  # this clang writes the dump to stdout
        found = records(layout_text)

        lines = [
            "# libagora-audio — API manifest, macOS 10.12",
            "#",
            "# GENERATED by tools/api-manifest.py from a MacOSX10.12.sdk; DO NOT EDIT.",
            "# It is the facts this repository's declarations are written from and checked against:",
            "# record size, alignment and field offsets, enumeration values, typedefs, prototypes.",
            "# The SDK itself is never committed — only measurements of it.",
            "#",
            "# headers: " + " ".join(args.headers),
            "# framework headers staged: %d" % count,
            "",
        ]
        for name in sorted(found):
            record = found[name]
            lines.append("[record] " + name)
            lines.append("  sizeof %d" % record["size"])
            lines.append("  align %d" % record["align"])
            for offset, type_name, field_name in record["fields"]:
                lines.append("  field %d %s %s" % (offset, field_name, type_name))
            lines.append("")
        for name in sorted(constants):
            lines.append("[constant] %s = %s" % (name, constants[name]))
        lines.append("")
        for name in sorted(typedefs):
            lines.append("[typedef] %s = %s" % (name, typedefs[name]))
        lines.append("")
        for name in sorted(functions):
            lines.append("[function] %s = %s" % (name, functions[name]))
        lines.append("")

        text = "\n".join(lines)
        if args.out == "-":
            sys.stdout.write(text)
        else:
            os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
            with open(args.out, "w") as handle:
                handle.write(text)
            sys.stderr.write("api-manifest: %d records, %d constants, %d typedefs, %d functions -> %s\n" % (
                len(found), len(constants), len(typedefs), len(functions), args.out))
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
