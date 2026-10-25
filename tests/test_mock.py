import json
import threading
import time
import unittest
import urllib.error
import urllib.request

from mock.server import (
    ERROR_STREAM_MODEL,
    FAIL_LOAD_MODEL,
    MockState,
    create_server,
)


class MockServerTest(unittest.TestCase):
    def setUp(self):
        self.state = MockState(token_delay=0.0, load_delay=0.02)
        self.server = create_server("127.0.0.1", 0, state=self.state)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        host, port = self.server.server_address[:2]
        self.base = "http://{}:{}".format(host, port)

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)

    def request(self, method, path, body=None):
        data = None
        headers = {}
        if body is not None:
            data = json.dumps(body).encode("utf-8")
            headers["Content-Type"] = "application/json"
        req = urllib.request.Request(
            self.base + path, data=data, headers=headers, method=method
        )
        try:
            with urllib.request.urlopen(req, timeout=5) as resp:
                return resp.status, json.loads(resp.read().decode("utf-8"))
        except urllib.error.HTTPError as exc:
            with exc:
                return exc.code, json.loads(exc.read().decode("utf-8"))

    def stream(self, body):
        data = json.dumps(body).encode("utf-8")
        req = urllib.request.Request(
            self.base + "/v1/chat/completions",
            data=data,
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        events = []
        with urllib.request.urlopen(req, timeout=5) as resp:
            for raw in resp:
                line = raw.decode("utf-8").strip()
                if not line.startswith("data:"):
                    continue
                events.append(line[len("data:") :].strip())
        return events

    def model_status(self, name):
        _, body = self.request("GET", "/models")
        for model in body["data"]:
            if model["id"] == name:
                return model["status"]
        self.fail("model {} not found".format(name))

    def wait_status(self, name, desired, timeout=2.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            status = self.model_status(name)
            if status["value"] in desired:
                return status
            time.sleep(0.01)
        self.fail("model {} did not reach {}".format(name, desired))

    def test_health(self):
        status, body = self.request("GET", "/health")
        self.assertEqual(status, 200)
        self.assertEqual(body, {"status": "ok"})

    def test_health_while_loading(self):
        self.state.load_delay = 0.3
        self.request("POST", "/models/load", {"model": "mock/small"})
        status, body = self.request("GET", "/health")
        self.assertEqual(status, 503)
        self.assertEqual(body["error"]["type"], "unavailable_error")
        self.wait_status("mock/small", {"loaded"})
        status, body = self.request("GET", "/health")
        self.assertEqual(status, 200)

    def test_list_models(self):
        status, body = self.request("GET", "/models")
        self.assertEqual(status, 200)
        self.assertEqual(body["object"], "list")
        ids = {m["id"] for m in body["data"]}
        self.assertIn("mock/small", ids)
        for model in body["data"]:
            self.assertEqual(model["status"]["value"], "unloaded")

    def test_load_unknown_model(self):
        status, body = self.request("POST", "/models/load", {"model": "nope"})
        self.assertEqual(status, 404)
        self.assertEqual(body["error"]["type"], "not_found_error")

    def test_load_and_poll(self):
        status, body = self.request("POST", "/models/load", {"model": "mock/small"})
        self.assertEqual(status, 200)
        self.assertTrue(body["success"])
        status = self.wait_status("mock/small", {"loaded"})
        self.assertEqual(status["value"], "loaded")

    def test_load_already_running(self):
        self.request("POST", "/models/load", {"model": "mock/large"})
        self.wait_status("mock/large", {"loaded"})
        status, body = self.request("POST", "/models/load", {"model": "mock/large"})
        self.assertEqual(status, 400)
        self.assertEqual(body["error"]["type"], "invalid_request_error")

    def test_load_failure(self):
        status, body = self.request("POST", "/models/load", {"model": FAIL_LOAD_MODEL})
        self.assertEqual(status, 200)
        result = self.wait_status(FAIL_LOAD_MODEL, {"unloaded"})
        self.assertTrue(result.get("failed"))
        self.assertEqual(result["exit_code"], 1)

    def test_unload_not_running(self):
        status, body = self.request("POST", "/models/unload", {"model": "mock/small"})
        self.assertEqual(status, 400)
        self.assertEqual(body["error"]["type"], "invalid_request_error")

    def test_unload_running(self):
        self.request("POST", "/models/load", {"model": "mock/small"})
        self.wait_status("mock/small", {"loaded"})
        status, body = self.request("POST", "/models/unload", {"model": "mock/small"})
        self.assertEqual(status, 200)
        self.assertTrue(body["success"])
        self.wait_status("mock/small", {"unloaded"})

    def test_chat_non_stream(self):
        status, body = self.request(
            "POST",
            "/v1/chat/completions",
            {"model": "mock/small", "messages": [{"role": "user", "content": "hi"}]},
        )
        self.assertEqual(status, 200)
        self.assertEqual(body["object"], "chat.completion")
        choice = body["choices"][0]
        self.assertEqual(choice["message"]["role"], "assistant")
        self.assertIn("echo: hi", choice["message"]["content"])
        self.assertEqual(choice["finish_reason"], "stop")

    def test_chat_stream(self):
        events = self.stream(
            {
                "model": "mock/slow-stream",
                "stream": True,
                "messages": [{"role": "user", "content": "hello"}],
            }
        )
        last = events[len(events) - 1]
        self.assertEqual(last, chr(91) + "DONE" + chr(93))
        chunks = [json.loads(e) for e in events[: len(events) - 1]]
        self.assertEqual(chunks[0]["object"], "chat.completion.chunk")
        first = chunks[0]["choices"][0]["delta"]
        self.assertEqual(first["role"], "assistant")
        self.assertIsNone(first["content"])
        text = ""
        finish = None
        for chunk in chunks[1:]:
            choice = chunk["choices"][0]
            if choice["finish_reason"]:
                finish = choice["finish_reason"]
            else:
                text += choice["delta"].get("content") or ""
        self.assertIn("echo: hello", text)
        self.assertEqual(finish, "stop")

    def test_chat_stream_usage(self):
        events = self.stream(
            {
                "model": "mock/small",
                "stream": True,
                "stream_options": {"include_usage": True},
                "messages": [{"role": "user", "content": "hi"}],
            }
        )
        done = chr(91) + "DONE" + chr(93)
        chunks = [json.loads(e) for e in events if e != done]
        self.assertTrue(any("usage" in chunk for chunk in chunks))

    def test_chat_stream_error(self):
        events = self.stream(
            {
                "model": ERROR_STREAM_MODEL,
                "stream": True,
                "messages": [{"role": "user", "content": "hi"}],
            }
        )
        self.assertFalse(any("DONE" in event for event in events))
        self.assertTrue(any("error" in json.loads(event) for event in events))

    def test_chat_unknown_model(self):
        status, body = self.request(
            "POST",
            "/v1/chat/completions",
            {"model": "nope", "messages": [{"role": "user", "content": "hi"}]},
        )
        self.assertEqual(status, 400)
        self.assertEqual(body["error"]["type"], "invalid_request_error")


if __name__ == "__main__":
    unittest.main()
