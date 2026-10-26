import argparse
import json
import os
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ERROR_TYPES = {
    "invalid_request_error": 400,
    "authentication_error": 401,
    "permission_error": 403,
    "not_found_error": 404,
    "server_error": 500,
    "not_supported_error": 501,
    "unavailable_error": 503,
    "exceed_context_size_error": 400,
}

DEFAULT_MODELS = [
    "mock/small",
    "mock/large",
    "mock/slow-stream",
    "mock/fail-load",
    "mock/error-stream",
]

FAIL_LOAD_MODEL = "mock/fail-load"
SLOW_STREAM_MODEL = "mock/slow-stream"
ERROR_STREAM_MODEL = "mock/error-stream"
DONE_EVENT = "data: " + chr(91) + "DONE" + chr(93)


def error_body(message, error_type="invalid_request_error"):
    return {
        "error": {
            "code": ERROR_TYPES[error_type],
            "message": message,
            "type": error_type,
        }
    }


class ModelState:
    def __init__(self, name):
        self.name = name
        self.status = "unloaded"
        self.failed = False
        self.exit_code = None
        self.args = ["llama-server", "-ctx", "4096"]
        self.architecture = {
            "input_modalities": ["text"],
            "output_modalities": ["text"],
        }

    def to_json(self):
        status = {"value": self.status, "args": self.args}
        if self.failed:
            status["failed"] = True
            status["exit_code"] = self.exit_code
        return {
            "id": self.name,
            "object": "model",
            "owned_by": "llamacpp",
            "created": 1735142223,
            "aliases": [],
            "tags": [],
            "source": "cache",
            "can_remove": True,
            "status": status,
            "architecture": self.architecture,
        }


class MockState:
    def __init__(self, model_ids=None, token_delay=0.02, load_delay=0.2):
        self.lock = threading.Lock()
        self.models = {name: ModelState(name) for name in (model_ids or DEFAULT_MODELS)}
        self.token_delay = token_delay
        self.load_delay = load_delay

    def is_running(self, model):
        return model.status in ("loading", "loaded", "sleeping")

    def get(self, name):
        return self.models.get(name)

    def list_models(self):
        with self.lock:
            return [m.to_json() for m in self.models.values()]

    def any_loading(self):
        with self.lock:
            return any(m.status == "loading" for m in self.models.values())

    def load(self, name):
        with self.lock:
            model = self.models.get(name)
            if model is None:
                return False, error_body("model is not found", "not_found_error")
            if self.is_running(model):
                return False, error_body("model is already running")
            model.status = "loading"
            model.failed = False
            model.exit_code = None
        thread = threading.Thread(target=self._finish_load, args=(name,), daemon=True)
        thread.start()
        return True, {"success": True}

    def _finish_load(self, name):
        time.sleep(self.load_delay)
        with self.lock:
            model = self.models.get(name)
            if model is None or model.status != "loading":
                return
            if name == FAIL_LOAD_MODEL:
                model.status = "unloaded"
                model.failed = True
                model.exit_code = 1
            else:
                model.status = "loaded"

    def unload(self, name):
        with self.lock:
            model = self.models.get(name)
            if model is None:
                return False, error_body("model is not found")
            if not self.is_running(model):
                return False, error_body("model is not running")
            model.status = "unloaded"
            model.failed = False
            model.exit_code = None
        return True, {"success": True}


def token_delay_for(state, model):
    if model == SLOW_STREAM_MODEL:
        return max(state.token_delay, 0.15)
    return state.token_delay


VALID_ROLES = {"system", "user", "assistant", "tool", "developer"}


def validate_messages(messages):
    for message in messages:
        if not isinstance(message, dict):
            return "messages must contain objects"
        if message.get("role") not in VALID_ROLES:
            return "message role is invalid"
        content = message.get("content")
        if content is not None and not isinstance(content, str):
            return "message content must be a string"
    return None


def generate_reply(model, messages):
    if messages and messages[-1].get("role") == "tool":
        content = messages[-1].get("content")
        text = content if isinstance(content, str) else ""
        return "tool result: " + text[:200]
    prompt = ""
    for message in messages:
        if message.get("role") == "user":
            content = message.get("content")
            if isinstance(content, str):
                prompt = content
    body = "echo: " + prompt if prompt else "echo:"
    return "{} ({}).".format(body, model)


def validate_tools(tools):
    if tools is None:
        return None
    if not isinstance(tools, list):
        return "tools must be an array"
    for tool in tools:
        if not isinstance(tool, dict):
            return "tools must contain objects"
        if tool.get("type") != "function":
            return "tool type must be function"
        function = tool.get("function")
        if not isinstance(function, dict):
            return "tool function must be an object"
        name = function.get("name")
        if not isinstance(name, str) or not name:
            return "tool function name is required"
        if not isinstance(function.get("parameters"), dict):
            return "tool function parameters must be an object"
    return None


