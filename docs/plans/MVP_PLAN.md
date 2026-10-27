# MVP Project Plan

## Goal

Build a lightweight C++ coding agent using cpp-tui, with an experience comparable to OpenCode. The MVP connects to llama.cpp v0.7.0, supports model selection and lifecycle controls, streams chat, and can inspect and modify a workspace with user approval.

Milestones 1-5 are implemented. Milestone 6 is planned. Deferred work is tracked in [`TODO.md`](TODO.md).

## Decisions and Constraints

- Pin llama.cpp to tag `v0.7.0`, commit `620d2225cc436ab0b001e3200e590e3b286ef9ab`.
- Use llama.cpp **router mode** for model management. The client and mock target `GET /models`, `POST /models/load`, `POST /models/unload`, and streamed `POST /v1/chat/completions`.
- Default server URL: `http://127.0.0.1:9931`, configurable by the user.
- The app connects to an already-running server. Starting llama-server, downloading models, and managing its process are out of MVP scope.
- Keep model tool execution in the C++ app. Do not rely on llama.cpp's server-side `--tools` or `--agent` features.
- File reads may run directly. File writes and shell commands require approval of the exact proposed operation.
- Chat history is in memory for the current session; persistent conversations are out of scope.
- Use C++17 compatibility and CMake as the starting build choices, subject to confirmation while establishing the project skeleton.

## Components

### First implementation slice

- **App:** Loads configuration, establishes the workspace root, and wires component lifetimes.
- **cpp-tui UI:** Displays conversation, prompt input, model selection/status, loading and generation state, and errors. It emits actions; it does not perform network or tool work.
- **Session Controller:** Owns visible session state and coordinates UI actions and services.
- **Async Work Bridge:** Runs network work off the UI thread and returns typed events to the controller. Keep this small; workers must not mutate UI widgets directly.
- **Llama Server Client:** Implements the pinned HTTP/SSE contract. It has no mock-specific behavior and can target either the Python mock or llama-server by changing the configured base URL.
- **Python Mock Server:** A separate development and test process that emulates the pinned llama.cpp API. Python is not a runtime dependency of the C++ app.

### Deferred until streamed chat works

- **Agent turn loop:** Handles model tool calls, tool results, turn limits, and cancellation.
- **Tool definitions and dispatch:** Introduce a minimal registry when the agent needs real tools; do not build a plugin framework.
- **Workspace tools:** Start with file listing, search, and reading. Add patch application and command execution later.
- **Approval flow:** Add the policy and connect it to real mutations and commands when those tools exist. The UI may prototype an approval view earlier, but it should not require a premature tool API.
- **Context assembly:** Initially keep conversation and workspace-root `AGENTS.md` handling in the agent turn path. Extract a separate Context Builder only if it becomes substantial.

## Milestones

1. **First build: cpp-tui UI and Python mock.** Define the mock contract from the pinned v0.7.0 server docs, then build the mock and UI together with only the C++ client/controller scaffolding needed to connect them. The mock covers model listing, load/unload state transitions, streamed text responses, delays, and errors. The UI covers prompt entry, conversation display, model selection/status, progress, and failure states. Do not build the real-server integration, agent loop, or tools before this slice works.

   **Exit condition:** The app can list, load, and unload mock models; stream a response while remaining responsive; and display server and request failures.

2. **Verify against llama.cpp v0.7.0.** Point the same client at a real v0.7.0 router-mode server and a locally available model. Confirm model listing, load/unload, chat streaming, and error behavior. Keep the server version, API payloads, and stream format pinned in the Python mock tests.

   **Exit condition:** The UI workflows that pass against the mock also work against the pinned server without mock-specific code.

3. **Add the agent loop and read-only tools.** Implement model tool-call handling and a bounded turn loop. Add file listing, search, and reading scoped to the workspace. Load workspace-root `AGENTS.md` as project guidance. Validate tool arguments and bound output and number of tool rounds.

   **Exit condition:** The agent can inspect a workspace and return a useful answer using its own tool loop.

4. **Add approved mutations and commands.** Add patch preview/application and command execution. Show the exact diff or command and working directory; execute only after approval. Revalidate a proposed file change before applying it. Approval is not a sandbox: shell commands run with the application user's permissions.

   **Exit condition:** Rejecting an action leaves the workspace unchanged; approving it performs only the displayed operation.

5. **Harden and document.** Cover cancellation, disconnects, malformed or incomplete streamed events, server errors, model lifecycle failures, terminal resize, and application shutdown. Document setup, mock usage, the pinned server version, and the exact build/test commands after the project build system is established.

