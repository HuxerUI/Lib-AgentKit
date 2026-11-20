import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time


META = "io.modelcontextprotocol/"
VERSION = "2026-07-28"
CRASHED = 17


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def send(message):
    wire = json.dumps(message, ensure_ascii=False).encode("utf-8")
    sys.stdout.buffer.write(wire + b"\n")
    sys.stdout.buffer.flush()


def serve(generation, evidence):
    print(f"MCP fixture process {os.getpid()} connected", file=sys.stderr)
    revision = generation
    listener = None
    request_ids = set()
    calls = 0
    for line in sys.stdin.buffer:
        require(line.endswith(b"\n") and len(line) < 65536, "Invalid MCP stdio frame")
        request = json.loads(line)
        method, params = request["method"], request["params"]
        require(request["jsonrpc"] == "2.0", "Invalid MCP envelope")
        if method == "notifications/cancelled":
            require("id" not in request and listener == params["requestId"], "Cancellation did not refer to the live subscription")
            with evidence.open("a", encoding="utf-8") as log:
                log.write(json.dumps([os.getpid(), method, listener]) + "\n")
            listener = None
            continue

        request_id = request["id"]
        meta = params["_meta"]
        require(request_id and request_id not in request_ids, "Duplicate or missing MCP request ID")
        request_ids.add(request_id)
        require(meta[META + "protocolVersion"] == VERSION and meta[META + "clientCapabilities"] == {} and
                meta[META + "clientInfo"] == {"name": "AgentKit stdio Test", "version": "1"}, "MCP request metadata was lost")

        def complete(result):
            send({"jsonrpc": "2.0", "id": request_id, "result": {"resultType": "complete", **result}})

        if method == "subscriptions/listen":
            expected = "original-listener" if generation == 1 else "restarted-listener"
            require(listener is None and request_id == expected, "MCP client did not establish the expected subscription")
            notifications = {"toolsListChanged": True}
            require(params["notifications"] == notifications, "Wrong MCP subscription filter")
            listener = request_id
            revision += 1
            subscription = {META + "subscriptionId": listener}
            send({"jsonrpc": "2.0", "method": "notifications/subscriptions/acknowledged",
                  "params": {"_meta": subscription, "notifications": notifications}})
            send({"jsonrpc": "2.0", "method": "notifications/tools/list_changed", "params": {"_meta": subscription}})
        elif method == "tools/list":
            require(generation == 1 or listener is not None, "The restarted MCP process was used without subscribing")
            require("cursor" not in params, "Unexpected cursor on the stdio catalog")
            complete({"tools": [{"name": "report", "description": f"Report revision {revision}",
                                 "inputSchema": {"type": "object"}, "outputSchema": {"type": "object"}}]})
        elif method == "tools/call":
            require(listener is not None and revision == generation + 1 and
                    params["name"] == "report" and params["arguments"] == {}, "A stale MCP call reached the child process")
            calls += 1
            with evidence.open("a", encoding="utf-8") as log:
                log.write(json.dumps([os.getpid(), method, revision, request_id]) + "\n")
            send({"jsonrpc": "2.0", "method": "notifications/progress",
                  "params": {"progressToken": meta["progressToken"], "progress": 1, "total": 1}})
            if generation == 1 and calls == 2:
                return CRASHED
            complete({"content": [], "structuredContent": {"pages": 3, "revision": revision}})
        else:
            raise RuntimeError(f"Unexpected MCP method {method}")

    require(generation == 2 and calls == 1 and listener is None, "The MCP client did not cancel and close the restarted process")
    return 0


def verify(runtime, evidence):
    open_fds = set()
    processes = []
    deadline = time.monotonic() + 35

    def pipe():
        fds = os.pipe()
        open_fds.update(fds)
        return fds

    def close(*fds):
        for fd in fds:
            os.close(fd)
            open_fds.remove(fd)

    def stop_after_exit(process):
        return process.wait(timeout=max(0, deadline - time.monotonic()))

    def start_peer(generation, read_fd, write_fd):
        process = subprocess.Popen([sys.executable, __file__, "--peer", str(generation), str(evidence)],
                                   stdin=read_fd, stdout=write_fd)
        processes.append(process)
        close(read_fd, write_fd)
        return process

    try:
        first_in, first_send = pipe()
        first_receive, first_out = pipe()
        next_in, next_send = pipe()
        next_receive, next_out = pipe()
        client_fds = [first_receive, first_send, next_receive, next_send]

        if os.name == "nt":
            import msvcrt

            native_handles = [msvcrt.get_osfhandle(fd) for fd in client_fds]
            for handle in native_handles:
                os.set_handle_inheritable(handle, True)
            startup = subprocess.STARTUPINFO(lpAttributeList={"handle_list": native_handles})
            client_options = {"startupinfo": startup, "close_fds": True}
        else:
            native_handles = client_fds
            client_options = {"pass_fds": client_fds}

        client = subprocess.Popen([runtime, "--mcp-stdio", *(str(handle) for handle in native_handles)], **client_options)
        processes.append(client)
        close(*client_fds)
        first = start_peer(1, first_in, first_out)
        require(stop_after_exit(first) == CRASHED, "First MCP process did not exit after its interrupted write")
        restarted = start_peer(2, next_in, next_out)
        require(stop_after_exit(client) == 0, "Native stdio MCP host failed")
        require(stop_after_exit(restarted) == 0, "The restarted MCP process did not shut down on EOF")

        records = [json.loads(line) for line in evidence.read_text(encoding="utf-8").splitlines()]
        require([entry[:3] for entry in records] == [[first.pid, "tools/call", 2], [first.pid, "tools/call", 2],
                 [restarted.pid, "tools/call", 3], [restarted.pid, "notifications/cancelled", "restarted-listener"]],
                f"Unexpected MCP invocation or cancellation: {records}")
        require(len({entry[3] for entry in records[:3]}) == 3, "The replacement process received a replayed tool request")
    finally:
        close(*list(open_fds))
        for process in processes:
            if process.poll() is None:
                process.kill()
        for process in processes:
            process.wait()


if __name__ == "__main__":
    if sys.argv[1] == "--peer":
        sys.exit(serve(int(sys.argv[2]), Path(sys.argv[3])))
    with tempfile.TemporaryDirectory(prefix="agentkit-mcp-stdio-") as temporary:
        verify(sys.argv[1], Path(temporary) / "effects.jsonl")
