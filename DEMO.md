# Demo Recording Guide

Everything you need to record a demo of lsmdb on any machine.

---

## Prerequisites

| Tool | Minimum version | Install |
|------|----------------|---------|
| CMake | 3.20 | `brew install cmake` / `apt install cmake` |
| C++20 compiler | gcc 10+, clang 11+, or MSVC 2019+ | system package manager |
| asciinema | any | `brew install asciinema` / `pip install asciinema` |
| `nc` (netcat) | any | ships with macOS/Linux |

Optional — only needed if you want to convert the recording to a GIF:

```bash
# macOS
brew install agg

# or from source: https://github.com/asciinema/agg
```

---

## Setup (one-time)

```bash
git clone https://github.com/PranayGoel/lsmdb.git
cd lsmdb

# Configure + build (downloads Catch2 the first time — takes ~2 min)
cmake -S . -B build
cmake --build build --parallel

# Sanity check: all 70 tests should pass
ctest --test-dir build --output-on-failure
```

---

## Record

```bash
# From the repo root — natural pacing, ~6-8 min total
asciinema rec lsmdb-demo.cast -c 'bash demo.sh'
```

The script handles everything: starts and stops servers, runs commands with visible
output, and cleans up after itself. Just let it run.

**Terminal setup for a clean recording:**
- Width: 130 columns, height: 40 rows (`resize` or terminal settings)
- Theme: dark background (Dracula, One Dark, Catppuccin all look good)
- Font: any monospaced — JetBrains Mono / Fira Code / Cascadia Code work well at 14pt+

---

## What the recording covers

| Scene | Duration | What it shows |
|-------|----------|---------------|
| 1 — Build | ~20 sec | `cmake --build` — all targets compile clean |
| 2 — Test suite | ~15 sec | `ctest` — 70/70 pass in under 1 second |
| 3 — Wire protocol | ~30 sec | PING, PUT, GET, missing key (`$-1`), DELETE over `nc` |
| 4 — Crash recovery | ~40 sec | SIGKILL the server mid-data, restart, all 3 keys survive |
| 5 — Benchmark | ~20 sec | 16 concurrent clients, 160k ops, ~28k ops/sec, zero errors |
| 6 — Replication | ~60 sec | Snapshot, live replication, reject writes on replica, primary killed, replica holds everything |

---

## Dry-run (no delays)

To verify the script works before recording:

```bash
FAST=1 bash demo.sh
```

Safe to run any number of times — it cleans up leftover processes and temp dirs
at startup.

---

## Share / Publish

**Upload to asciinema.org** (free, shareable link):

```bash
asciinema upload lsmdb-demo.cast
# → https://asciinema.org/a/<id>
```

You'll be prompted to log in / create an account on first upload. The link is
immediately shareable.

**Embed in README** (asciinema player, no autoplay):

```markdown
[![asciicast](https://asciinema.org/a/<id>.svg)](https://asciinema.org/a/<id>)
```

**Convert to GIF** (for platforms that don't support embed):

```bash
agg lsmdb-demo.cast lsmdb-demo.gif
```

`agg` respects the original timing and colors. Default output is `1280×720`; use
`--cols 130 --rows 40` to match the recording dimensions if you set them explicitly.

---

## Re-recording tips

- The `.cast` file is a plain JSON text file — you can re-upload or re-convert any time.
- To trim the start/end: [`asciinema-edit`](https://github.com/cirocosta/asciinema-edit) has a `cut` subcommand.
- To speed up slow sections: `asciinema-edit quantize --range 2 lsmdb-demo.cast`.
