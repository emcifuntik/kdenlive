#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kdenlive contributors
# SPDX-License-Identifier: BSD-2-Clause
"""Start the packaged editor and verify its automatically enabled MCP endpoint."""

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "dev-docs" / "examples"))
from mcp_client import McpClient  # noqa: E402


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--log", required=True, type=Path)
    parser.add_argument("--craft-root", type=Path)
    args = parser.parse_args()
    executable = args.executable.resolve(strict=True)
    args.log.parent.mkdir(parents=True, exist_ok=True)
    environment = os.environ.copy()
    if args.craft_root:
        craft = str(args.craft_root.resolve()).casefold()
        environment["PATH"] = os.pathsep.join(
            entry for entry in environment.get("PATH", "").split(os.pathsep)
            if not os.path.abspath(entry).casefold().startswith(craft + os.sep)
            and os.path.abspath(entry).casefold() != craft
        )
    for key in list(environment):
        if key.startswith(("QT_", "QML", "MLT_", "KDE", "CRAFT", "XDG_")):
            environment.pop(key)
    # CI runners have no audio device: the monitor's sdl2_audio consumer would fail and Kdenlive would block
    # on a modal "Could not create the video preview window" error before starting the MCP server.
    environment.update(QT_QPA_PLATFORM="offscreen", QT_FORCE_STDERR_LOGGING="1", SDL_AUDIODRIVER="dummy")

    with tempfile.TemporaryDirectory(prefix="kdenlive-mcp-smoke-") as profile:
        # First-run configuration and caches must not affect the runner's profile.
        environment.update(APPDATA=profile, LOCALAPPDATA=profile, XDG_CONFIG_HOME=profile,
                           XDG_CACHE_HOME=profile, XDG_DATA_HOME=profile)
        with args.log.open("wb") as log:
            process = subprocess.Popen(
                [str(executable), "--no-welcome"], cwd=executable.parent, env=environment,
                stdout=log, stderr=subprocess.STDOUT,
                creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
            )
            try:
                deadline = time.monotonic() + 120
                while time.monotonic() < deadline:
                    if process.poll() is not None:
                        raise RuntimeError(f"Kdenlive exited during startup: {process.returncode}")
                    output = args.log.read_text(encoding="utf-8", errors="replace")
                    match = re.search(r"MCP HTTP endpoint: (http://127\.0\.0\.1:\d+/mcp)", output)
                    if match:
                        with McpClient(match[1], timeout=20) as client:
                            names = {tool["name"] for tool in client.request("tools/list")["tools"]}
                            required = {"project_info", "profiles_list", "clip_insert", "render_start",
                                        "timeline_frame", "subtitle_add", "asset_parameters_set"}
                            if not required <= names:
                                raise RuntimeError(f"Missing editing tools: {sorted(required - names)}")
                            result = client.call("profiles_list")
                            if result.get("isError"):
                                raise RuntimeError(f"Could not inspect installed profiles: {result}")
                        print(f"Packaged MCP startup, discovery and profiles: PASS ({len(names)} tools)")
                        return
                    time.sleep(0.5)
                raise TimeoutError("The packaged editor did not start its MCP endpoint within 120 seconds")
            finally:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=10)


if __name__ == "__main__":
    main()
