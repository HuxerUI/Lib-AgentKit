import http.server
import json
import pathlib
import subprocess
import sys
import tempfile
import threading
import time


def stream(content):
    text = {"choices": [{"index": 0, "delta": {"content": content}}]}
    end = {"choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}]}
    return f"data: {json.dumps(text)}\n\ndata: {json.dumps(end)}\n\ndata: [DONE]\n\n".encode()


def verify(executable, fixture):
    calls = []
    probes = []
    expert_calls = []
    lock = threading.Lock()

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_args):
            pass

        def do_GET(self):
            if (self.path != "/original/v1/models"
                    or self.headers.get("Authorization") != "Bearer revision-fixture-key-original"):
                self.send_error(403)
                return
            body = json.dumps({"data": [{"id": "editable-chat"}]}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def send_stream(self, content):
            body = stream(content)
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_POST(self):
            endpoint = self.path.split("/")[1]
            request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            if self.path == "/expert/v1/chat/completions":
                self.handle_expert_request(request)
                return
            probe = request.get("messages") == [{"role": "user", "content": "Reply with OK."}]
            if (probe and self.path == "/original/v1/chat/completions" and request.get("model") == "editable-chat"
                    and self.headers.get("Authorization") == "Bearer invalid-fixture-key"):
                with lock:
                    probes.append("rejected")
                self.send_error(401)
                return
            if (endpoint not in ("original", "updated") or self.path != f"/{endpoint}/v1/chat/completions"
                    or request.get("model") != "editable-chat"
                    or self.headers.get("Authorization") != f"Bearer revision-fixture-key-{endpoint}"):
                print(f"Unexpected endpoint, model or credential for {self.path}", file=sys.stderr)
                self.send_error(403)
                return
            if probe:
                if "tools" in request or "response_format" in request or endpoint != "original":
                    self.send_error(400)
                    return
                with lock:
                    probes.append("verified")
                self.send_stream("OK")
                return
            classify = "response_format" in request
            with lock:
                calls.append((endpoint, classify))
                number = calls.count((endpoint, False))
            if endpoint == "original" and classify:
                (fixture / "original-started").touch()
                deadline = time.monotonic() + 35
                while not (fixture / "release-original").exists():
                    if time.monotonic() >= deadline:
                        self.send_error(504)
                        return
                    time.sleep(0.02)
            if classify:
                content = '{"route":"answer"}'
            elif endpoint == "original":
                content = "Original endpoint reply."
            else:
                content = f"Updated endpoint reply {number}."
            self.send_stream(content)

        def handle_expert_request(self, request):
            if self.headers.get("Authorization") != "Bearer revision-fixture-key-original":
                self.send_error(401)
                return
            model = request.get("model")
            messages = request.get("messages", [])
            text = json.dumps(messages)
            if "Classify this request" in text:
                phase, content = "route", '{"route":"expert-team"}'
            elif "Plan independent" in text:
                if not all(f"ganwork.expert.{name}" in text for name in ("research", "verification", "code-review", "documentation")):
                    self.send_error(400, "Missing expert profiles")
                    return
                assignments = ([("documentation", "Check documentation evidence.", "reasoning")]
                               if "documentation-only expert pass" in text else [
                                   ("code-review", "Review the code evidence.", "code"),
                                   ("verification", "Check verification evidence.", "reasoning")])
                phase, content = "plan", json.dumps({
                    "assignments": [{"expert": f"ganwork.expert.{name}", "task": task, "focus": focus,
                                     "include_attachments": False} for name, task, focus in assignments],
                    "parent_action": False})
            elif "Review the code evidence." in text and "Inspect implementation" in text:
                phase, content = "code-review", "Code evidence."
            elif "Check verification evidence." in text and "Independently check behavior" in text:
                phase, content = "verification", "Verification evidence."
            elif "Check documentation evidence." in text and "Check documentation, examples" in text:
                phase, content = "documentation", "Documentation evidence."
            elif "Code evidence." in text and "Verification evidence." in text:
                phase, content = "conclusion", "Both experts reported evidence."
            elif "Documentation evidence." in text:
                phase, content = "conclusion", "The documentation expert reported evidence."
            else:
                self.send_error(400)
                return
            with lock:
                expert_calls.append((phase, model))
            self.send_stream(content)

    with http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler) as server:
        worker = threading.Thread(target=server.serve_forever)
        worker.start()
        try:
            result = subprocess.run([executable, "--model-editing", str(fixture), f"http://127.0.0.1:{server.server_port}"],
                                    timeout=100)
        finally:
            (fixture / "release-original").touch()
            server.shutdown()
            worker.join()
    if result.returncode:
        return result.returncode
    expected = [("original", True), ("original", False)] + [("updated", True), ("updated", False)] * 2
    if calls != expected:
        raise RuntimeError(f"Unexpected endpoint sequence: {calls}")
    if probes != ["rejected", "verified", "verified"]:
        raise RuntimeError(f"Unexpected connection checks: {probes}")
    if sorted(expert_calls) != sorted([
            ("route", "expert-verification"), ("plan", "expert-verification"), ("code-review", "expert-research"),
            ("verification", "expert-verification"), ("conclusion", "expert-verification"),
            ("route", "expert-verification"), ("plan", "expert-verification"), ("documentation", "expert-verification"),
            ("conclusion", "expert-verification")]):
        raise RuntimeError(f"Unexpected expert model calls: {expert_calls}")
    return 0


if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as directory:
        sys.exit(verify(sys.argv[1], pathlib.Path(directory)))
