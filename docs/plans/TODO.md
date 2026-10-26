# TODO and Deferred Decisions

## External tool manifests

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
