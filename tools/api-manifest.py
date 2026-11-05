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
typedef, and the qualified type of every function — plus, for the Objective-C doors, every class,
its superclass and protocols, and every method and property it declares. It is committed. The SDK
is not — an SDK is Apple's text, a manifest is measurements.

WHY THIS RUNS ON LINUX: clang parses Apple's headers given three shims (`tools/refshim/`) and a
copy of the header tree with Darwin's nullability keywords spelled the way clang spells them
everywhere else. That copy is made in a scratch directory; the SDK is only ever read.

ONE LANGUAGE, TWO READINGS: the unit is compiled as Objective-C even when it only includes C
headers, because a C header parses identically that way and `AUParameters.h` cannot be read any
other way. `-fobjc-runtime=macosx-10.12` is what makes `__OBJC2__` true, which is the `#if` the
AUv3 headers hide inside; nothing here is ever generated or run.

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

# The Objective-C reading. `-fobjc-runtime=macosx-10.12` is a parse-time fact only: it defines
# `__OBJC2__`, which is the guard both AUv3 headers sit inside.
OBJC_FLAGS = ("-x", "objective-c", "-fobjc-runtime=macosx-10.12", "-fblocks")

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


PROPERTY_ATTRIBUTES = ("readonly", "readwrite", "copy", "retain", "assign", "weak",
                       "unsafe_unretained", "nonatomic", "atomic", "nullability")


def objc_facts(tree, prefix):
    """Classes, protocols, methods and properties, from declarations inside the staged SDK.

    A property's accessors are `ObjCMethodDecl`s too, marked `isImplicit`; recording both would make
    the record say the same thing twice and would let a header drop `readonly` without the record
    noticing, so the accessors are the ones skipped and the property carries its attributes.

    The enclosing class is carried down the recursion because clang's JSON nests a method under its
    interface rather than naming the class on the method. A category is reported under the class it
    extends, which is where a client writes the selectors.
    """
    classes, protocols, methods, properties = {}, {}, {}, {}
    categories = {}

    # clang's JSON omits `file` whenever a node repeats the PREVIOUS node's file, so the file a
    # declaration came from is only knowable by carrying it across the whole dump in source order —
    # `walk` does exactly that, and its answer is indexed here by node so the recursion below can
    # descend with the enclosing class and still know where each node really lives.
    files = {id(node): path for path, node in walk(tree, None)}

    def descend(node, klass):
        path = files.get(id(node))
        here = path is not None and path.startswith(prefix)
        kind = node.get("kind")

        if here and kind in ("ObjCInterfaceDecl", "ObjCCategoryDecl"):
            if kind == "ObjCCategoryDecl":
                klass = (node.get("interface") or {}).get("name") or klass
                if klass:
                    categories.setdefault(klass, []).append(node.get("name") or "")
            else:
                name = node.get("name")
                # `@class Foo;` parses as an interface whose superclass is an empty reference and
                # which has no body: it declares nothing, so it is a name a signature may use rather
                # than part of the surface.
                if name and (node.get("super") or {}).get("name"):
                    names = [entry.get("name") for entry in node.get("protocols") or []]
                    classes[name] = ((node.get("super") or {}).get("name"),
                                     [entry for entry in names if entry])
                    klass = name
        elif here and kind == "ObjCProtocolDecl" and node.get("name"):
            protocols[node["name"]] = True

        if here and klass is not None:
            if kind == "ObjCMethodDecl" and not node.get("isImplicit") and node.get("name"):
                parameters = [child.get("type", {}).get("qualType", "")
                              for child in node.get("inner") or []
                              if child.get("kind") == "ParmVarDecl"]
                key = (klass, "+" if not node.get("instance") else "-", node["name"])
                methods[key] = (node.get("returnType", {}).get("qualType", ""), parameters)
            elif kind == "ObjCPropertyDecl" and node.get("name"):
                attributes = [name for name in PROPERTY_ATTRIBUTES if node.get(name)]
                properties[(klass, node["name"])] = (node.get("type", {}).get("qualType", ""),
                                                     sorted(attributes))

        for child in node.get("inner") or []:
            descend(child, klass)

    descend(tree, None)
    return classes, protocols, categories, methods, properties


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

        common = [CLANG, "-fsyntax-only"] + list(OBJC_FLAGS) + ["-std=gnu11", "-I", args.refshim, "-I", stage_dir]
        tree = json.loads(run(common + ["-Xclang", "-ast-dump=json", unit]).stdout)
        constants, typedefs, functions, seen = ast_facts(tree, prefix)
        classes, protocols, categories, methods, properties = objc_facts(tree, prefix)

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
        for name in sorted(classes):
            superclass, conforms = classes[name]
            lines.append("[objc-class] %s : %s%s" % (
                name, superclass or "",
                (" <" + ", ".join(conforms) + ">") if conforms else ""))
        for name in sorted(protocols):
            lines.append("[objc-protocol] %s" % name)
        for name in sorted(categories):
            for category in sorted(set(categories[name])):
                lines.append("[objc-category] %s (%s)" % (name, category))
        for key in sorted(properties):
            klass, name = key
            qual_type, attributes = properties[key]
            lines.append("[objc-property] %s %s = %s %s" % (
                klass, name, qual_type, ",".join(attributes)))
        for key in sorted(methods):
            klass, sign, selector = key
            qual_type, parameters = methods[key]
            lines.append("[objc-method] %s %s%s = %s (%s)" % (
                klass, sign, selector, qual_type, ", ".join(parameters)))
        lines.append("")

        text = "\n".join(lines)
        if args.out == "-":
            sys.stdout.write(text)
        else:
            os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
            with open(args.out, "w") as handle:
                handle.write(text)
            sys.stderr.write("api-manifest: %d records, %d constants, %d typedefs, %d functions, "
                             "%d objc classes, %d objc methods -> %s\n" % (
                                 len(found), len(constants), len(typedefs), len(functions),
                                 len(classes), len(methods), args.out))
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
