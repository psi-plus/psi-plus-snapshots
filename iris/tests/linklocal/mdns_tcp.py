#!/usr/bin/env python3
"""Two real processes: loopback mDNS discovery -> SRV -> TCP XML -> goodbye.

No internet server, Docker, port reservation, artificial DNS cache or manual
calls into a stream's private state are involved. Requires local multicast UDP
5353/TCP sockets (run outside a network-restricted sandbox).
"""
import queue
import subprocess
import sys
import threading
import time
import uuid


class Process:
    def __init__(self, executable, *arguments):
        self.process = subprocess.Popen(
            [executable, "--allow-plain", "--address", "127.0.0.1", *arguments],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        )
        self.lines = []
        self.updates = queue.Queue()
        self.reader = threading.Thread(target=self.read, daemon=True)
        self.reader.start()

    def read(self):
        for line in self.process.stdout:
            self.lines.append(line.rstrip())
            self.updates.put(line)

    def wait_for(self, text, timeout=12):
        until = time.monotonic() + timeout
        while time.monotonic() < until:
            if any(text in line for line in self.lines):
                return
            try:
                self.updates.get(timeout=0.05)
            except queue.Empty:
                if self.process.poll() is not None:
                    break
        raise AssertionError(f"Missing {text!r}; exit={self.process.poll()}:\n" + "\n".join(self.lines))

    def finish(self, request_stop=True):
        if request_stop and self.process.poll() is None:
            self.process.terminate()  # ProcessQuit must flush the PTR goodbye.
        try:
            code = self.process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
            raise AssertionError("Example failed to shut down")
        self.reader.join(timeout=2)
        assert code == 0, f"exit={code}:\n" + "\n".join(self.lines)


def scenario(executable, empty_txt):
    suffix = uuid.uuid4().hex[:10]
    recipient = f"juliet@pronto-{suffix}"
    sender = f"romeo@forza-{suffix}"
    body = "XEP-0174: привет <&> test"
    server = Process(executable, "--jid", recipient, "--echo", "--expect", body,
                     *(('--empty-txt',) if empty_txt else ()))
    client = None
    try:
        server.wait_for("ANNOUNCED " + recipient)
        client = Process(executable, "--jid", sender, "--send", recipient, "--message", body,
                         "--expect", body, "--exit-after-message", "--run-for", "12000")
        client.wait_for("RECEIVED " + recipient)
        client.wait_for("OFFLINE " + sender)
        client.finish(request_stop=False)  # --exit-after-message is already shutting down
        server.wait_for("RECEIVED " + sender)
        server.wait_for("UNAVAILABLE " + sender)
        assert any("CONNECTING " + recipient in line for line in client.lines)
        print(f"PASS mDNS/SRV/TCP/reply/goodbye (empty TXT={empty_txt})")
    finally:
        if client and client.process.poll() is None:
            client.finish()
        server.finish()


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: mdns_tcp.py /path/to/iris-linklocal")
    scenario(sys.argv[1], False)
    scenario(sys.argv[1], True)
