from base64 import b64decode
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
from threading import Event, Thread

try:
    import paramiko
except ImportError:
    print("Paramiko is required for the isolated SSH server fixture")
    sys.exit(77)


COMMAND = b"agentkit-verify-build"
STATUSES = (0, 17, None)


class Server(paramiko.ServerInterface):
    def __init__(self, client_key):
        self.client_key = client_key
        self.request = Event()
        self.command = None

    def get_allowed_auths(self, username):
        return "publickey"

    def check_auth_publickey(self, username, key):
        if username == "fixture" and key.asbytes() == self.client_key.asbytes():
            return paramiko.AUTH_SUCCESSFUL
        return paramiko.AUTH_FAILED

    def check_channel_request(self, kind, chanid):
        if kind == "session":
            return paramiko.OPEN_SUCCEEDED
        return paramiko.OPEN_FAILED_ADMINISTRATIVELY_PROHIBITED

    def check_channel_exec_request(self, channel, command):
        self.command = command
        self.request.set()
        return command == COMMAND


def verify(executable, ssh, root):
    private = root / "client-key"
    known = root / "known_hosts"
    config = root / "ssh_config"
    host_key = paramiko.RSAKey.generate(2048)
    client = Path(ssh)
    keygen = client.with_name("ssh-keygen" + client.suffix)
    subprocess.run([str(keygen), "-q", "-t", "rsa", "-b", "2048", "-N", "", "-f", str(private)],
                   check=True, capture_output=True, timeout=10)
    public = (root / "client-key.pub").read_text(encoding="utf-8").split()
    client_key = paramiko.RSAKey(data=b64decode(public[1]))
    stopped = Event()
    executed = []
    failures = []

    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen(4)
        listener.settimeout(0.2)
        port = listener.getsockname()[1]
        known.write_text(f"[127.0.0.1]:{port} {host_key.get_name()} {host_key.get_base64()}\n", encoding="utf-8")
        config.write_text(
            f"Host fixture-build\n"
            f"    HostName 127.0.0.1\n"
            f"    Port {port}\n"
            f"    User fixture\n"
            f'    UserKnownHostsFile "{known.as_posix()}"\n'
            f"    GlobalKnownHostsFile none\n"
            f'    IdentityFile "{private.as_posix()}"\n'
            f"    IdentitiesOnly yes\n",
            encoding="utf-8",
        )

        def serve():
            try:
                while not stopped.is_set():
                    try:
                        connection, address = listener.accept()
                    except TimeoutError:
                        continue
                    with connection:
                        if address[0] != "127.0.0.1" or len(executed) >= len(STATUSES):
                            raise RuntimeError("Unexpected SSH connection")
                        with paramiko.Transport(connection) as transport:
                            transport.add_server_key(host_key)
                            server = Server(client_key)
                            transport.start_server(server=server)
                            channel = transport.accept(8)
                            if channel is None or not server.request.wait(8) or server.command != COMMAND:
                                raise RuntimeError("The client did not invoke the configured SSH task")
                            status = STATUSES[len(executed)]
                            executed.append((server.command, status))
                            if status is not None:
                                channel.sendall(b"Remote build verification completed\n")
                                channel.send_exit_status(status)
                                channel.shutdown_write()
                                channel.close()
                                for _ in range(50):
                                    if not transport.is_active():
                                        break
                                    stopped.wait(0.02)
            except Exception as error:
                failures.append(error)

        worker = Thread(target=serve, daemon=True)
        worker.start()
        try:
            result = subprocess.run([executable, ssh, str(config), "fixture-build"],
                                    capture_output=True, text=True, timeout=85)
        finally:
            stopped.set()
            worker.join(timeout=10)
        if failures:
            raise failures[0]
        if result.returncode:
            raise RuntimeError(result.stderr or result.stdout or f"SSH host exited with {result.returncode}")
        if executed != [(COMMAND, status) for status in STATUSES]:
            raise RuntimeError(f"Expected three approved invocations only, received {executed}")
        print(result.stdout.strip())
        print("OpenSSH verified the fixture host key and executed only the approved build checks")


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="agentkit-ssh-") as cwd:
        verify(sys.argv[1], sys.argv[2], Path(cwd))
