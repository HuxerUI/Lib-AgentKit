import argparse
import http.server
import json
import pathlib
import re
import subprocess
import tempfile
import threading
import time

from model_editing_server import stream


def verify(executable, root, real_profile):
    calls = []
    reviews = []
    errors = []

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_args):
            pass

        def reply(self, payload, content_type="application/json"):
            self.send_response(200)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

        def do_POST(self):
            try:
                request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                if self.path == "/mcp":
                    self.mcp(request)
                elif self.path == "/v1/chat/completions" and not real_profile:
                    self.review(request)
                else:
                    raise ValueError("Unexpected test endpoint")
            except (KeyError, ValueError, AssertionError) as error:
                errors.append(str(error))
                self.send_error(400)
            except (BrokenPipeError, ConnectionResetError):
                pass

        def mcp(self, request):
            method = request["method"]
            assert self.headers.get("Mcp-Method") == method
            if method == "tools/list":
                result = {"resultType": "complete", "tools": [{
                    "name": "record_review",
                    "description": "Record one review marker in the local fixture.",
                    "inputSchema": {"type": "object", "required": ["marker"], "additionalProperties": False,
                                    "properties": {"marker": {"type": "string"}}},
                }]}
            elif method == "tools/call":
                assert request["params"]["name"] == "record_review"
                marker = request["params"]["arguments"]["marker"]
                assert marker in ("mcp-allowed", "mcp-manual"), "A denied MCP operation reached the server"
                calls.append(marker)
                (root / (marker + ".called")).write_text("recorded", encoding="utf-8")
                result = {"content": [], "structuredContent": {"recorded": marker}, "resultType": "complete"}
            else:
                raise ValueError("Unexpected MCP method")
            self.reply(json.dumps({"jsonrpc": "2.0", "id": request["id"], "result": result}).encode())

        def review(self, request):
            contents = json.dumps(request["messages"])
            assert "user_request" in contents and "prepared_summary" in contents, "Incomplete review facts"
            match = re.search(r"(file|powershell|mcp)-(allowed|denied|manual|timeout)", contents)
            assert match, "Missing prepared review target"
            marker, disposition = match.group(), match.group(2)
            reviews.append(marker)
            if disposition == "timeout":
                time.sleep(18)
            decisions = {"allowed": "approve", "denied": "deny", "manual": "defer", "timeout": "approve"}
            self.reply(stream(json.dumps({"decision": decisions[disposition]})), "text/event-stream")

    with http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler) as server:
        base = f"http://127.0.0.1:{server.server_port}"
        profile = json.loads(real_profile.read_text(encoding="utf-8-sig")) if real_profile else {
            "model": "review-fixture", "protocol": "chat-completions", "provider": "compatible",
            "base_url": base + "/v1",
        }
        profile.update({"profile_id": "tool-review-acceptance", "revision": 1})
        (root / "models").mkdir()
        (root / "models" / "review.json").write_text(json.dumps(profile), encoding="utf-8")
        (root / "settings.json").write_text(json.dumps({
            "review_model": {"id": profile["profile_id"], "version": profile["revision"]},
            "mcp_servers": [{"id": "1", "name": "Review fixture", "url": base + "/mcp"}],
        }), encoding="utf-8")
        worker = threading.Thread(target=server.serve_forever)
        worker.start()
        try:
            args = [executable, "--tool-auto-review", str(root)]
            if not real_profile:
                args.append("with-timeout")
            result = subprocess.run(args, timeout=180, check=False)
        finally:
            server.shutdown()
            worker.join()
    if result.returncode:
        return result.returncode
    if errors or calls != ["mcp-allowed", "mcp-manual"]:
        raise RuntimeError(f"Unexpected MCP execution markers: {calls}, errors: {errors}")
    if not real_profile:
        expected = [f"{tool}-{case}" for tool in ("file", "powershell", "mcp")
                    for case in ("allowed", "denied", "manual")] + ["file-timeout"]
        if reviews != expected:
            raise RuntimeError(f"Unexpected review requests: {reviews}")
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("executable")
    parser.add_argument("--review-profile", type=pathlib.Path)
    parser.add_argument("--directory", type=pathlib.Path)
    args = parser.parse_args()
    if args.directory:
        args.directory.mkdir(parents=True, exist_ok=False)
        raise SystemExit(verify(args.executable, args.directory, args.review_profile))
    with tempfile.TemporaryDirectory(prefix="ganwork-tool-auto-review-") as directory:
        raise SystemExit(verify(args.executable, pathlib.Path(directory), args.review_profile))
