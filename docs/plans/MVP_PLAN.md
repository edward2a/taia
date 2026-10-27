# MVP Project Plan

## Goal

Build a lightweight C++ coding agent using cpp-tui, with an experience comparable to OpenCode. The MVP connects to llama.cpp v0.7.0, supports model selection and lifecycle controls, streams chat, and can inspect and modify a workspace with user approval.

Milestones 1-5 are implemented. Deferred work is tracked in [`TODO.md`](TODO.md).

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

## Verification

- Test Python mock routes and state transitions, including load/unload failures and delayed responses.
- Test C++ client parsing and controller state changes against deterministic mock responses.
- Test streamed responses, disconnects, cancellation, and server errors before connecting to the real server.
- Run a manual end-to-end check against llama.cpp v0.7.0 and a locally available model.
- Add tests for tool argument validation, workspace path boundaries, approval/rejection, and patch behavior when those components are introduced.
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
