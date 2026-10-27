# TODO and Deferred Decisions

## Deferred decisions

### External tool manifests

**Status:** deferred for the MVP.

Both forms are deferred:

- User-supplied local executable manifests (a config file declaring extra tools
  with descriptions and argument schemas).
- MCP/plugin tool servers.

**Why:**

- The MVP plan says to keep the tool registry minimal and not build a plugin
  framework; MCP and plugins are explicitly out of scope.
- Approved `exec_command` already covers ad hoc commands. A manifest does not
  make execution safer, and approval is not a sandbox.
- A manifest raises decisions that are not needed yet: configuration location
  and trust (workspace manifests must not be auto-trusted), argument passing,
  per-invocation approval, and process lifecycle.

**Current behavior:** built-in tools are registered in C++ through
`make_builtin_tools()`; file writes and commands require approval.

**Revisit when:** there is a concrete, recurring need for a tool that cannot be
covered by a built-in or an approved command. If revisited, treat local
executable manifests and MCP/plugin servers as separate decisions.

## Optional polish

These are not required for the MVP; they improve distribution and confidence.

### Status bar metadata

**Summary:** the agent, variant, and context-usage fields currently display
`-` placeholders. Wire them to real values where the server exposes them, or
remove the unused fields so the status bar does not imply data it does not have.

**Why deferred:** variant and context usage depend on server/model metadata and
token accounting that the MVP does not need; the agent label is effectively
fixed for the single-agent design.

### Automated UI checks

**Summary:** approval, cancellation, resize, and shutdown are verified with
manual PTY scripts. Promote the important scenarios into a repeatable test
target (scripted PTY assertions in CTest or a dedicated harness) so regressions
are caught without reading screen logs by hand.

**Why deferred:** the controller and tool layers already have deterministic
tests; the remaining risk is terminal integration, which needs a stable PTY
harness to avoid flaky output matching.

### CI pipeline

**Summary:** add a Linux CI workflow that configures, builds, runs CTest and the
Python unittest suite, and enforces black/flake8. This makes the documented
commands the actual gate for changes.

**Why deferred:** the project currently has no CI configuration or hosting
requirements.

### Packaging and install

**Summary:** add a CMake install target for the `taia_ct` binary and any needed
runtime assets, plus a package recipe if the project is distributed. Ensure the
installed binary still finds its configured server URL and workspace rules.

**Why deferred:** development runs from the build tree; installation and
distribution are not part of the MVP workflow.

## Out of scope

These are intentionally excluded from the MVP and should be treated as separate
product decisions if ever pursued.

### Persistent chat history

**Summary:** conversations live only in memory for the current run. Persistence
would require a storage format, session selection, migration, and privacy
decisions.

### TLS and remote model providers

**Summary:** the client supports `http://` only and targets a local llama.cpp
router. HTTPS, authentication beyond a bearer token, and remote provider APIs
are not implemented.

### Model downloads and server management

**Summary:** the app connects to an already-running server. Downloading models,
starting/stopping llama-server, and managing its configuration remain outside
the app.

### Multi-agent workflows

**Summary:** the MVP has one agent and one turn loop. Delegation, subagents,
parallel agent sessions, and cross-agent coordination are not implemented.
