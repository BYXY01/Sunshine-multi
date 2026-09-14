# Session Groups (Window Capture)

This fork adds multi-session window capture: one or more **session groups**
group windows together, and each group is streamed over its own Moonlight
session with its own capture thread. Different applications (or the same
application for different users) can be captured and streamed independently
and in parallel.

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

### Group fields

| Field | Description |
|---|---|
| `name` | Unique group name. |
| `capture` | `window` (window compositing backend) or `monitor` (stock monitor capture). |
| `rules` | Window matching rules; a window joins the group when **any** rule matches (OR). |
| `aux_include` | Reserved. Auxiliary window classes are included by default. |
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

## Window capture backends

Window capture is available with both hardware and software encoders:
hardware encoders use the GPU-backed window backend, where frames stay in
video memory and feed NVENC directly (no GPU→CPU round trip); software
encoders use the RAM backend.

## Command line

| Option | Description |
|---|---|
| `--group <name>` | Name of a single session group. |
| `--capture <backend>` | Capture backend: `window` or `monitor`. |
| `--box <name>` | Process-group container name matching rule. |
| `--process <name>` | Process name matching rule. |
| `--title <pattern>` | Window title matching rule. |
| `--class <name>` | Window class name matching rule. |
| `--hwnd <handle>` | Direct window handle (decimal or `0x` hex). |
| `--port <number>` | Moonlight port for the session group. |
| `--config <path>` | Load session groups from a JSON file. |

Example:

```
sunshine --group user1-notepad --capture window --process notepad.exe
```