6. **Add CLI configuration.** Extend the positional and environment setup with a tested argument parser while preserving existing invocations.

   **Invocation:**

   ```text
   taia_ct [OPTIONS] [SERVER_URL]
   ```

   **Options:**

   - `-h`, `--help`: print usage and exit successfully without starting the TUI.
   - `-u`, `--server-url URL`: set the server base URL.
   - `-w`, `--workspace PATH`: set the workspace root.
   - `--stream-start-timeout-seconds N`: set the first-token timeout.
   - `--stream-idle-timeout-seconds N`: set the inter-chunk idle timeout.
   - `--prompt-api-key`: read an API key from hidden stdin before TUI startup.

   Do not add an `--api-key VALUE` option. Keep `TAIA_API_KEY` as the
   non-interactive source.

   **Implementation:**

   - Add `src/cli_options.hpp` and `src/cli_options.cpp` with a parser that
     returns structured overrides plus help/error status. Keep parsing free of
     filesystem, network, terminal, and TUI work. Put usage text in one place.
   - Resolve ordinary settings with **CLI > environment > existing default**.
     Retain the positional server URL for compatibility. Reject a positional
     URL when `--server-url` is also supplied. Reject unknown options, missing
     values, and extra positional arguments.
   - Parse timeout values as whole seconds in the existing accepted range,
     `1-86400`. Invalid CLI values report a clear error, print usage, and exit
     nonzero before starting the TUI. Keep current environment-variable
     fallback behavior for invalid environment values.
   - In `src/main.cpp`, parse arguments first. On `--help`, print usage and
     return success. On parse errors, print the error and usage to stderr and
     return nonzero. Otherwise resolve the final settings and start the app.
   - Implement `--prompt-api-key` in `src/main.cpp`: require
     `isatty(STDIN_FILENO)`, disable echo with `termios`, read one line, restore
     terminal settings on every success and error path, and print a newline
     after input. Reject empty input or EOF with a nonzero exit. The prompted
     key overrides `TAIA_API_KEY`. Never print or log the key.
   - When `--prompt-api-key` is absent, preserve current `TAIA_API_KEY`
     behavior, including no prompt and no key when unset.
   - Update `README.md` and `AGENTS.md` with the flags, precedence, examples,
     and the security tradeoff between the hidden prompt and environment
     variables.

   **Tests:**

   - Add `tests/cpp/test_cli_options.cpp` and register a `taia_cli_test` target
     in `CMakeLists.txt` with `src/cli_options.cpp`.
   - Cover every flag, positional URL compatibility, CLI-over-environment
     precedence, conflicting URL sources, unknown options, missing values, extra
     positional arguments, timeout bounds, and help parsing.
   - Add a PTY test that confirms the API-key prompt is not echoed, the key is
     sent for authentication, and terminal settings are restored after success,
     empty input, and EOF.

   **Exit condition:** `make test`, the Python tests, Black, and flake8 pass.
   `--help` and invalid invocations exit before TUI startup with the documented
   status. Existing environment-variable and positional-URL invocations continue
   to work, and the prompted API key authenticates without appearing on screen.

## Verification

- Test Python mock routes and state transitions, including load/unload failures and delayed responses.
- Test C++ client parsing and controller state changes against deterministic mock responses.
- Test streamed responses, disconnects, cancellation, and server errors before connecting to the real server.
- Run a manual end-to-end check against llama.cpp v0.7.0 and a locally available model.
- Test tool argument validation, workspace path boundaries, approval/rejection, and patch behavior when those components are introduced.
- Test CLI parsing, precedence, validation, help, and the hidden API-key prompt. Verify invalid invocations exit before TUI startup.
- Define exact build and test commands in project files; do not invent commands before those files exist.

## Out of Scope

Remote LLM providers, model downloads, llama-server process management, persistent chat history, multimodal input, semantic indexing, MCP/plugins, and multi-agent workflows.

## Command execution

The initial implementation supports Linux/bash only: `/bin/bash -lc` runs in the
workspace under the application user's permissions. Approval is not a sandbox.

## References

- [llama.cpp v0.7.0 release](https://github.com/ggml-org/llama.cpp/releases/tag/v0.7.0)
- [v0.7.0 server documentation](https://github.com/ggml-org/llama.cpp/blob/v0.7.0/tools/server/README.md)
- [cpp-tui](https://github.com/jonoton/cpp-tui)
