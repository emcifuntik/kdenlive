<!-- SPDX-FileCopyrightText: 2026 Kdenlive contributors -->
<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# Embedded MCP server

Kdenlive can expose its open project to an LLM through an embedded C++ HTTP
server. The endpoint is part of the editor process; it operates on the same
timeline, bin, undo stack, effects and render engine as the user interface.
It starts automatically in every GUI session and requires no authentication.

The API covers the editing workflow from source inspection to rendering. It
exposes the entire installed effect/transition catalog and native parameters,
instead of a fixed subset of common effects. `tools/list` is the authoritative
reference for tool names, arguments, units and required fields.

## Start the server

Build Kdenlive normally using [the build instructions](build.md). Qt Network is
the only additional explicitly linked Qt component; no web framework, Python
runtime or MCP daemon is required by the server.

Start Kdenlive normally:

```sh
kdenlive
```

On Windows, launch `kdenlive.exe` normally. The default endpoint is
`http://127.0.0.1:9250/mcp`; no token, environment variable or HTTP authorization
header is needed. If the default port is unavailable, the editor selects a free
port. The actual endpoint URL is printed in the application log on each launch.

Use `--mcp-port PORT` to choose a specific port, or `--mcp-port 0` to select a
free port. An unavailable explicitly requested port fails startup. The server
listens only on IPv4 loopback. It is not started in `--render` or `--verify-file`
mode. The welcome screen remains available; clients can discover profiles with
`profiles_list`, then use `project_new` or `project_open` to begin editing when
no project is open yet.

Configure an MCP client with these connection settings:

| Setting | Value |
| --- | --- |
| Transport | Streamable HTTP |
| URL | `http://127.0.0.1:9250/mcp` |
| Authentication | None |

Configuration field names depend on the client. MCP session/protocol headers
are negotiated during initialization; they are not authentication credentials.

## Command-line client

The [example client](examples/mcp_client.py) uses only Python's standard library.
It initializes a session, sends the initialized notification, preserves the
session/protocol headers and deletes the session when finished.

```sh
python dev-docs/examples/mcp_client.py list
python dev-docs/examples/mcp_client.py call project_info
python dev-docs/examples/mcp_client.py call timeline_get
python dev-docs/examples/mcp_client.py call timeline_frame --arguments '{"frame":0,"width":960}' --image review.png
```

Use `--url http://127.0.0.1:PORT/mcp` for a nondefault port and
`--arguments-file arguments.json` when shell quoting is inconvenient. The
client returns a nonzero exit code for transport/protocol failures and tool
errors. An image destination must be a new file. The client does not start the
editor or send data to a cloud service.

## Editing conventions

- Time arguments are integer **project frames**, including source clip ranges.
  `project_info` reports the exact frame-rate numerator and denominator. Do not
  assume 25/30 FPS or use a source file's native FPS to compute tool arguments.
- Timeline and subtitle ranges use an **exclusive end**. Clip source `out`,
  render `out` and effect-zone `out` are **inclusive**. Each schema states its
  convention. A source clip with `in=10, out=19` contributes 10 frames.
- `bin_id` is a string identifying a source/folder; `item_id` identifies an
  instance on the timeline; `track_id` is a stable model ID, not a row number.
  Refresh state after project switches, undo/redo, cuts and sequence changes.
- Mutations use native undo commands. `history_undo` and `history_redo` share the
  GUI history, including interactive edits. Saving/exporting, background jobs,
  project replacement, monitor/selection changes and sequence activation have
  different semantics described in their tools. A tool call is not a global
  transaction covering several calls.
- Import, proxy generation, scene detection and rendering are asynchronous.
  Poll `bin_get`/`bin_jobs` or `render_status`. Do not insert a source until its
  `ready` flag is true. Background analysis can add markers after it completes.
- `project_new` and `project_open` reject unsaved changes unless
  `discard_changes=true`. File exports require `overwrite=true` to replace an
  existing file. Saving the current document is allowed to replace that document.
- A modal editor dialog makes tool calls return an error until it closes.
  Some existing project-open workflows, such as missing-media repair or project
  migration, may themselves show dialogs. These require user interaction.
- Calls from different sessions and interactive edits share one project. Send
  dependent edits serially and inspect returned state. A nested GUI event loop
  cannot dispatch a second MCP operation during an active operation.

