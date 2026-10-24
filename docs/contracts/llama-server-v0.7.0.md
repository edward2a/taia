# llama.cpp v0.7.0 server contract (router mode)

Scope: the exact HTTP contract the C++ client and Python mock implement for
Milestone 1. Source of truth is the pinned server docs; do not invent fields.

- Server: llama.cpp tag `v0.7.0`, commit `620d2225cc436ab0b001e3200e590e3b286ef9ab`.
- Docs: `tools/server/README.md` at that commit (the `v0.7.0` tag dereferences to it).
- Mode: router mode (`llama-server` started with no model).
- Base URL: `http://127.0.0.1:9931` by default, configurable.

## Transport

- JSON requests use `Content-Type: application/json`.
- If the server was started with an API key, requests carry `Authorization: Bearer <key>`. The MVP treats the key as optional configuration.
- Streaming responses use `Content-Type: text/event-stream` (SSE).

## Error envelope

All JSON error responses share one envelope:

```json
{ "error": { "code": 400, "message": "invalid request", "type": "invalid_request_error" } }
```

`code` is the HTTP status code. `type` is one of:

| `type` | HTTP |
|---|---|
| `invalid_request_error` | 400 |
| `authentication_error` | 401 |
| `permission_error` | 403 |
| `not_found_error` | 404 |
| `server_error` | 500 |
| `not_supported_error` | 501 |
| `unavailable_error` | 503 |
| `exceed_context_size_error` | 400 |

## GET /health

Public (no API key). The mock exposes this so the client can probe readiness.

- 200: `{ "status": "ok" }` — server ready.
- 503: `{ "error": { "code": 503, "message": "Loading model", "type": "unavailable_error" } }` — model still loading.

## GET /models

Router model list. Optional query `?reload=1` refreshes the list from sources.

Response:

```json
{
  "object": "list",
  "data": [
    {
      "id": "ggml-org/gemma-3-4b-it-GGUF:Q4_K_M",
      "object": "model",
      "owned_by": "llamacpp",
      "created": 1735142223,
      "aliases": [],
      "tags": [],
      "source": "cache",
      "can_remove": true,
      "context_length": 131072,
      "status": { "value": "loaded", "args": ["llama-server", "-ctx", "4096"] },
      "architecture": {
        "input_modalities": ["text"],
        "output_modalities": ["text"]
      }
    }
  ]
}
```

`status.value` is one of `downloading`, `downloaded`, `unloaded`, `loading`,
`loaded`, `sleeping`. On a failed load the status object also carries
`"failed": true` and `"exit_code": <int>`. `downloading` carries a
`progress` map keyed by URL with `{ "done": <bytes>, "total": <bytes> }`.

Client notes:
- Match models by `id`. Tolerate unknown fields and unknown `status.value`.
- `architecture` may be absent on older servers; do not require it.
- `decisions` in `output_modalities` marks a decision model; irrelevant to M1.

## POST /models/load

Request:

```json
{ "model": "ggml-org/gemma-3-4b-it-GGUF:Q4_K_M" }
```

Success `200`:

```json
{ "success": true }
```

Errors:
- `404 not_found_error`: `"model is not found"`.
- `400 invalid_request_error`: `"model is already running"`.

The load is asynchronous: `success` means the load started. Poll `GET /models`
until `status.value` becomes `loaded` (or `failed`).

## POST /models/unload

Request:

```json
{ "model": "ggml-org/gemma-3-4b-it-GGUF:Q4_K_M" }
```

Success `200`:

```json
{ "success": true }
```

Errors (both `400 invalid_request_error`):
- `"model is not found"`.
- `"model is not running"`.

After a successful call, poll `GET /models` until `status.value` is `unloaded`.

## POST /v1/chat/completions

Router routing uses the JSON body's `"model"` field. In router mode `"model"`
is required and names the model to route to.

Request fields used by M1:

