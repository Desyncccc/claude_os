#!/usr/bin/env python3
"""Write release.json and a .sha256 file next to the ISO published on the website.

    python3 tools/mkrelease.py build/radiant-x64.iso docs/download
"""
import datetime
import hashlib
import json
import os
import re
import sys

iso, outdir = sys.argv[1], sys.argv[2]
data = open(iso, "rb").read()
digest = hashlib.sha256(data).hexdigest()
header = open(os.path.join(os.path.dirname(__file__), "..", "kernel", "include", "ntdef.h")).read()
version = re.search(r'#define RADIANT_VERSION_STR\s+"([^"]+)"', header).group(1)
codename = re.search(r'#define RADIANT_CODENAME\s+"([^"]+)"', header).group(1)

release = {
    "version": version,
    "codename": codename,
    "file": "radiant-x64.iso",
    "bytes": len(data),
    "sha256": digest,
    "built": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d"),
}
with open(os.path.join(outdir, "release.json"), "w") as f:
    json.dump(release, f, indent=2)
    f.write("\n")
with open(os.path.join(outdir, "radiant-x64.iso.sha256"), "w") as f:
    f.write(f"{digest}  radiant-x64.iso\n")
print(json.dumps(release, indent=2))
