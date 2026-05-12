#!/usr/bin/env python3
import argparse
import json
import os
import pathlib
import queue
import subprocess
import sys
import threading
import time
from urllib.parse import unquote, urlparse


def send_msg(proc, msg):
    data = json.dumps(msg, separators=(",", ":")).encode("utf-8")
    header = f"Content-Length: {len(data)}\r\n\r\n".encode("ascii")
    proc.stdin.write(header + data)
    proc.stdin.flush()


def read_exact(stream, n):
    chunks = []
    remaining = n
    while remaining:
        chunk = stream.read(remaining)
        if not chunk:
            raise EOFError(f"EOF while reading LSP body, {remaining} bytes missing")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def read_msg(stream):
    headers = {}

    while True:
        line = stream.readline()
        if not line:
            return None

        if line in (b"\r\n", b"\n"):
            break

        # Ignore accidental non-header noise defensively.
        if b":" not in line:
            continue

        key, value = line.decode("ascii", errors="replace").split(":", 1)
        headers[key.lower()] = value.strip()

    if "content-length" not in headers:
        return None

    length = int(headers["content-length"])
    body = read_exact(stream, length)
    return json.loads(body.decode("utf-8"))


def reader(proc, q):
    while True:
        msg = read_msg(proc.stdout)
        if msg is None:
            q.put(None)
            return
        q.put(msg)


def file_uri(path):
    return pathlib.Path(path).resolve().as_uri()


def path_from_uri(uri):
    parsed = urlparse(uri)
    return unquote(parsed.path)


class ClangdClient:
    def __init__(self, args):
        self.q = queue.Queue()
        self.next_id = 1
        self.versions = {}

        root = pathlib.Path(args.root).resolve()
        cmd = [
            args.clangd,
            "--background-index=false",
            "--clang-tidy",
            "--pch-storage=memory",
            "--enable-config",
            "--experimental-modules-support",
            f"--query-driver={args.query_driver}",
        ]

        if args.build_dir:
            cmd.append(f"--compile-commands-dir={pathlib.Path(args.build_dir).resolve()}")

        cmd.extend(args.extra_clangd_arg)

        self.proc = subprocess.Popen(
            cmd,
            cwd=str(root),
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=sys.stderr,
            bufsize=0,
        )

        self.reader_thread = threading.Thread(target=reader, args=(self.proc, self.q), daemon=True)
        self.reader_thread.start()

        self.root_uri = root.as_uri()
        self.initialize()

    def request(self, method, params):
        rid = self.next_id
        self.next_id += 1
        send_msg(self.proc, {"jsonrpc": "2.0", "id": rid, "method": method, "params": params})
        return rid

    def notify(self, method, params):
        send_msg(self.proc, {"jsonrpc": "2.0", "method": method, "params": params})

    def wait_response(self, rid, timeout=30.0):
        deadline = time.monotonic() + timeout
        stash = []
        while time.monotonic() < deadline:
            try:
                msg = self.q.get(timeout=deadline - time.monotonic())
            except queue.Empty:
                break
            if msg is None:
                raise RuntimeError("clangd exited")
            if msg.get("id") == rid:
                for m in stash:
                    self.q.put(m)
                return msg
            stash.append(msg)
        raise TimeoutError(f"timed out waiting for response id {rid}")

    def initialize(self):
        rid = self.request("initialize", {
            "processId": os.getpid(),
            "rootUri": self.root_uri,
            "workspaceFolders": [{"uri": self.root_uri, "name": "workspace"}],
            "capabilities": {
                "workspace": {
                    "configuration": True,
                    "workspaceFolders": True,
                },
                "textDocument": {
                    "publishDiagnostics": {
                        "relatedInformation": True,
                        "versionSupport": True,
                    },
                    "synchronization": {
                        "didSave": True,
                    },
                },
            },
            "clientInfo": {"name": "clangd-diags.py", "version": "1"},
        })
        self.wait_response(rid)
        self.notify("initialized", {})

    def open_or_change(self, path):
        path = pathlib.Path(path).resolve()
        uri = path.as_uri()
        text = path.read_text(encoding="utf-8", errors="replace")
        version = self.versions.get(uri, 0) + 1
        self.versions[uri] = version

        if version == 1:
            self.notify("textDocument/didOpen", {
                "textDocument": {
                    "uri": uri,
                    "languageId": "cpp",
                    "version": version,
                    "text": text,
                }
            })
        else:
            self.notify("textDocument/didChange", {
                "textDocument": {"uri": uri, "version": version},
                "contentChanges": [{"text": text}],
            })

        return uri

    def diagnostics_for(self, path, timeout=120.0, settle=0.75):
        uri = self.open_or_change(path)
        deadline = time.monotonic() + timeout
        last_diags = None
        last_update = None
        stash = []

        while time.monotonic() < deadline:
            if last_diags is not None and time.monotonic() - last_update >= settle:
                for m in stash:
                    self.q.put(m)
                return last_diags

            wait = 0.1
            if last_diags is not None:
                wait = min(wait, max(0.0, settle - (time.monotonic() - last_update)))

            try:
                msg = self.q.get(timeout=min(wait, max(0.0, deadline - time.monotonic())))
            except queue.Empty:
                continue

            if msg is None:
                raise RuntimeError("clangd exited")

            if (
                msg.get("method") == "textDocument/publishDiagnostics"
                and msg.get("params", {}).get("uri") == uri
            ):
                last_diags = msg["params"].get("diagnostics", [])
                last_update = time.monotonic()
            else:
                stash.append(msg)

        raise TimeoutError(f"no diagnostics received for {path}")

    def shutdown(self):
        try:
            rid = self.request("shutdown", None)
            self.wait_response(rid, timeout=5.0)
            self.notify("exit", None)
        except Exception:
            self.proc.terminate()