## Coverage

| Area | Tools and supported operations |
| --- | --- |
| Projects | `project_info`, `profiles_list`, `project_new`, `project_open`, `project_save`, `project_xml`, `project_set_notes`; exact profile/FPS, native XML, save-as/copy |
| History and sequences | `history_get`, `history_undo`, `history_redo`, `sequences_list`, `sequence_create`, `sequence_activate`; sequence clips can be nested through normal insertion |
| Media bin | `bin_list`, `bin_get`, `bin_import`, `bin_folder_create`, `bin_rename`, `bin_move`, `bin_delete`, `bin_subclip_create`, `bin_relink`, `bin_proxy`, `bin_jobs`, `bin_jobs_cancel`, `bin_detect_scenes` |
| Generated sources/titles | `generator_color`, `generator_slideshow`, `title_create`, `title_update`, `title_template`; native title XML supports rich text, shapes, images, gradients and viewport animation |
| Inspection and tracks | `timeline_get`, `timeline_item_get`, `track_create`, `track_delete`, `track_rename`, `track_lock`, `track_set_state` |
| Editing | `clip_insert`, `timeline_insert_zone`, `timeline_item_move`, `timeline_item_delete`, `timeline_item_resize`, `clip_cut`, `timeline_cut_all`, `clip_slip`, `clip_duplicate`, `timeline_copy`, `timeline_duplicate_items` |
| Timing and streams | `clip_speed`, `clip_time_remap_get`, `clip_time_remap_set`, `clip_time_remap_clear`, `clip_set_state`, `clip_split_audio`, `clip_split_video` |
| Groups and gaps | `timeline_select`, `group_create`, `group_remove`, `group_get`, `timeline_extract`, `timeline_lift`, `timeline_insert_space`, `timeline_remove_gap`, `timeline_remove_gaps` |
| Transitions | `composition_create`, `composition_set_track`, `mix_create`, `mix_resize`, `mix_remove`; composition and mix parameters use the common asset tools |
| All installed effects | `assets_list`, `assets_describe`, `effects_list`, `effect_add`, `effect_remove`, `effect_move`, `effect_enable`, `effects_enable`, `effect_set_zone`, `effects_copy`, `asset_parameters_get`, `asset_parameters_set`, `asset_sample`, `clip_fade` |
| Animation | `keyframe_types`, `keyframes_get`, `keyframe_add`, `keyframe_update`, `keyframe_remove`, `keyframe_move`; interpolation modes come from the running build |
| Guides/markers | `markers_list`, `marker_set`, `marker_delete`, `marker_move`, `markers_import`, `markers_export`; sequence guides and source markers, including range markers |
| Subtitles | `subtitles_list`, `subtitle_add`, `subtitle_edit`, `subtitle_move`, `subtitle_delete`, `subtitle_cut`, `subtitles_import`, `subtitles_export`, `subtitles_set_state`, `subtitle_layer_create`, `subtitle_layer_delete`, `subtitle_styles_list`, `subtitle_style_set`, `subtitle_assign_style`, `subtitles_force_style` |
| Visual review | `monitor_get`, `monitor_seek`, `monitor_play`, `monitor_set_zone`, `monitor_open_clip`, `monitor_frame`, `bin_frame`, `timeline_frame`; images returned as MCP PNG content |
| Export | `render_presets`, `render_start`, `render_status`, `render_cancel`, `render_forget`; installed presets/custom MLT parameters, ranges, proxies, two-pass, embedded subtitles, image sequences, per-track audio and guide sections |

Effect targets are objects such as `{"kind":"clip","item_id":42}`,
`{"kind":"track","item_id":8}`, `{"kind":"master"}` or
`{"kind":"bin","bin_id":"3"}`. Use `effect_row` for top-level effects and
`effect_path` from `effects_list` for nested effects. Compositions and incoming
mixes use `{"kind":"composition","item_id":42}` and
`{"kind":"mix","item_id":42}` directly with asset parameter/keyframe tools.

Always discover asset IDs and parameter definitions first. The native parameter
interface covers installed audio processing, color correction, transforms,
compositing, masks, custom effects and their animation without a hardcoded list
of filters. Availability still depends on the local MLT/plugins installation;
the API does not install missing plugins or analysis models.

