#!/usr/bin/env python3
"""Run the production lightweight C UI on a browser canvas or JSON-RPC socket."""

import argparse
import ctypes
import io
import json
import os
import shutil
import signal
import socketserver
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from PIL import Image

PAGE = """<!doctype html><html lang="zh"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Vibe Mode · Simulator</title>
<style>html{background:#151515;color:#eee;font:15px system-ui}body{margin:32px auto;max-width:680px;text-align:center}h1{font-size:20px;font-weight:500}canvas{width:min(480px,90vw);height:min(480px,90vw);background:#000;border-radius:22px;box-shadow:0 16px 60px #0008;touch-action:none}button{margin:20px 5px;padding:9px 18px;border:1px solid #444;border-radius:9px;background:#252525;color:#eee}p{color:#aaa;font-size:13px}</style>
<h1>Vibe Mode</h1><canvas width="480" height="480"></canvas><br><button data-command="ai">AI</button><button data-command="boot">Boot</button><p>点击或滑动屏幕 · 长按删除 / 恢复出厂设置</p><script>
const c=document.querySelector('canvas'),ctx=c.getContext('2d');let pressed=false,queue=Promise.resolve();
function send(x,y,p){queue=queue.then(()=>fetch('/pointer',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({x,y,pressed:p})}));}
function sample(e,p){let r=c.getBoundingClientRect();send(Math.max(0,Math.min(479,Math.round((e.clientX-r.left)*480/r.width))),Math.max(0,Math.min(479,Math.round((e.clientY-r.top)*480/r.height))),p);}
c.onpointerdown=e=>{pressed=true;c.setPointerCapture(e.pointerId);sample(e,true)};c.onpointermove=e=>{if(pressed)sample(e,true)};c.onpointerup=e=>{sample(e,false);pressed=false};c.onpointercancel=e=>{sample(e,false);pressed=false};
async function frame(){try{let r=await fetch('/frame.png',{cache:'no-store'});let b=await createImageBitmap(await r.blob());ctx.drawImage(b,0,0);b.close()}catch(e){}setTimeout(frame,50)}frame();
document.querySelectorAll('button').forEach(b=>b.onclick=()=>fetch('/control',{method:'POST',body:b.dataset.command}));</script></html>"""


class NativeUI:
    def __init__(self, library):
        self.dll_directory = None
        if os.name == "nt" and shutil.which("gcc"):
            # Python 3.8+ does not use PATH to resolve dependencies of a DLL.
            self.dll_directory = os.add_dll_directory(
                str(Path(shutil.which("gcc")).parent)
            )
        self.lib = ctypes.CDLL(str(library))
        pointer = ctypes.c_void_p
        self.lib.vui_create.argtypes = [ctypes.POINTER(pointer)]
        self.lib.vui_destroy.argtypes = [pointer]
        self.lib.vibe_pc_init.argtypes = [pointer]
        self.lib.vibe_pc_deinit.argtypes = [pointer]
        self.lib.vui_tick.argtypes = [pointer, ctypes.c_uint32]
        self.lib.vui_render.argtypes = [pointer, ctypes.POINTER(ctypes.c_uint16)]
        self.lib.vui_render.restype = ctypes.c_bool
        self.lib.vui_pointer.argtypes = [
            pointer,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_bool,
        ]
        self.ui = pointer()
        if self.lib.vui_create(ctypes.byref(self.ui)):
            raise RuntimeError("Unable to load UI assets")
        if self.lib.vibe_pc_init(self.ui):
            self.lib.vui_destroy(self.ui)
            raise RuntimeError("Unable to start controller")
        self.pixels = (ctypes.c_uint16 * (480 * 480))()
        self.lock = threading.RLock()
        self.image = None
        self.stopped = threading.Event()
        self.worker = threading.Thread(target=self.run, daemon=True)
        self.worker.start()

    def run(self):
        last = time.monotonic()
        while not self.stopped.wait(0.01):
            now = time.monotonic()
            elapsed = int((now - last) * 1000)
            last += elapsed / 1000
            with self.lock:
                self.lib.vui_tick(self.ui, elapsed)
                if self.lib.vui_render(self.ui, self.pixels):
                    self.image = None

    def pointer(self, x, y, pressed):
        with self.lock:
            self.lib.vui_pointer(self.ui, int(x), int(y), bool(pressed))
            if self.lib.vui_render(self.ui, self.pixels):
                self.image = None

    def frame(self):
        with self.lock:
            if self.image is None:
                # Pillow's BGR;16 decoder expects RGB565 little endian.
                self.image = Image.frombytes(
                    "RGB", (480, 480), bytes(self.pixels), "raw", "BGR;16"
                )
            return self.image.copy()

    def close(self):
        self.stopped.set()
        self.worker.join()
        self.lib.vibe_pc_deinit(self.ui)
        self.lib.vui_destroy(self.ui)
        if self.dll_directory:
            self.dll_directory.close()