def print_diags(path, diags):
    sev = {1: "error", 2: "warning", 3: "info", 4: "hint"}
    path = str(pathlib.Path(path).resolve())

    if not diags:
        print(f"{path}: no diagnostics")
        return

    for d in diags:
        start = d["range"]["start"]
        line = start["line"] + 1
        col = start["character"] + 1
        level = sev.get(d.get("severity"), "diagnostic")
        msg = " ".join(d.get("message", "").split())
        source = d.get("source", "clangd")
        code = d.get("code")
        suffix = f" [{source}"
        if code is not None:
            suffix += f":{code}"
        suffix += "]"
        print(f"{path}:{line}:{col}: {level}: {msg}{suffix}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="*")
    ap.add_argument("-B", "--build-dir")
    ap.add_argument("--root", default=os.getcwd())
    ap.add_argument("--clangd", default="clangd")
    ap.add_argument("--query-driver", default="/usr/bin/clang++*")
    ap.add_argument("--timeout", type=float, default=120.0)
    ap.add_argument("--settle", type=float, default=0.75)
    ap.add_argument("--extra-clangd-arg", action="append", default=[])
    args = ap.parse_args()

    client = ClangdClient(args)

    try:
        if args.files:
            for f in args.files:
                diags = client.diagnostics_for(f, timeout=args.timeout, settle=args.settle)
                print_diags(f, diags)
        else:
            print("clangd diagnostic REPL. Type a source path, Ctrl-D to exit.", file=sys.stderr)
            for line in sys.stdin:
                f = line.strip()
                if not f:
                    continue
                try:
                    diags = client.diagnostics_for(f, timeout=args.timeout, settle=args.settle)
                    print_diags(f, diags)
                    print()
                    sys.stdout.flush()
                except Exception as e:
                    print(f"{f}: failed: {e}", file=sys.stderr)
    finally:
        client.shutdown()


if __name__ == "__main__":
    main()
