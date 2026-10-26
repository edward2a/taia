# Repository notes

- Project goal: build a lightweight C++ LLM agent using cpp-tui, with an experience comparable to OpenCode.
- Plan lives in `docs/plans/MVP_PLAN.md`. Current target: Milestone 1 — build the cpp-tui UI and Python mock together, with only the client/controller scaffolding needed to connect them.
- `cpp-tui` is a single-header C++17 library fetched via CMake `FetchContent`, pinned to commit `3ab1f3193c54b50adbaca019a448bfcff8daf159`. No system libraries are required to build the TUI.

## Build and run

- Configure: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug`
- Build: `cmake --build build -j`
- Run: `./build/taia_ct http://127.0.0.1:9931` (or set `TAIA_SERVER_URL`).
- API key: set `TAIA_API_KEY` when the server requires `Authorization: Bearer`.
- Timeouts: `TAIA_STREAM_START_TIMEOUT_SECONDS` (default 300) covers cold prefill
  before the first token; `TAIA_STREAM_IDLE_TIMEOUT_SECONDS` (default 30) covers
  stalls between chunks.
- C++ tests: `ctest --test-dir build --output-on-failure`

Python tests use the stdlib `unittest`.

## Python mock

- Run: `python3 mock/server.py --port 9931`
- Test: `python3 -m unittest discover -s tests -v`
- Lint: `python3 -m black mock tests` and `python3 -m flake8 mock tests` (config in `setup.cfg`)
