#!/usr/bin/env python3
"""Put the chasm bridge into an OpenCE checkout.

OpenCE (https://github.com/OpenCommunityEdition/OpenCE) is the Halo: Combat
Evolved decompilation's port to Linux, Windows, Android and the web. The
bridge is:

  opence/port/linux/src/chasm_client.[ch]    new files, copied in
  opence/port/linux/src/chasm_opus.[ch]      new files, copied in
  opence/port/linux/game/chasm_bridge.[ch]   new files, copied in
  opence/patches/opence-chasm-bridge.patch   the hooks in OpenCE's own files
                                             (main loop, HUD, checkpoints,
                                             sound mix, settings, the co-op
                                             messages, the talk to AI key)

made against the OpenCE commit in opence/OPENCE_COMMIT. Without --opence, a
checkout of that commit is cloned into ./OpenCE-build (not ./OpenCE: on
Windows and macOS, that is this repository's opence/); with it, the given checkout
is used as it is (at another commit, the patch may not apply).

    python tools/apply_to_opence.py [--opence DIR] [--copy-profile DIR]

Then build OpenCE as its README says (python configure.py, then ninja), and
put chasm-profile/ in the game's data folder (next to maps/): --copy-profile
does that.
"""

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parent.parent
OVERLAY = REPOSITORY / "opence"
PATCH = OVERLAY / "patches" / "opence-chasm-bridge.patch"
NEW_FILES = [
    "port/linux/src/chasm_client.c",
    "port/linux/src/chasm_client.h",
    "port/linux/src/chasm_opus.c",
    "port/linux/src/chasm_opus.h",
    "port/linux/game/chasm_bridge.c",
    "port/linux/game/chasm_bridge.h",
]
UPSTREAM = "https://github.com/OpenCommunityEdition/OpenCE.git"


def git(*arguments, cwd: Path, check=True) -> subprocess.CompletedProcess:
    result = subprocess.run(["git", *arguments], cwd=cwd, capture_output=True, text=True)
    if check and result.returncode != 0:
        raise SystemExit(f"git {' '.join(arguments)} failed ({result.returncode}):\n{result.stderr}")
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--opence", help="an OpenCE checkout (default: clone the pinned commit into ./OpenCE-build)")
    parser.add_argument("--copy-profile", metavar="DIR",
                        help="also copy chasm-profile/ into DIR (the game's data folder, holding maps/)")
    arguments = parser.parse_args()

    commit = (OVERLAY / "OPENCE_COMMIT").read_text().strip()
    if arguments.opence:
        opence = Path(arguments.opence).resolve()
        if not (opence / "configure.py").exists():
            print(f"{opence} is not an OpenCE checkout", file=sys.stderr)
            return 1
    else:
        opence = (Path.cwd() / "OpenCE-build").resolve()
        if not opence.exists():
            # (that commit alone, shallow; long paths: Windows' 260 characters are too few for some of its files)
            print(f"fetching OpenCE {commit[:12]} into {opence}")
            opence.mkdir(parents=True)
            git("init", "--quiet", cwd=opence)
            git("config", "core.longpaths", "true", cwd=opence)
            git("remote", "add", "origin", UPSTREAM, cwd=opence)
        # (a folder of its own, not one inside this or another repository)
        top = git("rev-parse", "--show-toplevel", cwd=opence, check=False).stdout.strip()
        if not top or Path(top).resolve() != opence:
            print(f"{opence} is not a checkout of its own; remove it and run this again", file=sys.stderr)
            return 1
        # (the commit by its id; where a server will not give one so, main's
        # history, which holds it)
        for fetch in (["--depth", "1", "origin", commit], ["--depth", "500", "origin", "main"], ["--unshallow", "origin"]):
            if git("cat-file", "-e", f"{commit}^{{commit}}", cwd=opence, check=False).returncode == 0:
                break
            result = git("fetch", "--quiet", *fetch, cwd=opence, check=False)
            if result.returncode != 0:
                print(f"git fetch {' '.join(fetch)}: {result.stderr.strip()}", file=sys.stderr)
        git("checkout", "--quiet", "--force", commit, cwd=opence)

    for name in NEW_FILES:
        target = opence / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(OVERLAY / name, target)
        print(f"copied {name}")

    # (the hooks: applied once; already there, left as they are)
    if git("apply", "--reverse", "--check", str(PATCH), cwd=opence, check=False).returncode == 0:
        print("the hooks are already applied")
    else:
        check = git("apply", "--check", str(PATCH), cwd=opence, check=False)
        if check.returncode != 0:
            print(f"the hooks do not apply to this checkout (made against {commit[:12]}):\n{check.stderr}",
                  file=sys.stderr)
            return 1
        git("apply", str(PATCH), cwd=opence)
        print(f"applied {PATCH.name}")

    if arguments.copy_profile:
        destination = Path(arguments.copy_profile) / "chasm-profile"
        shutil.copytree(REPOSITORY / "chasm-profile", destination, dirs_exist_ok=True)
        print(f"copied chasm-profile/ to {destination}")

    print(f"\nnow build it: cd {opence} && python configure.py --release && ninja")
    return 0


if __name__ == "__main__":
    sys.exit(main())
