# Repository notes

- Project goal: build a lightweight C++ LLM agent using cpp-tui, with an experience comparable to OpenCode.
- Plan lives in `docs/plans/MVP_PLAN.md`. Milestones 1-6 are implemented; `docs/plans/TODO.md` tracks deferred work.
- Setup, usage, tool/approval behavior, and commands are documented in `README.md`.
- `cpp-tui` is a single-header C++17 library fetched via CMake `FetchContent`, pinned to commit `3ab1f3193c54b50adbaca019a448bfcff8daf159`. No system libraries are required to build the TUI.

## Build and run

- Configure: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug`
- Build: `cmake --build build -j` or `make`
- Tests: `ctest --test-dir build --output-on-failure` or `make test`
- Remove the build directory: `make clean`
- Run: `./build/taia_ct http://127.0.0.1:9931` (or set `TAIA_SERVER_URL`).
- CLI: `./build/taia_ct [OPTIONS] [SERVER_URL]`; `--help` lists options. Command-line values override environment variables.
- Workspace: `TAIA_WORKSPACE` sets the agent workspace root (default: current directory).
- API key: set `TAIA_API_KEY` when the server requires `Authorization: Bearer`, or pass `--prompt-api-key` to read it from hidden terminal input before startup.
- Timeouts: `TAIA_STREAM_START_TIMEOUT_SECONDS` (default 300) covers cold prefill
  before the first token; `TAIA_STREAM_IDLE_TIMEOUT_SECONDS` (default 30) covers
  stalls between chunks.
- Agent tools: `list_files`, `search`, `read_file` are scoped to the workspace;
  `write_file` and `exec_command` require approval. Commands run with
  `/bin/bash -lc` in the workspace under the application user's permissions, so
  approval is not a sandbox. The server must support OpenAI-style tool calls
  (`--jinja` with a compatible template).

Python tests use the stdlib `unittest`.

## Python mock

- Run: `python3 mock/server.py --port 9931`
- Test: `python3 -m unittest discover -s tests -v`
- Lint: `python3 -m black mock tests helper-scripts` and `python3 -m flake8 mock tests helper-scripts` (config in `setup.cfg`)
