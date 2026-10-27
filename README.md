# taia-ct

A lightweight C++17 coding agent for the terminal. It uses
[cpp-tui](https://github.com/jonoton/cpp-tui) for the interface and talks to a
llama.cpp router-mode server over its OpenAI-compatible API. The agent can list,
search, and read workspace files, and can write files or run shell commands
after showing an exact preview and asking for approval.

## Requirements

- Linux with a POSIX shell (`/bin/bash` for agent commands).
- CMake 3.16 or newer and a C++17 compiler.
- Network access during the first configure to fetch pinned dependencies.
- Python 3 for the mock server and Python tests (not a runtime dependency).
- Optional: llama.cpp v0.7.0 in router mode with a tool-calling-capable model
  template (`--jinja`).

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

Or use the Makefile wrappers:

```sh
make            # configure and build
make test       # build and run CTest
make python-test
make clean      # remove the build directory
```

## Run

Start the mock for local development:

```sh
python3 mock/server.py --port 9931
```

Then run the app:

```sh
./build/taia_ct http://127.0.0.1:9931
```

The server URL can also come from `TAIA_SERVER_URL`; a positional argument wins.
The app connects to an already-running server and does not start or manage
llama-server.

### Environment

| Variable | Default | Purpose |
| --- | --- | --- |
| `TAIA_SERVER_URL` | `http://127.0.0.1:9931` | Server base URL (HTTP only). |
| `TAIA_API_KEY` | unset | Sent as `Authorization: Bearer` when set. It is removed from agent command environments. |
| `TAIA_WORKSPACE` | current directory | Workspace root for tools and agent commands. |
| `TAIA_STREAM_START_TIMEOUT_SECONDS` | `300` | Time allowed before the first streamed token (cold prefill). |
| `TAIA_STREAM_IDLE_TIMEOUT_SECONDS` | `30` | Time allowed between streamed chunks. |

## Keys

| Key | Action |
| --- | --- |
| `Enter` | Submit prompt |
| `Esc` | Cancel generation/tool execution, or reject approval |
| `y` / `n` | Approve / reject a pending mutation |
| Arrows, PgUp/PgDn | Scroll approval preview |
| Left/Right, Home/End | Pan/scroll long approval lines |
| `Ctrl+L` / `Ctrl+U` | Load / unload selected model |
| `Ctrl+R` | Refresh model list |
| `Ctrl+J` | Insert a newline in the prompt |
| `q`, `Ctrl+C` | Quit |

`Ctrl+C` copies a text selection when one exists; otherwise it quits.

## Tools and approval

| Tool | Approval | Notes |
| --- | --- | --- |
| `list_files` | No | Workspace-relative path, non-recursive by default. |
| `search` | No | Substring search in text files; skips `.git` and binary files. |
| `read_file` | No | Line-numbered reads with line limits. |
| `write_file` | Yes | Shows a diff. Revalidates the file before applying an atomic replacement. |
| `exec_command` | Yes | Shows the exact command and working directory. Runs `/bin/bash -lc` in the workspace. |

Read-only tools are scoped to the workspace. Paths that resolve outside the root
are rejected. `write_file` writes through a non-symlink temporary file in the
target directory and preserves an existing file's mode; new files respect the
process umask.

Approval is **not** a sandbox. Commands run with the application user's
permissions and can access anything that user can. A command inherits the
environment except `TAIA_API_KEY`, has stdin connected to `/dev/null`, runs with
a timeout (default 10 s, maximum 60 s), and has bounded output. The process
group is killed on timeout, cancellation, or shell exit so background children
do not outlive the call.
The displayed command is the command that runs; file writes are revalidated
against the preview before replacement.

The server must support OpenAI-style tool calls. With llama.cpp, enable a
compatible chat template (`--jinja`).

## Development

### Python mock

```sh
python3 mock/server.py --port 9931
python3 mock/server.py --port 9931 --token-delay 0.15 --load-delay 0.5
```

The mock implements the pinned model routes, streamed chat, tool calls, slow
streams, load failures, and request validation. It is deterministic and is the
primary development target.

### Tests

```sh
ctest --test-dir build --output-on-failure
python3 -m unittest discover -s tests -v
```

### Lint

```sh
python3 -m black --check mock tests helper-scripts
python3 -m flake8 mock tests helper-scripts
```

Format with `python3 -m black mock tests helper-scripts`.

### Manual terminal checks

`helper-scripts/run_pty.py` drives the TUI under a pseudo-terminal and writes a
raw screen log. `render_screen.py` renders the final frame. For example:

```sh
TAIA_WORKSPACE=$PWD \
PTY_START_WAIT=2 PTY_KEY_WAIT=3 PTY_RESIZE_STEPS='24x80:1' \
python3 helper-scripts/run_pty.py 40 120 tmp/ui.pty.log '' \
  ./build/taia_ct http://127.0.0.1:9931
python3 helper-scripts/render_screen.py tmp/ui.pty.log 24 80
```

## Architecture

- `src/app.cpp`: wires the controller and UI, drains controller events, handles
  UI actions, and shuts down the worker.
- `src/ui/`: cpp-tui widgets for conversation, prompt input, approval, status,
  and shortcuts. The UI performs no network or tool work.
- `src/controller/`: owns session state and the agent turn loop. A single worker
  thread performs requests and tools; typed events return to the UI thread.
- `src/client/`: maps the llama.cpp router/chat contract to the app.
- `src/net/`: small HTTP/1.1 + SSE client with cancellation and timeouts.
- `src/tools/`: built-in workspace tools, approval plumbing, and command
  execution.
- `mock/server.py`: deterministic development/test server.

Conversation history is in memory for the current run. The controller bounds
history, per-turn tool output, tool rounds, and tool-call arguments and drops a
failed turn so a retry does not duplicate it.

## Pinned dependencies

- llama.cpp `v0.7.0`, commit `620d2225cc436ab0b001e3200e590e3b286ef9ab`
- cpp-tui commit `3ab1f3193c54b50adbaca019a448bfcff8daf159`
- nlohmann/json commit `55f93686c01528224f448c19128836e7df245f72`

## Limitations

- Linux/bash only; command execution is not sandboxed.
- HTTP only (no TLS) and no remote LLM providers.
- No persistent chat history, model downloads, or llama-server management.
- No MCP, plugins, or external tool manifests. See
  [`docs/plans/TODO.md`](docs/plans/TODO.md).

## Planning

- [`docs/plans/MVP_PLAN.md`](docs/plans/MVP_PLAN.md)
- [`docs/plans/TODO.md`](docs/plans/TODO.md)
- [`docs/contracts/llama-server-v0.7.0.md`](docs/contracts/llama-server-v0.7.0.md)
