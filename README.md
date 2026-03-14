# netwatch 🌐

![Build and Test](https://github.com/iameijaz/netwatch/actions/workflows/build-and-test.yml/badge.svg)

A minimal, cross-platform command-line tool that monitors internet connectivity
and alerts you when the connection goes up or down.

No DNS lookups. No ICMP. No root required. Near-zero CPU and memory usage.

## How it works

netwatch opens a non-blocking TCP connection to a well-known IP address
(Google DNS `8.8.8.8`, port 53 by default) with a configurable timeout.
If the connection succeeds — you're online. If it times out or is refused — you're not.

On state change, it prints a timestamped line, optionally sends a system
notification, and optionally runs a user-defined command.

Port 53 (DNS) is used because it is almost never blocked by firewalls,
even on restricted networks.

## Features

- Detects internet up/down transitions instantly
- Dual-host check (primary + fallback) to avoid false positives
- System notifications (`notify-send` / `osascript` / PowerShell)
- Run custom commands on connect/disconnect (`--on-up`, `--on-down`)
- Log events to file
- One-shot mode for use in scripts
- Tracks outage count and total downtime (`--stats`)
- Cross-platform: Linux, macOS, Windows
- Single C file, no dependencies

## Installation

### From source

```bash
git clone https://github.com/iameijaz/netwatch.git
cd netwatch
make
sudo make install
```

### From DEB package (Debian/Ubuntu)

Download the latest `.deb` from [Releases](https://github.com/iameijaz/netwatch/releases):

```bash
sudo dpkg -i netwatch_1.0.0_amd64.deb
```

### Windows

Download `netwatch.exe` from [Releases](https://github.com/iameijaz/netwatch/releases)
and place it anywhere in your `PATH`.

Or build with MinGW:
```bash
gcc -Wall -O2 -std=c99 -o netwatch.exe netwatch.c -lws2_32
```

## Usage

```
netwatch [options]
```

| Option | Default | Description |
|--------|---------|-------------|
| `--host <ip>` | `8.8.8.8` | Primary check host |
| `--port <n>` | `53` | TCP port to probe |
| `-i, --interval <s>` | `5` | Check interval in seconds |
| `-t, --timeout <ms>` | `2000` | Connect timeout in ms |
| `-n, --notify` | off | System notification on change |
| `-q, --quiet` | off | Suppress startup message |
| `-v, --verbose` | off | Show every check result |
| `-1, --once` | off | One-shot check, exit 0/1 |
| `-s, --stats` | off | Print stats on exit |
| `-l, --log <file>` | none | Log events to file |
| `--on-up <cmd>` | none | Run command when online |
| `--on-down <cmd>` | none | Run command when offline |

## Examples

```bash
# Basic monitor — prints only on state change
netwatch

# Check every 10 seconds with system notifications
netwatch -i 10 -n

# One-shot: is the internet up right now?
netwatch -1 && echo "online" || echo "offline"

# Use in a script: wait until internet comes back
until netwatch -1 -t 1000; do sleep 2; done
echo "Internet is back"

# Run a sync script when reconnected, log everything
netwatch --on-up './sync_data.sh' -l /var/log/netwatch.log

# Silent background monitor with stats on exit
netwatch -q -s -l ~/netwatch.log

# Verbose — see every check (useful for debugging)
netwatch -v -i 2

# Alert on disconnect only (no notify on reconnect — edit --on-down)
netwatch --on-down 'echo "DOWN at $(date)" >> ~/outages.txt'

# Use a custom host and port (probe your own server)
netwatch --host 203.0.113.1 --port 443 -i 3
```

## Output

```
netwatch v1.0.0 — monitoring every 5s (Ctrl+C to stop)

[2026-03-15 14:22:01] ↑ ONLINE   — Internet is online
[2026-03-15 14:35:47] ↓ OFFLINE  — Internet connection lost
[2026-03-15 14:36:12] ↑ ONLINE   — Internet restored (was down 25s)

── netwatch stats ────────────────────
  Outages detected : 1
  Total downtime   : 25s
──────────────────────────────────────
```

## Resource usage

| Resource | Usage |
|----------|-------|
| CPU | ~0% between checks |
| Memory | < 1 MB RSS |
| Network | One TCP SYN per interval (< 100 bytes) |
| Dependencies | None (stdlib + sockets only) |

## Exit codes (--once mode)

| Code | Meaning |
|------|---------|
| `0` | Online |
| `1` | Offline |
| `2` | Error (bad arguments) |

## Building on Windows

Using MSYS2/MinGW:

```bash
pacman -S mingw-w64-x86_64-gcc
gcc -Wall -O2 -std=c99 -o netwatch.exe netwatch.c -lws2_32
```

## License

MIT — see [LICENSE](LICENSE)

---

*Part of the [Verbit](https://github.com/iameijaz) utility toolkit.*
*Related: [dmitree](https://github.com/iameijaz/dmitree) — directory tree viewer*
```
