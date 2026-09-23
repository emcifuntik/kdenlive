<!-- SPDX-FileCopyrightText: 2026 Kdenlive contributors -->
<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# GitHub builds and fork releases

The [Build and release workflow](../.github/workflows/build-release.yml) builds
this fork's Windows x64 application using KDE Craft and MSVC 2022. It runs on
pushes to `master` and `codex/**`, pull requests targeting `master`, tags named
`mcp-v*`, and manual runs from the GitHub Actions page. Linux runs the standalone
MCP transport tests; this workflow does not distribute Linux or macOS packages.

## Build outputs

Every successful build uploads the `kdenlive-mcp-windows-x64` artifact for 14 days:

- An NSIS installer (`*-setup.exe`).
- A portable archive (`*-portable.7z`); extract it and run `bin/kdenlive.exe`.
- `build-info.json` with the source, Craft and blueprint revisions.
- `SHA256SUMS` for the packages and build metadata.

Packages are unsigned. They include the embedded MCP server, which starts
automatically without authentication on IPv4 loopback. See [MCP setup](mcp.md).

Craft supplies Qt, KDE Frameworks, MLT, FFmpeg and the packaging dependencies.
The application uses the checked-out fork sources through `kdenlive.srcDir` and
`--no-cache`, never an upstream Kdenlive binary. Craft and KDE blueprint revisions
are pinned in the workflow; update both when newer dependencies are needed.
KDE's binary cache is used for dependencies. Their initial installation can take
substantial time on a fresh runner.

The workflow builds and runs the standalone HTTP/MCP tests on Linux and Windows.
It also extracts the portable archive, clears Craft's library paths, launches
the packaged editor with an isolated profile, and checks automatic MCP startup,
tool discovery and installed profile access. It does not run the full Kdenlive
timeline/model test suite.

## Publish a release

Push a new `mcp-v*` tag pointing to the commit to release, for example:

```sh
git tag -a mcp-v0.1.0 -m "Kdenlive MCP 0.1.0"
git push origin mcp-v0.1.0
```

Only tag pushes publish releases. Ordinary pushes and manual runs produce
artifacts without creating releases. A tagged run must pass both jobs, including
the portable application smoke test, before the release job verifies checksums
and publishes a GitHub Release with generated notes and all four assets.
An existing release is never overwritten; use a new tag for changed binaries.

No personal access token or signing secret is required. Only the release job has
`contents: write`; build and pull-request jobs have read-only repository access.
If GitHub Actions is disabled in a newly created fork, enable it in the fork's
Actions tab before running the workflow. Organization policies may also restrict
workflow execution or release writes.

## Troubleshooting

Failed Windows jobs upload available Craft logs, test logs and the packaged
application log as `windows-build-diagnostics` for seven days. Start with the
first failed workflow step. Dependency/version failures usually require updating
the pinned Craft/blueprint revisions; packaging failures must be fixed before a
release can be published.

Each Craft stage also writes `build-release/<stage>.log`; failed commands include
their last output lines in the job annotations, including bootstrap failures.
The build script removes external MinGW/MSYS/Cygwin/Strawberry directories from
its process `PATH` before every stage. This prevents a runner-provided native
`make.exe` from shadowing Craft's MSYS make, which understands `/c/...` paths.
The runner's machine-wide environment is not modified.

Upstream references: [KDE Craft setup](https://develop.kde.org/docs/getting-started/building/craft/)
and [Kdenlive's Craft blueprint](https://github.com/KDE/craft-blueprints-kde/blob/master/kde/kdemultimedia/kdenlive/kdenlive.py).
