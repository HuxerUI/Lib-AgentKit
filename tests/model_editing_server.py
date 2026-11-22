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
    lock = threading.Lock()

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_args):
            pass

        def do_POST(self):
            endpoint = self.path.split("/")[1]
            request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            if (endpoint not in ("original", "updated") or self.path != f"/{endpoint}/v1/chat/completions"
                    or request.get("model") != "editable-chat"
                    or self.headers.get("Authorization") != f"Bearer revision-fixture-key-{endpoint}"):
                print(f"Unexpected endpoint, model or credential for {self.path}", file=sys.stderr)
                self.send_error(403)
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
            body = stream(content)
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

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
    return 0


if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as directory:
        sys.exit(verify(sys.argv[1], pathlib.Path(directory)))
