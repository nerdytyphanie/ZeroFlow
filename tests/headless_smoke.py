"""Hardware-free process tests. Never connects to a real sharing server."""
import json
import pathlib
import queue
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

source = pathlib.Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix="zeroflow-headless-test-") as temporary:
    root = pathlib.Path(temporary)
    exe = root / "ZeroFlow.exe"
    shutil.copy2(source, exe)  # No Qt/OpenSSL/CRT DLLs beside the executable.
    flags = subprocess.CREATE_NO_WINDOW
    def run(args):
        return subprocess.run([str(exe), *args], capture_output=True, text=True, timeout=10, creationflags=flags)
    assert run(["--help"]).returncode == 0
    for args in [[], ["--client"], ["--unknown"], ["--server", "--client", "--settings", str(root / "invalid.ini")],
                 ["--client", "--settings", "relative.ini"],
                 ["--client", "--settings", str(root / "invalid.ini"), "--port", "65536"]]:
        assert run(args).returncode == 3, args
    assert not (root / "invalid.ini").exists()

    # Occupy a loopback port without listening. No external peer can be contacted.
    with socket.socket() as port_guard:
        port_guard.bind(("127.0.0.1", 0))
        port = str(port_guard.getsockname()[1])
        args = ["--client", "--headless", "--status-json", "--control-stdin", "--settings", str(root / "config" / "client.ini"),
                "--host", "127.0.0.1", "--port", port]
        def start():
            p = subprocess.Popen([str(exe), *args], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True, creationflags=flags)
            messages, errors = queue.Queue(), []
            def read():
                for line in p.stdout: messages.put(json.loads(line))
            threading.Thread(target=read, daemon=True).start()
            def read_errors():
                for line in p.stderr: errors.append(line)
            threading.Thread(target=read_errors, daemon=True).start()
            return p, messages, errors
        for stop_with_eof in [False, True]:
            p, messages, errors = start()
            try:
                try: records = [messages.get(timeout=15) for _ in range(3)]
                except queue.Empty: raise AssertionError(f"exit={p.poll()} stderr={''.join(errors)}")
                assert p.poll() is None, ''.join(errors)
                assert all(r['schema'] == 1 and r['type'] == 'status' and r['role'] == 'client' for r in records)
                assert records[0]['sequence'] < records[1]['sequence'] < records[2]['sequence']
                assert records[2]['uptimeMs'] > records[1]['uptimeMs']
                assert records[2]['uptimeMs'] < 60000
                assert records[2]['state'] != 'starting', records
                assert run(args).returncode == 5, 'duplicate instance not rejected'
                cert = root / 'config' / 'tls' / 'identity.pem'
                assert cert.exists() and cert.stat().st_size > 1000
                before = cert.read_bytes()
                if stop_with_eof: p.stdin.close()
                else:
                    p.stdin.write('{"command":"stop"}\n'); p.stdin.flush()
                assert p.wait(timeout=8) == 0, ''.join(errors)
                assert cert.read_bytes() == before
                assert not any('INFO:' in line or 'DEBUG:' in line or 'initial settings file:' in line for line in errors), ''.join(errors)
            finally:
                if p.poll() is None: p.kill(); p.wait()
                for pipe in [p.stdin, p.stdout, p.stderr]:
                    if not pipe.closed: pipe.close()
    print('PASS: loose EXE, CLI validation, offline heartbeats, duplicate guard, TLS identity, JSON stop, stdin EOF')
