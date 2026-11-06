#!/usr/bin/env python3
# Copyright © 2026 Kyle J. Cardoza.
# SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Fetch a MacOSX10.12.sdk-shaped header tree for tools/api-manifest.py.

    tools/fetch-sdk.py --out /tmp/refsdk
    tools/api-manifest.py --sdk /tmp/refsdk --scratch /tmp/agora-api-stage --out tests/api/manifest-10.12.txt ...

**THE SDK IS NEVER COMMITTED, AND THAT IS WHY THIS EXISTS.** `tests/api/manifest-10.12.txt` is the measurements; Apple's
text is not. But a manifest can only be regenerated from Apple's text, and the tree it comes from is 1.7 MB of headers
that nobody keeps between reboots — so the procedure lives here as a command instead of in a handoff's prose, where it
was lost once already.

The source is `phracker/MacOSX-SDKs` on GitHub: the *contents* API lists a framework's `Versions/A/Headers` directory
and the raw tree serves each file. Four frameworks, because `AudioToolbox/MusicPlayer.h` includes
`CoreMIDI/MIDIServices.h`: CoreAudio (9 files), AudioToolbox (30), AudioUnit (16), CoreMIDI (5). `CoreAudioTypes` has no
framework directory of its own in that SDK — the file lives in CoreAudio's, which is where the generator looks.

`--out` is created if it does not exist and its tree is replaced per framework, so a second run is a refresh rather
than a merge: a header Apple removed from the SDK should not survive in the copy.

**GITHUB RATE-LIMITS RAW DOWNLOADS**, about sixty an hour unauthenticated, and the failure is a `404` rather than a
`429` — which looks exactly like "this SDK does not have that header". MEASURED: the first fetch of all four frameworks
succeeded, and a second one an hour later answered 404 for every file. So a framework's files are held in memory and
written only once all of them arrived: a throttled run leaves the tree it was pointed at untouched instead of emptying
it. If this fails, wait, or point `--out` at a tree that is already staged.
"""

import argparse
import json
import os
import shutil
import sys
import urllib.request

REPOSITORY = "phracker/MacOSX-SDKs"
SDK = "MacOSX10.12.sdk"
RAW = "https://raw.githubusercontent.com/%s/%s" % (REPOSITORY, SDK)
CONTENTS = ("https://api.github.com/repos/%s/contents/%s/System/Library/Frameworks/%%s.framework/Versions/A/Headers"
            % (REPOSITORY, SDK))
FRAMEWORKS = ("CoreAudio", "AudioToolbox", "AudioUnit", "CoreMIDI")


def get(url):
    with urllib.request.urlopen(url, timeout=60) as response:
        return response.read()


def fetch_framework(framework, destination):
    """Copy one framework's headers in, and answer with how many files that was.

    Nothing is written until everything has arrived: a run that is throttled half way must not leave a directory that
    looks staged. The whole SDK is under two megabytes, so holding it is the cheap way to be able to say "this copy is
    complete".
    """
    try:
        listing = json.loads(get(CONTENTS % framework))
    except Exception:
        return 0        # not every framework has a Versions/A/Headers directory in this SDK

    files = {}
    for entry in listing:
        if entry["type"] != "file":
            continue
        url = "%s/System/Library/Frameworks/%s.framework/Versions/A/Headers/%s" % (RAW, framework, entry["name"])
        try:
            files[entry["name"]] = get(url)
        except Exception as exc:
            sys.stderr.write("fetch-sdk: %s: %s\n" % (url, exc))
            return 0

    directory = os.path.join(destination, framework + ".framework/Versions/A/Headers")
    if os.path.isdir(directory):
        shutil.rmtree(directory)
    os.makedirs(directory)

    for name, body in files.items():
        with open(os.path.join(directory, name), "wb") as handle:
            handle.write(body)

    return len(files)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", required=True, help="where to stage the tree, e.g. /tmp/refsdk")
    args = parser.parse_args()

    destination = os.path.join(args.out, "System/Library/Frameworks")
    os.makedirs(destination, exist_ok=True)

    total = 0
    for framework in FRAMEWORKS:
        count = fetch_framework(framework, destination)
        total += count
        sys.stderr.write("fetch-sdk: %-12s %d headers\n" % (framework, count))

    sys.stderr.write("fetch-sdk: %d headers under %s\n" % (total, args.out))
    if total == 0:
        sys.stderr.write("fetch-sdk: nothing was fetched — GitHub throttles raw downloads, and a throttled request "
                         "answers 404 rather than 429\n")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