```json
{
  "model": "ggml-org/gemma-3-4b-it-GGUF:Q4_K_M",
  "messages": [
    { "role": "system", "content": "You are a helpful assistant." },
    { "role": "user", "content": "hello" }
  ],
  "stream": false,
  "max_tokens": 256,
  "temperature": 0.8
}
```

`messages[].role` is one of `system`, `user`, `assistant`, `tool` (plus
`developer` on newer servers). `content` is a string. Tool fields are out of
scope for M1.

### Non-streaming response (`stream`: false or absent)

```json
{
  "id": "chatcmpl-ecQULm0WqPrftUqjPZO1CFYeDjGZNbDu",
  "object": "chat.completion",
  "created": 1757141666,
  "model": "llama-test-model",
  "system_fingerprint": "b6789-abcd",
  "choices": [
    {
      "index": 0,
      "message": { "role": "assistant", "content": "Hello!" },
      "finish_reason": "stop"
    }
  ],
  "usage": {
    "prompt_tokens": 44,
    "completion_tokens": 48,
    "total_tokens": 92,
    "prompt_tokens_details": { "cached_tokens": 0 }
  },
  "timings": {
    "prompt_n": 1, "prompt_ms": 30.958, "prompt_per_second": 32.3,
    "predicted_n": 35, "predicted_ms": 661.064, "predicted_per_second": 52.9
  }
}
```

Notes:
- `finish_reason` is `stop`, `length`, or (with tools) `tool_calls`.
- `message.content` may be `null` when the model emits tool calls (out of scope for M1).
- `model` is the server-side alias, not guaranteed to equal the requested name.
- `timings` and `system_fingerprint` are present but not required by the client.

### Streaming response (`stream`: true)

Server emits Server-Sent Events, one JSON object per event, each terminated by a blank line.

Each event line carries the SSE data field followed by the JSON chunk.

The first chunk's delta carries the assistant role and null content.

Subsequent chunks carry delta content; a final chunk carries an empty delta and the finish reason.

```json
{"choices":[{"finish_reason":null,"index":0,"delta":{"role":"assistant","content":null}}],"created":1757141666,"id":"chatcmpl-...","model":"llama-test-model","system_fingerprint":"b6789-abcd","object":"chat.completion.chunk"}
```

Subsequent content chunk:

```json
{"choices":[{"finish_reason":null,"index":0,"delta":{"content":"Hello"}}],"created":1757141666,"id":"chatcmpl-...","model":"llama-test-model","system_fingerprint":"b6789-abcd","object":"chat.completion.chunk"}
```

Final chunk:

```json
{"choices":[{"finish_reason":"stop","index":0,"delta":{}}],"created":1757141666,"id":"chatcmpl-...","model":"llama-test-model","system_fingerprint":"b6789-abcd","object":"chat.completion.chunk"}
```

When `stream_options.include_usage` is true, one extra chunk with an empty `choices` array and a `usage` object is sent after the final chunk.

After the last chunk the stream ends with a sentinel event whose data field is the literal token DONE enclosed in square brackets.

The server may send keep-alive comment events (a line containing only a colon) while the stream is silent.

If an error occurs mid-stream the server sends one event carrying the standard error envelope, then closes without a DONE token.

## Routing and autoload

POST endpoints route by the body's `model` field; GET endpoints route by the `model` query parameter (URL-encoded).

A missing or unknown model yields `400 invalid_request_error`.

By default the router autoloads a model on first request; the per-request `autoload=true` query parameter forces it and `autoload=false` disables it.

## Mock contract (Milestone 1)

The Python mock emulates these routes for development and tests.

- Serves `GET /health`, `GET /models`, `POST /models/load`, `POST /models/unload`, `POST /v1/chat/completions`.
- Models load/unload as async state transitions: `unloaded` to `loading` to `loaded`, and `loaded` to `unloaded`, with a failure path reaching `unloaded` plus `failed: true` and `exit_code`.
- Supports streamed chat with a configurable per-token delay, and both streaming and non-streaming responses.
- Returns the exact error envelope and HTTP status codes defined above.
