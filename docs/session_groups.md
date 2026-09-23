# Session Groups (Window Capture)

This fork adds multi-session window capture. A **session group** is the
isolation boundary (one capture thread, and in `per-group-port` mode one RTSP
port); it holds one or more **sessions**. A **session** is one independent
stream picture: the set of windows it matches (the main window plus its
menus/dialogs) is composited, by real Z-order, into that session's own frame.
The session identifier is the Moonlight `appid`, which is how a client selects
which application to stream.

```
group A ── capture thread A ──► session A1 (appid 1) ──► RTSP/session
                            └──► session A2 (appid 2) ──► RTSP/session
group B ── capture thread B ──► session B1 (appid 1) ──► RTSP/session
```

## Configuration

Session groups are configured either through a JSON file
(`session-groups.json`) or through command-line options. The command line
overrides the file. The configuration is only a **preset template**: the same
groups and sessions can also be created and changed at runtime through the
control channel.

```json
{
  "port_mode": "per-group-port",
  "port_range": "48100-48110",
  "default_group": "user1",
  "session_groups": [
    {
      "name": "user1",
      "capture": "window",
      "max_fps": 60,
      "bitrate_kbps": 20000,
      "sessions": [
        {
          "id": 1,
          "name": "notepad",
          "rules": [
            { "process": "notepad.exe" }
          ],
          "aux_exclude": ["tooltips_class32", "IME", "MSCTFIME UI"]
        },
        {
          "id": 2,
          "name": "paint",
          "rules": [
            { "process": "mspaint.exe" }
          ]
        }
      ]
    },
    {
      "name": "user2",
      "capture": "window",
      "sessions": [
        { "id": 1, "name": "chrome", "rules": [{ "process": "chrome.exe" }] }
      ]
    }
  ]
}
```

### Top-level fields

| Field | Description |
|---|---|
| `port_mode` | **Required.** `single-port` (one shared RTSP port) or `per-group-port` (one port per group). |
| `port_range` | **Required for `per-group-port`.** Inclusive port range, e.g. `"48100-48110"`. The number of ports bounds the maximum number of groups. The range **must not include** the reserved Sunshine service ports (HTTP 47989, HTTPS 47984, Web UI HTTPS 47990, RTSP 48010) — the startup validation rejects such ranges. |
| `default_group` | Group that receives sessions without an explicit group. In `per-group-port` mode it takes the first port of the range. |
| `session_groups` | Ordered list of group definitions. |

### Group fields

| Field | Description |
|---|---|
| `name` | Unique group name. |
| `capture` | `window` (window compositing backend) or `monitor` (stock monitor capture). |
| `max_fps` | Maximum capture framerate. |
| `bitrate_kbps` | Stream bitrate in kbps; 0 lets the client decide. |
| `sessions` | Ordered list of sessions served by this group. A `window` group must define at least one session. |

### Session fields

| Field | Description |
|---|---|
| `id` | **Required, non-zero.** Session identifier; equals the Moonlight `appid` used to route a client to this session. Unique within the group. |
| `name` | Human-readable, client-facing session name. Unique within the group. |
| `rules` | Window matching rules for this session; a window joins the session when **any** rule matches (OR). |
| `aux_exclude` | Window classes filtered from this session's frame (e.g. tooltips, IME). |
| `hwnd` | Direct window handle; overrides all rules when non-zero. |

### Window matching rules

A rule matches when any of its non-empty fields match the window:

| Field | Match semantics |
|---|---|
| `box` | Process-group container name (accepted for configuration compatibility; not evaluated). |
| `process` | Process executable name (e.g. `notepad.exe`). |
| `title` | Substring of the window title. |
| `class` | Window class name. |
| `hwnd` | Direct window handle. |

## Port modes

### single-port

All groups share the official RTSP port (`47989`). Sessions are routed to a
group by name: the launch request's `group` argument, then the configured
`default_group`, then the lone window group if there is exactly one.

### per-group-port

Each group gets its own dedicated RTSP port, allocated in ascending order from
`port_range`. The configured `default_group` takes the first port; the
remaining groups follow in configuration order. Binding each group logs its
port:

```
Session group [user1] listening on RTSP port 48100
```

A group whose port cannot be allocated (the range is exhausted) is refused
with an error instead of crashing.

## Window capture and popup composition

When `capture` is `window`, a session binds to the process owning its matched
window and captures the session's whole window set: the main window plus the
application's own menus (`#32768`), dialogs (`#32770`), and other visible
top-level windows, composited by real Z-order so menus and dialogs appear
above the main window. Windows whose class is listed in the session's
`aux_exclude` are filtered out.

Hardware encoders use the GPU-backed window backend; software encoders use
the RAM backend. The RAM backend composites the window set on the CPU. The GPU
backend composites on the GPU by default: the main window frame is copied into
the capture texture and each auxiliary window is drawn over it at its
screen-relative offset with alpha blending, so no CPU round trip is needed. If
GPU composition fails, the backend falls back to compositing on the CPU and
uploading the result.

A client selects a session by its `appid`; the capture backend resolves that
session within the group and composes the windows matching the session's rules
(regardless of the owning process), so the session's own menus, dialogs and
other windows appear in its picture while other sessions stay isolated.

> The per-session **canvas** is currently the main window's rectangle; the
> target-resolution black canvas planned for the multi-process layout stage is
> not implemented yet.

## Command line

| Option | Description |
|---|---|
| `--port-mode <mode>` | Global port mode: `single-port` or `per-group-port` (required). |
| `--port-range <range>` | Per-group port range, e.g. `48100-48110` (must avoid the reserved service ports 47984/47989/47990/48010). |
| `--default-group <name>` | Group receiving sessions without an explicit group. |
| `--group <name>` | Name of a single session group (and of its single session). |
| `--capture <backend>` | Capture backend: `window` or `monitor`. |
| `--box <name>` | Process-group container name matching rule. |
| `--process <name>` | Process name matching rule. |
| `--title <pattern>` | Window title matching rule. |
| `--class <name>` | Window class name matching rule. |
| `--hwnd <handle>` | Direct window handle (decimal or `0x` hex). |
| `--port <number>` | Moonlight port for the session group. |
| `--config <path>` | Load session groups from a JSON file. |

Examples:

```
sunshine --port-mode single-port --group user1 --capture window --process notepad.exe
sunshine --port-mode per-group-port --port-range 48100-48110 --config session-groups.json
```