def tool_names(tools):
    names = set()
    for tool in tools:
        if not isinstance(tool, dict):
            continue
        function = tool.get("function")
        if isinstance(function, dict):
            name = function.get("name")
            if isinstance(name, str):
                names.add(name)
    return names


def make_tool_call(name, arguments):
    return {
        "id": "call_" + uuid.uuid4().hex[:16],
        "name": name,
        "arguments": arguments,
    }


def tool_call_payload(call):
    return {
        "id": call["id"],
        "type": "function",
        "function": {
            "name": call["name"],
            "arguments": json.dumps(call["arguments"]),
        },
    }


def select_tool_call(messages, tools):
    names = tool_names(tools or [])
    if not names or not messages:
        return None
    last = messages[-1]
    if not isinstance(last, dict) or last.get("role") != "user":
        return None
    content = last.get("content")
    if not isinstance(content, str):
        return None
    lowered = content.lower()
    if "list_files" in names and "list" in lowered:
        return make_tool_call("list_files", {"path": "."})
    if "search" in names and "search" in lowered:
        query = content.split("search", 1)[1].strip() or "TODO"
        return make_tool_call("search", {"query": query})
    if "read_file" in names and "read" in lowered:
        path = content.split("read", 1)[1].strip() or "README.md"
        return make_tool_call("read_file", {"path": path})
    return None


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "taia-mock/0.1"

    def log_message(self, fmt, *args):
        return

    @property
    def state(self):
        return self.server.state

    def _read_json(self):
        length = int(self.headers.get("Content-Length", 0) or 0)
        if length == 0:
            return {}
        raw = self.rfile.read(length)
        return json.loads(raw.decode("utf-8"))

    def _send_json(self, status, payload):
        data = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path in ("/health", "/v1/health"):
            if self.state.any_loading():
                self._send_json(503, error_body("Loading model", "unavailable_error"))
            else:
                self._send_json(200, {"status": "ok"})
        elif path in ("/models", "/v1/models"):
            self._send_json(200, {"object": "list", "data": self.state.list_models()})
        else:
            self._send_json(404, error_body("not found", "not_found_error"))

    def do_POST(self):
        try:
            body = self._read_json()
        except (json.JSONDecodeError, UnicodeDecodeError):
            self._send_json(400, error_body("invalid JSON body"))
            return
        if not isinstance(body, dict):
            self._send_json(400, error_body("request body must be a JSON object"))
            return
        path = self.path.split("?", 1)[0]
        if path == "/models/load":
            self._handle_load(body)
        elif path == "/models/unload":
            self._handle_unload(body)
        elif path == "/v1/chat/completions":
            self._handle_chat(body)
        else:
            self._send_json(404, error_body("not found", "not_found_error"))

    def _handle_load(self, body):
        name = body.get("model", "")
        if not isinstance(name, str) or not name:
            self._send_json(400, error_body("model name is missing from the request"))
            return
        ok, payload = self.state.load(name)
        self._send_json(200 if ok else payload["error"]["code"], payload)

    def _handle_unload(self, body):
        name = body.get("model", "")
        if not isinstance(name, str) or not name:
            self._send_json(400, error_body("model name is missing from the request"))
            return
        ok, payload = self.state.unload(name)
        self._send_json(200 if ok else payload["error"]["code"], payload)

    def _completion_id(self):
        return "chatcmpl-" + uuid.uuid4().hex

    def _chat_response(self, model, reply, tool_call=None):
        created = int(time.time())
        prompt_tokens = 7
        completion_tokens = len(reply.split(" "))
        message = {"role": "assistant", "content": reply}
        finish_reason = "stop"
        if tool_call is not None:
            message = {
                "role": "assistant",
                "content": None,
                "tool_calls": [tool_call_payload(tool_call)],
            }
            finish_reason = "tool_calls"
        return {
            "id": self._completion_id(),
            "object": "chat.completion",
            "created": created,
            "model": model,
            "system_fingerprint": "b0000-taia-mock",
            "choices": [
                {
                    "index": 0,
                    "message": message,
                    "finish_reason": finish_reason,
                }
            ],
            "usage": {
                "prompt_tokens": prompt_tokens,
                "completion_tokens": completion_tokens,
                "total_tokens": prompt_tokens + completion_tokens,
                "prompt_tokens_details": {"cached_tokens": 0},
            },
        }

    def _handle_chat(self, body):
        model = body.get("model", "")
        if not isinstance(model, str) or not model:
            self._send_json(400, error_body("model name is missing from the request"))
            return
        if self.state.get(model) is None:
            self._send_json(400, error_body("model '{}' not found".format(model)))
            return
        messages = body.get("messages", [])
        if not isinstance(messages, list):
            self._send_json(400, error_body("messages must be an array"))
            return
        options = body.get("stream_options", {})
        if not isinstance(options, dict):
            self._send_json(400, error_body("stream_options must be an object"))
            return
        error = validate_messages(messages)
        if error:
            self._send_json(400, error_body(error))
            return
        tools = body.get("tools", [])
        error = validate_tools(tools)
        if error:
            self._send_json(400, error_body(error))
            return
        reply = generate_reply(model, messages)
        tool_call = select_tool_call(messages, tools)
        include_usage = bool(options.get("include_usage", False))
        if body.get("stream", False):
            self._stream_chat(model, reply, include_usage, tool_call)
        else:
            self._send_json(200, self._chat_response(model, reply, tool_call))

    def _write_event(self, obj):
        self.wfile.write(("data: " + json.dumps(obj) + "\n\n").encode("utf-8"))
        self.wfile.flush()

    def _chunk(self, model, cid, created, delta, finish_reason=None):
        return {
            "choices": [{"finish_reason": finish_reason, "index": 0, "delta": delta}],
            "created": created,
            "id": cid,
            "model": model,
            "system_fingerprint": "b0000-taia-mock",
            "object": "chat.completion.chunk",
        }

    def _stream_chat(self, model, reply, include_usage, tool_call=None):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True

        cid = self._completion_id()
        created = int(time.time())
        self._write_event(
            self._chunk(model, cid, created, {"role": "assistant", "content": None})
        )
        if tool_call is not None:
            payload = tool_call_payload(tool_call)
            arguments = payload["function"]["arguments"]
            midpoint = len(arguments) // 2
            self._write_event(
                self._chunk(
                    model,
                    cid,
                    created,
                    {
                        "tool_calls": [
                            {
                                "index": 0,
                                "id": payload["id"],
                                "type": "function",
                                "function": {
                                    "name": payload["function"]["name"],
                                    "arguments": arguments[:midpoint],
                                },
                            }
                        ]
                    },
                )
            )
            if midpoint < len(arguments):
                self._write_event(
                    self._chunk(
                        model,
                        cid,
                        created,
                        {
                            "tool_calls": [
                                {
                                    "index": 0,
                                    "function": {"arguments": arguments[midpoint:]},
                                }
                            ]
                        },
                    )
                )
            self._write_event(self._chunk(model, cid, created, {}, "tool_calls"))
            if include_usage:
                self._write_event(self._usage_chunk(model, cid, created, 12))
            self.wfile.write((DONE_EVENT + chr(10) * 2).encode("utf-8"))
            self.wfile.flush()
            return

        words = reply.split(" ")
        delay = token_delay_for(self.state, model)
        sent = 0
        for index, word in enumerate(words):
            piece = word if index == len(words) - 1 else word + " "
            if model == ERROR_STREAM_MODEL and sent == 2:
                self._write_event(error_body("mock mid-stream failure", "server_error"))
                return
            self._write_event(self._chunk(model, cid, created, {"content": piece}))
            sent += 1
            if delay:
                time.sleep(delay)

        self._write_event(self._chunk(model, cid, created, {}, "stop"))
        if include_usage:
            self._write_event(self._usage_chunk(model, cid, created, len(words)))
        self.wfile.write((DONE_EVENT + chr(10) * 2).encode("utf-8"))
        self.wfile.flush()

    def _usage_chunk(self, model, cid, created, completion_tokens):
        return {
            "choices": [],
            "created": created,
            "id": cid,
            "model": model,
            "system_fingerprint": "b0000-taia-mock",
            "object": "chat.completion.chunk",
            "usage": {
                "prompt_tokens": 7,
                "completion_tokens": completion_tokens,
                "total_tokens": 7 + completion_tokens,
                "prompt_tokens_details": {"cached_tokens": 0},
            },
        }


class MockHTTPServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, address, state):
        super().__init__(address, Handler)
        self.state = state


def create_server(
    host="127.0.0.1", port=0, state=None, token_delay=0.02, load_delay=0.2
):
    if state is None:
        state = MockState(token_delay=token_delay, load_delay=load_delay)
    return MockHTTPServer((host, port), state)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default=os.environ.get("MOCK_HOST", "127.0.0.1"))
    parser.add_argument(
        "--port", type=int, default=int(os.environ.get("MOCK_PORT", "9931"))
    )
    parser.add_argument(
        "--token-delay",
        type=float,
        default=float(os.environ.get("MOCK_TOKEN_DELAY", "0.02")),
    )
    parser.add_argument(
        "--load-delay",
        type=float,
        default=float(os.environ.get("MOCK_LOAD_DELAY", "0.2")),
    )
    args = parser.parse_args()
    server = create_server(
        args.host, args.port, token_delay=args.token_delay, load_delay=args.load_delay
    )
    host, port = server.server_address[0], server.server_address[1]
    print("taia mock listening on http://{}:{}".format(host, port), flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
