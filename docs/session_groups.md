# Session Groups (Window Capture)

This fork adds multi-session window capture: one or more **session groups**
group windows together, and each group is streamed over its own RTSP port /
Moonlight session with its own capture thread. Different applications (or
the same application for different users) can be captured and streamed
independently and in parallel.

```
group A (window capture) ── capture thread A ── display_t (compositing) ──► RTSP/session A
group B (window capture) ── capture thread B ── display_t (compositing) ──► RTSP/session B
```

## Configuration

Session groups are configured either through a JSON file
(`session-groups.json`) or through command-line options. The command line
overrides the file.

```json
{
  "port_mode": "per-group-port",
  "port_range": "48010-48100",
  "default_group": "user1-notepad",
  "session_groups": [
    {
      "name": "user1-notepad",
      "capture": "window",
      "rules": [
        { "process": "notepad.exe" }
      ],
      "aux_exclude": ["tooltips_class32", "IME", "MSCTFIME UI"],
      "max_fps": 60,
      "bitrate_kbps": 20000
    },
    {
      "name": "user2-chrome",
      "capture": "window",
      "rules": [
        { "process": "chrome.exe" }
      ]
    }
  ]
}
```

### Top-level fields

| Field | Description |
|---|---|
| `port_mode` | **Required.** `single-port` (one shared RTSP port) or `per-group-port` (one port per group). |
| `port_range` | **Required for `per-group-port`.** Inclusive port range, e.g. `"48010-48100"`. The number of ports bounds the maximum number of window groups. |
| `default_group` | Group that receives sessions without an explicit group. In `per-group-port` mode it takes the first port of the range. |
| `session_groups` | Ordered list of group definitions. |

### Group fields

| Field | Description |
|---|---|
| `name` | Unique group name. |
| `capture` | `window` (window compositing backend) or `monitor` (stock monitor capture). |
| `rules` | Window matching rules; a window joins the group when **any** rule matches (OR). |
| `aux_exclude` | Window classes filtered from the composited frame (e.g. tooltips, IME). |
| `max_fps` | Maximum capture framerate. |
| `bitrate_kbps` | Stream bitrate in kbps; 0 lets the client decide. |

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

Each window group gets its own dedicated RTSP port, allocated in ascending
order from `port_range`. The configured `default_group` takes the first
port; the remaining groups follow in configuration order. Binding each
group logs its port:

```
Session group [user1-notepad] listening on RTSP port 48010
```

A group whose port cannot be allocated (the range is exhausted) is refused
with an error instead of crashing.

## Window capture and popup composition

When `capture` is `window`, the backend binds to the **process** owning the
matched (anchor) window — regardless of which rule matched — and enumerates
all visible top-level windows of that process every frame. Menus, dialogs,
and tooltips opened by the application are composited onto the anchor
window frame at their screen-relative offsets with alpha blending, so the
full application appears in the stream. Windows whose class is listed in
`aux_exclude` are filtered out.

Hardware encoders use the GPU-backed window backend; software encoders use
the RAM backend. The RAM backend composites popups on the CPU. The GPU
backend composites them on the GPU by default: the anchor frame is copied
into the capture texture and each popup is drawn over it at its
screen-relative offset with alpha blending, so no CPU round trip is needed.
If GPU composition fails, the backend falls back to compositing the popups
on the CPU and uploading the result.

## Command line

| Option | Description |
|---|---|
| `--port-mode <mode>` | Global port mode: `single-port` or `per-group-port` (required). |
| `--port-range <range>` | Per-group port range, e.g. `48010-48100`. |
| `--default-group <name>` | Group receiving sessions without an explicit group. |
| `--group <name>` | Name of a single session group. |
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
sunshine --port-mode single-port --group user1-notepad --capture window --process notepad.exe
sunshine --port-mode per-group-port --port-range 48010-48100 --config session-groups.json
```
