#!/usr/bin/env python3
"""Fetch the pinned sources the build needs into third_party/."""

import argparse
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent.parent


def git(directory, *args, capture=False, check=True):
    return subprocess.run(["git", "-C", str(directory), *args], check=check,
                          capture_output=capture, text=True)


def main():
    dependencies = json.loads((ROOT / "dependencies.lock.json").read_text())
    player_manifest_path = ROOT / "player-source-manifest.json"
    player_manifest = {}
    if player_manifest_path.is_file():
        try:
            player_manifest = json.loads(player_manifest_path.read_text(encoding="utf-8-sig"))
        except (OSError, json.JSONDecodeError) as exc:
            raise SystemExit(f"Invalid Player source manifest: {exc}")

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("names", nargs="*", help="Default: all of dependencies.lock.json")
    args = parser.parse_args()
    selected = args.names or list(dependencies)
    for name in selected:
        if name not in dependencies:
            parser.error(f"Unknown dependency: {name}")
    for name in selected:
        dependency = dependencies[name]
        directory = ROOT / "third_party" / name

        # Public Player ZIPs intentionally strip nested .git metadata. They carry
        # a manifest written by release-player.ps1 that records which pinned
        # dependency revisions were copied into the source snapshot. Accept that
        # trusted vendored snapshot instead of trying "git rev-parse" in a folder
        # that is no longer a Git repository.
        if directory.exists() and not (directory / ".git").exists():
            manifest_revision = (
                player_manifest.get("dependencies", {}).get(name)
                if isinstance(player_manifest, dict) else None
            )
            sentinels = {
                "ZArchive": directory / "CMakeLists.txt",
                "addrlib": directory / "include" / "addrinterface.h",
            }
            sentinel = sentinels.get(name, directory / "CMakeLists.txt")
            if manifest_revision == dependency["revision"] and sentinel.exists():
                print(f"{name}: {manifest_revision} (vendored Player snapshot)")
                continue
            parser.exit(
                1,
                f"Refusing unverified vendored dependency {directory}: "
                f"expected manifest revision {dependency['revision']}\n",
            )

        if not directory.exists():
            directory.mkdir(parents=True)
            git(directory, "init", "--quiet")
            git(directory, "remote", "add", "origin", dependency["url"])
            git(directory, "fetch", "--depth", "1", "origin", dependency["revision"])
            git(directory, "checkout", "--detach", "FETCH_HEAD")
        actual = git(directory, "rev-parse", "HEAD", capture=True).stdout.strip()
        if actual != dependency["revision"]:
            parser.exit(1, f"Refusing to change existing {directory}: expected {dependency['revision']}, got {actual}\n")
        print(f"{name}: {actual}")


if __name__ == "__main__":
    main()
