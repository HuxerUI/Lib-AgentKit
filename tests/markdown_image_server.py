import http.server
import os
import pathlib
import struct
import subprocess
import sys
import tempfile
import threading
import zlib


def png_chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


WIDTH, HEIGHT = 32, 20
HEADER = png_chunk(b"IHDR", struct.pack(">2I5B", WIDTH, HEIGHT, 8, 2, 0, 0, 0))
PIXELS = (b"\0" + b"\x10\xb0\x70" * WIDTH) * HEIGHT
PNG_PREFIX = b"\x89PNG\r\n\x1a\n" + HEADER
END = png_chunk(b"IEND", b"")
PNG = PNG_PREFIX + png_chunk(b"IDAT", zlib.compress(PIXELS)) + END
DAMAGED = PNG_PREFIX + png_chunk(b"IDAT", b"invalid pixel data") + END


def main(executable):
    requested = []
    lock = threading.Lock()

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_args):
            pass

        def do_GET(self):
            with lock:
                requested.append(self.path)
            status = 200
            body = b""
            if self.path == "/diagram.png":
                body = PNG
            elif self.path == "/damaged.png":
                body = DAMAGED
            else:
                status = 404
            self.send_response(status)
            self.send_header("Content-Type", "image/png" if body else "text/plain")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            try:
                self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError):
                pass

    if sys.platform in ("win32", "darwin") or os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY"):
        with http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler) as server:
            worker = threading.Thread(target=server.serve_forever)
            worker.start()
            try:
                result = subprocess.run(
                    [executable, "--markdown-image-transport", f"http://127.0.0.1:{server.server_port}"], timeout=60)
            finally:
                server.shutdown()
                worker.join()
        if result.returncode:
            return result.returncode
        with lock:
            required = ["/diagram.png", "/missing.png", "/damaged.png"]
            if requested != required:
                raise RuntimeError(f"Unexpected Markdown image requests: {requested}")
    with tempfile.TemporaryDirectory() as directory:
        fixture = pathlib.Path(directory) / "diagram.png"
        fixture.write_bytes(PNG)
        return subprocess.run([executable, "--markdown-images", str(fixture)], timeout=180).returncode


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
