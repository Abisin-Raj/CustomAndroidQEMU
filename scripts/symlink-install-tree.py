#!/usr/bin/env python3

from pathlib import PurePath
import errno
import json
import os
import shlex
import shutil
import subprocess
import sys

def destdir_join(d1: str, d2: str) -> str:
    if not d1:
        return d2
    # c:\destdir + c:\prefix must produce c:\destdir\prefix
    return str(PurePath(d1, *PurePath(d2).parts[1:]))

try:
    introspect = os.environ.get('MESONINTROSPECT', '')
    if introspect:
        out = subprocess.run(shlex.split(introspect) + ['--installed'],
                             stdout=subprocess.PIPE, check=True).stdout
        for source, dest in json.loads(out).items():
            bundle_dest = destdir_join('qemu-bundle', dest)
            path = os.path.dirname(bundle_dest)
            try:
                os.makedirs(path, exist_ok=True)
            except BaseException:
                pass
            try:
                if os.path.exists(bundle_dest):
                    if os.path.isdir(bundle_dest):
                        shutil.rmtree(bundle_dest)
                    else:
                        os.remove(bundle_dest)
                os.symlink(source, bundle_dest)
            except BaseException:
                try:
                    if os.path.isdir(source):
                        shutil.copytree(source, bundle_dest, dirs_exist_ok=True)
                    else:
                        shutil.copy2(source, bundle_dest)
                except BaseException:
                    pass
except Exception:
    sys.exit(0)
