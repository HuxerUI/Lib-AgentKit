import http.server
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import threading


def verify(executable, fixture, legacy):
    calls = []
    failures = []
    path = "/mcp-legacy" if legacy else "/mcp"
    tool_count = 65
    tools = [{
        "name": f"release_fixture_{index}",
        "description": f"Release fixture tool {index}",
        "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
    } for index in range(tool_count - 1)]
    tools.append({
        "name": "get_release_count", "description": "Read the number of published releases",
        "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
    })

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_args):
            pass

        def respond(self, content, content_type="application/json", headers=()):
            self.send_response(200)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(content)))
            for name, value in headers:
                self.send_header(name, value)
            self.end_headers()
            self.wfile.write(content)

        def do_POST(self):
            try:
                request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                if self.path == path:
                    self.mcp(request)
                elif self.path == "/v1/chat/completions":
                    self.chat(request)
                else:
                    raise ValueError(f"Unexpected endpoint {self.path}")
            except (KeyError, ValueError, AssertionError, StopIteration) as error:
                failures.append(str(error))
                self.send_error(400)

        def mcp(self, request):
            method = request["method"]
            calls.append(method)
            assert request["jsonrpc"] == "2.0", "Malformed MCP envelope"
            if legacy:
                if method == "initialize":
                    assert request["params"]["protocolVersion"] == "2025-11-25", "Wrong legacy initialization version"
                    body = {"jsonrpc": "2.0", "id": request["id"], "result": {
                        "protocolVersion": "2025-11-25", "capabilities": {"tools": {}},
                        "serverInfo": {"name": "Release fixture", "version": "1"},
                    }}
                    self.respond(json.dumps(body).encode(), headers=(("Mcp-Session-Id", "release-fixture"),))
                    return
                assert self.headers["Mcp-Protocol-Version"] == "2025-11-25", "Wrong negotiated version"
                assert self.headers["Mcp-Session-Id"] == "release-fixture", "Lost the legacy session ID"
                if method == "notifications/initialized":
                    self.send_response(202)
                    self.end_headers()
                    return
            else:
                assert self.headers["Mcp-Method"] == method, "Missing MCP method header"
                assert self.headers["Mcp-Protocol-Version"] == "2026-07-28", "Unexpected MCP protocol"
            if method == "tools/list":
                result = {"tools": tools}
            elif method == "tools/call":
                assert (fixture / "mcp-approved").exists(), "The HTTP request preceded approval"
                assert request["params"]["name"] == "get_release_count", "Wrong MCP tool"
                assert request["params"]["arguments"] == {}, "Wrong MCP tool arguments"
                (fixture / "mcp-called").touch()
                result = {"content": [], "structuredContent": {"release_count": 7}}
            else:
                raise ValueError(f"Unexpected MCP method {method}")
            if not legacy:
                result["resultType"] = "complete"
            body = {"jsonrpc": "2.0", "id": request["id"], "result": result}
            if method == "tools/call":
                self.respond(f": progress\r\n\r\ndata: {json.dumps(body)}\r\n\r\n".encode(), "text/event-stream")
            else:
                self.respond(json.dumps(body).encode())

        def chat(self, request):
            history = request["messages"]
            assert "Use the revised release checklist." in json.dumps(history), "Active Skill instructions were omitted"
            if "response_format" in request:
                delta = {"content": '{"route":"answer"}'}
                reason = "stop"
            elif any(message["role"] == "tool" for message in history):
                assert '"release_count":7' in json.dumps(history).replace("\\", "").replace(" ", ""), "MCP result was omitted"
                delta = {"content": "The MCP fixture reports seven releases."}
                reason = "stop"
            else:
                descriptions = {entry["function"]["description"] for entry in request["tools"]}
                assert all(tool["description"] in descriptions for tool in tools), "The model did not receive all 65 MCP tools"
                tool = next(entry["function"] for entry in request["tools"]
                            if entry["function"]["description"] == "Read the number of published releases")
                delta = {"tool_calls": [{"index": 0, "id": "release-count", "type": "function",
                                         "function": {"name": tool["name"], "arguments": "{}"}}]}
                reason = "tool_calls"
            events = [{"choices": [{"index": 0, "delta": delta}]},
                      {"choices": [{"index": 0, "delta": {}, "finish_reason": reason}]}]
            payload = "".join(f"data: {json.dumps(event)}\n\n" for event in events) + "data: [DONE]\n\n"
            self.respond(payload.encode(), "text/event-stream")

    with http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler) as server:
        worker = threading.Thread(target=server.serve_forever)
        worker.start()
        base = f"http://127.0.0.1:{server.server_port}"
        env = {key: value for key, value in os.environ.items() if not key.startswith("HUXERUI_AGENTKIT_")}
        env.update(HUXERUI_AGENTKIT_MODEL="extension-fixture", HUXERUI_AGENTKIT_BASE_URL=base + "/v1",
                   HUXERUI_AGENTKIT_PROVIDER="compatible", HUXERUI_AGENTKIT_PROTOCOL="chat-completions")
        try:
            result = subprocess.run([executable, "--extensions", str(fixture), base + path], env=env, timeout=70)
        finally:
            server.shutdown()
            worker.join()
    if failures:
        raise RuntimeError(failures)
    if result.returncode:
        return result.returncode
    expected = ["tools/list", "tools/call"]
    if legacy:
        expected = ["initialize", "notifications/initialized"] + expected
    if calls != expected:
        raise RuntimeError(f"Unexpected MCP requests: {calls}")
    return 0


if __name__ == "__main__":
    for legacy in (False, True):
        with tempfile.TemporaryDirectory() as directory:
            result = verify(sys.argv[1], pathlib.Path(directory), legacy)
            if result:
                sys.exit(result)
