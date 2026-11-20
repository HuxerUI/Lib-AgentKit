import json
import os
import subprocess
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


VERSION = "2026-07-28"
META = "io.modelcontextprotocol/"


class CatalogServer(ThreadingHTTPServer):
    def __init__(self):
        super().__init__(("127.0.0.1", 0), CatalogHandler)
        self.revision = 1
        self.ids = set()
        self.lists = []
        self.listeners = []
        self.calls = []
        self.errors = []
        self.lock = threading.Lock()
        self.refreshed = {2: threading.Event(), 3: threading.Event()}
        self.first_report_started = threading.Event()
        self.second_report_ready = threading.Event()
        self.release_first = threading.Event()

    def verify(self):
        if self.errors:
            raise RuntimeError("; ".join(self.errors))
        expected_lists = [(1, None), (1, "page-two"), (2, None), (2, "page-two"), (3, None)]
        if self.lists != expected_lists or self.listeners != ["first-listener", "second-listener"]:
            raise RuntimeError(f"Unexpected MCP catalog requests: {self.lists}, {self.listeners}")
        if (len(self.calls) != 3 or self.calls[0] != (2, "report", {}) or
                not all((2, "report", {"section": section}) in self.calls[1:] for section in ("first", "second")) or
                not self.release_first.is_set()):
            raise RuntimeError(f"Unexpected approved report requests: {self.calls}")


def require(condition, message):
    if not condition:
        raise ValueError(message)


class CatalogHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, format_string, *args):
        pass

    def send_json(self, status, message):
        body = json.dumps(message).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True
        self.wfile.write(body)

    def open_events(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Transfer-Encoding", "chunked")
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True
        self.send_chunk(b": MCP fixture\r\n\r\n")

    def send_chunk(self, data):
        self.wfile.write(f"{len(data):x}\r\n".encode("ascii") + data + b"\r\n")
        self.wfile.flush()

    def send_event(self, value):
        self.send_chunk(b"data: " + json.dumps(value).encode("utf-8") + b"\r\n\r\n")

    def do_POST(self):
        try:
            self.exchange()
        except (KeyError, ValueError) as error:
            with self.server.lock:
                self.server.errors.append(str(error))
            self.send_json(400, {"jsonrpc": "2.0", "error": {"code": -32600, "message": str(error)}})

    def do_GET(self):
        if (self.path != "/mcp/test/release-first" or not self.server.second_report_ready.is_set() or
                self.server.release_first.is_set()):
            self.send_json(409, {"error": "The second report must finish before releasing the first"})
            return
        self.server.release_first.set()
        self.send_json(200, {})

    def exchange(self):
        require(self.path == "/mcp" and self.headers.get("Origin") is None, "Invalid MCP endpoint or origin")
        require(self.headers.get("Authorization") == "Bearer agentkit-test", "MCP test credential missing")
        accept = self.headers.get("Accept", "")
        require("application/json" in accept and "text/event-stream" in accept, "Missing MCP Accept types")
        length = int(self.headers["Content-Length"])
        require(0 < length < 65536, "Invalid MCP request length")
        request = json.loads(self.rfile.read(length))
        method, request_id, params = request["method"], request["id"], request["params"]
        meta = params["_meta"]
        require(request["jsonrpc"] == "2.0" and request_id, "Invalid JSON-RPC envelope")
        require(self.headers.get("Mcp-Method") == method and
                self.headers.get("Mcp-Protocol-Version") == VERSION == meta[META + "protocolVersion"],
                "MCP transport headers disagree with the request")
        require(meta[META + "clientCapabilities"] == {}, "Unexpected MCP client capabilities")
        require(meta[META + "clientInfo"] == {"name": "AgentKit HTTP Test", "version": "1"}, "Missing client identity")
        with self.server.lock:
            require(request_id not in self.server.ids, "Duplicate JSON-RPC request ID")
            self.server.ids.add(request_id)

        def complete(result):
            return {"jsonrpc": "2.0", "id": request_id, "result": {"resultType": "complete", **result}}

        if method == "subscriptions/listen":
            self.subscribe(request_id, params, complete)
        elif method == "tools/list":
            self.list_tools(params, complete)
        elif method == "tools/call":
            self.call_tool(params, complete)
        else:
            raise ValueError(f"Unexpected MCP method {method}")

    def subscribe(self, request_id, params, complete):
        requested = {"toolsListChanged": True}
        require(params["notifications"] == requested, "Unexpected notification filter")
        with self.server.lock:
            self.server.listeners.append(request_id)
            self.server.revision += 1
            revision = self.server.revision
        subscription = {META + "subscriptionId": request_id}
        self.open_events()
        self.send_event({"jsonrpc": "2.0", "method": "notifications/subscriptions/acknowledged",
                         "params": {"_meta": subscription, "notifications": requested}})
        self.send_event({"jsonrpc": "2.0", "method": "notifications/tools/list_changed",
                         "params": {"_meta": subscription}})
        if not self.server.refreshed[revision].wait(timeout=6):
            with self.server.lock:
                self.server.errors.append("Client did not refresh while the subscription was open")
            self.send_chunk(b"")
            return
        self.send_event(complete({"_meta": subscription}))
        self.send_chunk(b"")

    def list_tools(self, params, complete):
        cursor = params.get("cursor")
        require(cursor in (None, "page-two"), "Unexpected MCP cursor")
        with self.server.lock:
            revision = self.server.revision
            self.server.lists.append((revision, cursor))
        if revision == 3:
            self.send_json(403, {"jsonrpc": "2.0", "error": {"code": -32000, "message": "Scope withdrawn"}})
            self.server.refreshed[revision].set()
            return
        first = cursor is None
        tool = {"name": "report" if first else "private",
                "description": f"Report revision {revision}" if first else "Private tool",
                "inputSchema": {"type": "object"}, "outputSchema": {"type": "object"},
                "annotations": {"readOnlyHint": True}}
        result = {"tools": [tool]}
        if first:
            result["nextCursor"] = "page-two"
        self.send_json(200, complete(result))
        if revision == 2 and not first:
            self.server.refreshed[revision].set()

    def call_tool(self, params, complete):
        args = params["arguments"]
        section = args.get("section")
        require(args == {} or (section in ("first", "second") and args == {"section": section}),
                "Unexpected report scope")
        with self.server.lock:
            revision = self.server.revision
            self.server.calls.append((revision, params["name"], args))
        require(self.headers.get("Mcp-Name") == params["name"] == "report", "MCP tool name header mismatch")
        require(revision == 2, "Stale MCP invocation reached the server")
        token = params["_meta"]["progressToken"]
        self.open_events()
        self.send_event({"jsonrpc": "2.0", "method": "notifications/progress",
                         "params": {"progressToken": token, "progress": 1, "total": 1}})
        if section == "first":
            self.server.first_report_started.set()
            require(self.server.release_first.wait(timeout=6), "The first report was not released")
        elif section == "second":
            require(self.server.first_report_started.wait(timeout=6), "The reports did not execute concurrently")
            self.server.second_report_ready.set()
        output = {"pages": 3, "revision": revision, **args}
        self.send_event(complete({"content": [], "structuredContent": output}))
        self.send_chunk(b"")


def main():
    with CatalogServer() as server:
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            endpoint = f"http://127.0.0.1:{server.server_port}/mcp"
            env = {**os.environ, "NO_PROXY": "127.0.0.1", "no_proxy": "127.0.0.1"}
            run = subprocess.run([sys.argv[1], "--mcp-http", endpoint], env=env, timeout=35)
            require(run.returncode == 0, f"Native MCP host exited with {run.returncode}: {server.errors}")
            server.verify()
        finally:
            server.shutdown()
            worker.join()


if __name__ == "__main__":
    main()
