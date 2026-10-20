#!/usr/bin/env python3
"""Brings d4r's command-hooks module up to date.

    python update_d4r_commands.py <d4r checkout> [--minhook DIR]

d4r (https://github.com/countervolts/d4r, GPL-3.0) splits a game's command list at the point where a
foreign GPU workload runs: everything recorded before it is submitted, the workload runs on the queue,
and what was recorded after it follows. d4r_commands/ holds the module that does that, copied as it is;
only d3d12_external.h is ours (a reduced stand-in for d4r's, which drags the whole of its HIP layer
along). This script copies the module's files over from a checkout of d4r's windows branch and prints
what changed, so that d4r's fixes come in with one command and a look at the diff.

MinHook, which the module needs, is copied from d4r's external/MinHook into external/MinHook of this
repository when --minhook is not given a different source.
"""
import argparse
import filecmp
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FILES = ["d3d12_command_hooks.cpp", "d3d12_command_hooks.h", "d3d12_command_hooks.generated.h",
         "d3d12_newer_commands.h", "d3d12_retirement.h"]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("d4r", help="a d4r checkout (the windows branch)")
    ap.add_argument("--minhook", help="MinHook's source tree, instead of the one in d4r's external/")
    args = ap.parse_args()
    source = os.path.join(args.d4r, "tools", "windows")
    if not os.path.isdir(source):
        raise SystemExit("no tools/windows in %s" % args.d4r)
    changed = 0
    for name in FILES:
        src, dst = os.path.join(source, name), os.path.join(HERE, "d4r_commands", name)
        if not os.path.isfile(src):
            raise SystemExit("d4r no longer has %s: the module has moved, and this script with it" % name)
        if not os.path.isfile(dst) or not filecmp.cmp(src, dst, shallow=False):
            changed += 1
            print("updated", name)
        shutil.copyfile(src, dst)
    minhook = args.minhook or os.path.join(args.d4r, "external", "MinHook")
    target = os.path.abspath(os.path.join(HERE, "..", "..", "external", "MinHook"))
    if os.path.isdir(minhook):
        for part in ("include", "src"):
            if os.path.isdir(os.path.join(target, part)):
                shutil.rmtree(os.path.join(target, part))
            shutil.copytree(os.path.join(minhook, part), os.path.join(target, part))
        print("MinHook copied from", minhook)
    print("%d file(s) changed; review the diff, and the module's own includes against d3d12_external.h." % changed)


if __name__ == "__main__":
    sys.exit(main())
