# Optimizer

A Windows game optimizer that detects when you launch a game and applies targeted system tweaks to improve performance. When the game closes, everything is restored exactly as it was.

---

## What it does

When a game is detected, Optimizer applies a set of tweaks based on your configured mode (Safe, Balanced, or Aggressive). Everything is backed up before being changed and fully restored when the game exits.

**CPU**
- Priority boost, timer resolution (0.5ms), CPU unparking, C-state disable, NUMA affinity, MMCSS scheduling, I/O priority, CPU set isolation

**Memory**
- Standby list flush, working set trim, RAM cleaner (periodic), memory compression toggle

**Network**
- Nagle disable, QoS reserve removal, RSS, interrupt moderation, adapter buffer tuning, DSCP tagging, flow control, DNS flush

**GPU**
- DWM priority boost, GPU process priority, fullscreen optimization disable, EcoQoS disable, HAGS check, GPU max power mode

**System**
- Service suspend (SysMain, WSearch, DiagTrack), Xbox Game Bar disable, USB power saving disable, audio low-latency mode, disk write cache, Defender exclusion, visual effects disable

---

## Setup

1. Run `optimizer.exe` as administrator

On first run, config files are created at `%LOCALAPPDATA%\Optimizer\`.

---

## Configuration

All settings live in `%LOCALAPPDATA%\Optimizer\`:

| File | Purpose |
|---|---|
| `config.json` | Global settings, mode, feature toggles |
| `games.json` | Per-game config (mode overrides, RAM cleaner, preserve groups) |
| `close_apps.json` | Apps to kill when a game launches |
| `reopen_apps.json` | Apps to relaunch when the game closes |
| `process_groups.json` | Launcher process groups to preserve |
| `presets.json` | Custom optimization presets |

---

## Modes

| Mode | Description |
|---|---|
| `safe` | Minimal changes, most compatible |
| `balanced` | Recommended for most setups |
| `aggressive` | Maximum performance, more invasive tweaks |

Set globally in `config.json` or per-game in `games.json`.

---

## Commands

Type `help` in the console for a full list. Some common ones:

```
detection on/off       toggle auto game detection
pause / resume         pause optimizer without closing
mode <name>            change global mode on the fly
addgame <exe>          add a game manually
whitelist add <exe>    exclude an app from auto-detection
lastsession            show last session stats
update                 check for and apply updates
```

---

## Notes

- Optimizer runs at `BELOW_NORMAL` priority so it never competes with the game
- The restore thread runs at `BELOW_NORMAL` priority and blocks until complete before the next game can be detected
- Closing the window triggers a full restore — no tweaks are left behind
- Dry run mode (`dry_run: true` in config) lets you preview what would be applied without changing anything