`clip_speed` uses a multiplier: `1` is normal, `2` is twice as fast, `-1` is
reverse. Variable time maps preserve duration, apply to linked AV partners and
use linear segments between output/source frame pairs. Include output frame 0
and `duration-1`. Equal consecutive source values create a freeze. Constant
speed must be normal before applying a variable time map.

`timeline_insert_zone` takes explicit destination tracks and assigns active
audio streams in the supplied order. It does not prompt to create tracks.
`clip_insert` with `stream="av"` uses Kdenlive's mirror-track layout; create
sufficient audio tracks first. All affected tracks must be unlocked. For a
specific multistream layout, use `timeline_insert_zone`.

Rendering captures the active sequence when `render_start` is called. One MCP
render runs at a time; two-pass/section/stem exports execute their steps
sequentially. `render_status.outputs` lists final paths or image-sequence
patterns. Cancellation uses the native renderer IPC; poll until the terminal
state. Finished requires the renderer's success acknowledgement, successful
process exit and nonempty output files. Output files are preserved by
`render_forget`.

Subtitles address the active subtitle file. SRT export clips events to the
requested range, starts the range at timestamp zero and strips ASS override
tags; complete ASS export preserves styling. Native title XML and ASS styles
are available for layouts that would not fit a small fixed text schema.

This is an editing API, not an automatic exposure of every GUI action. It does
not drive dialogs for hardware capture, online providers, speech-model setup,
application preferences or interactive analysis assistants. Tools operating on
native asset parameters do not automatically run those assistants. It also
does not offer arbitrary shell execution, arbitrary Qt method invocation,
remote HTTP binding, resources/prompts, server-initiated requests or SSE.

## Example LLM workflow

1. Inspect `project_info`, `bin_list` and `timeline_get`; discover available
   profiles/effects/presets as needed.
2. Import media, poll until ready, and examine metadata and `bin_frame` images.
3. Create tracks and insert source ranges. Cut, ripple trim, slip, group and
   adjust speed using exact frames. Inspect the resulting timeline after edits.
4. Discover effects, add them to clips/tracks/master, set named native
   parameters and animate them using keyframes. Add transitions and titles.
5. Add/edit/import subtitles and guides. Review multiple `timeline_frame`
   images; use the monitor for interactive playback with the user.
6. Save the project, start a render, and poll its job. Read `state`, `error` and
   the bounded renderer log on failure. Undo editing mistakes before retrying.

## Transport and testing

The endpoint implements JSON-RPC over
[MCP Streamable HTTP](https://modelcontextprotocol.io/specification/2025-11-25/basic/transports).
It negotiates protocol versions `2025-11-25`, `2025-06-18` and `2025-03-26`.
POST requires `Content-Type: application/json`, `Content-Length` and
`Accept: application/json, text/event-stream`. The initialization response
provides `Mcp-Session-Id`; subsequent requests carry it and the negotiated
`Mcp-Protocol-Version`. Send `notifications/initialized` before calling tools.

Successful requests return JSON; accepted notifications return empty HTTP 202.
GET returns 405 because this server does not offer SSE. DELETE terminates a
session. Sessions expire after one hour of inactivity. An unknown/expired
session returns 404, so reinitialize rather than retrying edits blindly.
An HTTP timeout may occur after an edit was applied; inspect state/history
before retrying a mutation.

Limits: 32 sessions, 32 simultaneous HTTP connections, 16 KiB headers, 4 MiB
request bodies, 15-second connection deadline, and 100 retained render jobs.
Header values and JSON tool arguments are validated. Host/Origin checks reject
nonlocal browser origins and DNS rebinding hosts. Chunked/compressed requests
and JSON-RPC batches are unsupported. Connections close after each response.

The standalone transport test needs only Qt Core/Network/Test:

```sh
cmake -S tests/mcp -B build-mcp -G Ninja
cmake --build build-mcp
ctest --test-dir build-mcp --output-on-failure
```

The normal Kdenlive test build also includes `mcptest`: real model operations
cover insertion bounds, collision rollback, strict input validation, cut/trim,
undo/redo, speed conversion, track locks, explicit zone insertion, effect order
and initial-keyframe protection. Run
`ctest --test-dir build -R '^(mcptest|mcptransporttest)$' --output-on-failure`
after building with the normal project test dependencies.