class RPC(socketserver.StreamRequestHandler):
    def handle(self):
        while True:
            headers = {}
            while True:
                line = self.rfile.readline()
                if not line:
                    return
                if line in (b"\n", b"\r\n"):
                    break
                k, v = line.decode().split(":", 1)
                headers[k.lower()] = v.strip()
            size = int(headers.get("content-length", "0"))
            if size <= 0 or size > 65536:
                return
            req = json.loads(self.rfile.read(size))
            response = {"jsonrpc": "2.0", "id": req.get("id")}
            try:
                params = req.get("params", {})
                method = req["method"]
                if method == "feed_pointer":
                    self.server.ui.pointer(**params)
                elif method == "feed_pointers":
                    # Replay samples at one UI clock instant. Rapid-input
                    # tests must not depend on host rendering/RPC latency.
                    with self.server.ui.lock:
                        for sample in params["samples"]:
                            self.server.ui.pointer(**sample)
                elif method == "wait":
                    time.sleep(min(10, params.get("frames", 1) * 0.01))
                elif method == "screenshot":
                    self.server.ui.frame().save(
                        params["path"],
                        format="PPM" if params.get("format") == "ppm" else "PNG",
                    )
                else:
                    raise ValueError("Unknown method: " + method)
                response["result"] = {}
            except (ValueError, OSError, TypeError) as e:
                response["error"] = {"code": -32602, "message": str(e)}
            body = json.dumps(response).encode()
            self.wfile.write((f"Content-Length: {len(body)}\r\n\r\n").encode() + body)
            self.wfile.flush()


class HTTP(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def respond(self, data, content):
        self.send_response(200)
        self.send_header("Content-Type", content)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if self.path.split("?")[0] == "/frame.png":
            b = io.BytesIO()
            self.server.ui.frame().save(b, format="PNG")
            self.respond(b.getvalue(), "image/png")
        elif self.path == "/":
            self.respond(PAGE.encode(), "text/html; charset=utf-8")
        else:
            self.send_error(404)

    def do_POST(self):
        size = int(self.headers.get("Content-Length", "0"))
        if size > 4096:
            self.send_error(413)
            return
        body = self.rfile.read(size)
        if self.path == "/pointer":
            try:
                self.server.ui.pointer(**json.loads(body))
            except (ValueError, TypeError):
                self.send_error(400)
                return
        elif self.path == "/control":
            if body not in (b"ai", b"boot"):
                self.send_error(400)
                return
            path = Path(os.environ["VIBE_SIM_CONTROL_FILE"])
            tmp = path.with_suffix(".tmp")
            tmp.write_bytes(body)
            tmp.replace(path)
        else:
            self.send_error(404)
            return
        self.respond(b"{}", "application/json")


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument(
        "--scenario",
        default="empty",
        help="Fixture for tests; normal preview starts with no Ideas",
    )
    parser.add_argument(
        "--port", type=int, default=0, help="Optional localhost browser port"
    )
    args = parser.parse_args()
    args.directory.mkdir(parents=True, exist_ok=True)
    os.environ.update(
        VIBE_SIM_SCENARIO=args.scenario,
        VIBE_SIM_STATE_FILE=str(args.directory / "state.json"),
        VIBE_SIM_CONTROL_FILE=str(args.directory / "control"),
    )
    ui = NativeUI(args.library.resolve())
    rpc = Server(("127.0.0.1", 0), RPC)
    rpc.ui = ui
    threading.Thread(target=rpc.serve_forever, daemon=True).start()
    http = None
    if args.port:
        http = ThreadingHTTPServer(("127.0.0.1", args.port), HTTP)
        http.daemon_threads = True
        http.ui = ui
        threading.Thread(target=http.serve_forever, daemon=True).start()
    ready = {
        "api": f"tcp://127.0.0.1:{rpc.server_address[1]}",
        "pid": os.getpid(),
        "http": f"http://localhost:{args.port}/" if args.port else None,
    }
    (args.directory / "ready.tmp").write_text(json.dumps(ready))
    (args.directory / "ready.tmp").replace(args.directory / "ready.json")
    stopped = threading.Event()

    def stop(*_):
        stopped.set()

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    try:
        stopped.wait()
    finally:
        rpc.shutdown()
        rpc.server_close()
        if http:
            http.shutdown()
            http.server_close()
        ui.close()


if __name__ == "__main__":
    main()
