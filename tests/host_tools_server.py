import http.server
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import threading
import urllib.parse


def verify(executable, root):
    calls = []
    token = "host-tools-fixture-key"

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_args):
            pass

        def do_GET(self):
            url = urllib.parse.urlsplit(self.path)
            query = urllib.parse.parse_qs(url.query)
            calls.append((url.path, query, self.headers.get("X-Subscription-Token")))
            if url.path == "/search":
                body = json.dumps({"web": {"results": [{
                    "title": "GanWork <b>documentation</b>",
                    "url": f"http://127.0.0.1:{self.server.server_port}/page",
                    "description": "A relevant page",
                }]}}).encode()
                mime_type = "application/json"
            elif url.path == "/page":
                body = (b"<!doctype html><html><head><style>hidden-style-marker</style></head><body>"
                        b"<h1>GanWork &amp; docs</h1><p>visible paragraph</p>"
                        b"<script>untrusted-script-marker</script></body></html>")
                mime_type = "text/html; charset=utf-8"
            elif url.path == "/redirect":
                self.send_response(302)
                self.send_header("Location", "/redirect-destination")
                self.end_headers()
                return
            else:
                self.send_error(403)
                return
            self.send_response(200)
            self.send_header("Content-Type", mime_type)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    environment = os.environ.copy()
    environment.pop("GANWORK_WEB_SEARCH_API_KEY", None)
    environment.pop("GANWORK_WEB_SEARCH_ENDPOINT", None)
    (root / "no-search").mkdir()
    subprocess.run([executable, "--host-tools", str(root / "no-search")], env=environment, timeout=60, check=True)

    with http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler) as server:
        worker = threading.Thread(target=server.serve_forever)
        worker.start()
        base = f"http://127.0.0.1:{server.server_port}"
        environment.update(GANWORK_WEB_SEARCH_API_KEY=token, GANWORK_WEB_SEARCH_ENDPOINT=f"{base}/search")
        (root / "with-search").mkdir()
        try:
            subprocess.run([executable, "--host-tools", str(root / "with-search"), base],
                           env=environment, timeout=60, check=True)
        finally:
            server.shutdown()
            worker.join()

    expected = [("/search", {"q": ["GanWork \u6d4b\u8bd5 + &"], "count": ["1"]}, token),
                ("/page", {}, None), ("/redirect", {}, None)]
    if calls != expected:
        raise RuntimeError(f"Unexpected web requests: {calls}")


if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as directory:
        verify(sys.argv[1], pathlib.Path(directory))
