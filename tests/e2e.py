#!/usr/bin/env python3
"""End-to-end test: two headless Chromium sessions transfer text and a file
through the real signaling server and a real WebRTC DataChannel.

Setup (once):
    python3 -m venv .venv && .venv/bin/pip install playwright
    .venv/bin/python -m playwright install chromium
    sudo .venv/bin/python -m playwright install-deps chromium   # system libraries

Run against local servers (builds the signaling server first):
    make -C server && .venv/bin/python tests/e2e.py

Run against a deployment instead:
    .venv/bin/python tests/e2e.py --base https://peeryeet.com/
"""
import functools
import hashlib
import http.server
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time

from playwright.sync_api import sync_playwright

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FILE_SIZE = 50 * 1000 * 1000 + 12345  # deliberately not a multiple of the chunk size
TEXT = "hello from peeryeet ✓ 🚀"

# Replaces the native save picker with one that records what was written, to
# exercise the stream-to-disk path headlessly.
MOCK_PICKER = """
window.__written = { bytes: 0, closed: false, parts: [] };
window.showSaveFilePicker = async () => ({
  createWritable: async () => ({
    write: async (buf) => { window.__written.bytes += buf.byteLength; window.__written.parts.push(new Uint8Array(buf)); },
    close: async () => { window.__written.closed = true; },
    abort: async () => {},
  }),
});
"""

VISIBLE_STATE = """() => [...document.querySelectorAll('section')].filter(x => !x.hidden)
  .map(x => x.id + ': ' + x.innerText.replace(/\\s+/g, ' ')).join(' | ')"""


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def serve_web(port, sig_port):
    """Serves web/ with the production CSP (plus the local signaling port)."""
    csp = (f"default-src 'self'; connect-src 'self' ws://localhost:{sig_port}; img-src 'self' data: blob:; "
           "object-src 'none'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'")

    class Handler(http.server.SimpleHTTPRequestHandler):
        def end_headers(self):
            self.send_header("Content-Security-Policy", csp)
            super().end_headers()

        def log_message(self, *args):
            pass

    httpd = http.server.ThreadingHTTPServer(
        ("127.0.0.1", port), functools.partial(Handler, directory=os.path.join(ROOT, "web")))
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    return httpd


def wait_until(page, js, timeout=120):
    # Polls from Python: page.wait_for_function evaluates strings, which the CSP forbids.
    end = time.time() + timeout
    while time.time() < end:
        if page.evaluate(js):
            return
        time.sleep(0.1)
    raise TimeoutError(js)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def main():
    server = httpd = None
    if len(sys.argv) == 3 and sys.argv[1] == "--base":
        base = sys.argv[2]
    else:
        sig_port, web_port = free_port(), free_port()
        base = f"http://localhost:{web_port}/?ws=ws://localhost:{sig_port}/ws"
        server = subprocess.Popen([os.path.join(ROOT, "server", "peeryeet-signal"), "-p", str(sig_port)],
                                  stderr=subprocess.DEVNULL)
        httpd = serve_web(web_port, sig_port)
    tmp = tempfile.TemporaryDirectory()
    big = os.path.join(tmp.name, "big.bin")
    with open(big, "wb") as f:
        f.write(os.urandom(FILE_SIZE))
    want = sha256_file(big)

    pages = []
    try:
        with sync_playwright() as p:
            browser = p.chromium.launch()

            def pair(send_action, picker=None):
                """A sender and a receiver in separate browser contexts; returns the code."""
                s_ctx, r_ctx = browser.new_context(), browser.new_context(accept_downloads=True)
                if picker == "none":
                    r_ctx.add_init_script("delete window.showSaveFilePicker;")
                elif picker == "mock":
                    r_ctx.add_init_script(MOCK_PICKER)
                s, r = s_ctx.new_page(), r_ctx.new_page()
                pages[:] = [("send", s), ("recv", r)]
                for tag, pg in pages:
                    pg.on("pageerror", lambda e, tag=tag: print(f"  [{tag} pageerror] {e}"))
                    pg.on("console", lambda m, tag=tag: m.type == "error" and print(f"  [{tag} console] {m.text}"))
                s.goto(base)
                send_action(s)
                s.wait_for_selector("#s-code:not(:empty)")
                assert not s.is_visible("#ad"), "no ad while waiting for the receiver"
                return s, r, s.text_content("#s-code")

            def sender_verified(s):
                wait_until(s, "() => document.querySelector('#s-status').textContent.includes('verified')")

            try:
                # Text; the receiver types the code sloppily.
                s, r, code = pair(lambda s: (s.click("#show-text"), s.fill("#text", TEXT), s.click("#send-text")))
                r.goto(base)
                r.fill("#code", code.lower().replace("-", " "))
                r.click("#join button")
                r.wait_for_selector("#accept", state="visible", timeout=40000)
                r.click("#accept")
                r.wait_for_selector("#r-text", state="visible", timeout=40000)
                assert r.input_value("#r-textbox") == TEXT
                sender_verified(s)
                assert r.is_visible("#ad") and s.is_visible("#ad"), "ad should show once done"
                assert not r.is_visible("#accept")
                print("ok   text")

                # File, in-memory fallback; the receiver opens the link/QR URL.
                s, r, code = pair(lambda s: s.set_input_files("#file", big), picker="none")
                r.goto(f"{base}#{code}")
                r.wait_for_selector("#accept", state="visible", timeout=40000)
                t0 = time.time()
                with r.expect_download(timeout=120000) as dl:
                    r.click("#accept")
                out = os.path.join(tmp.name, "got.bin")
                dl.value.save_as(out)
                dt = time.time() - t0
                assert sha256_file(out) == want, "file hash mismatch"
                sender_verified(s)
                print(f"ok   file, in-memory path ({FILE_SIZE / dt / 1e6:.0f} MB/s)")

                # File, stream-to-disk path.
                s, r, code = pair(lambda s: s.set_input_files("#file", big), picker="mock")
                r.goto(f"{base}#{code}")
                r.wait_for_selector("#accept", state="visible", timeout=40000)
                t0 = time.time()
                r.click("#accept")
                wait_until(r, "() => window.__written.closed")
                dt = time.time() - t0
                got = r.evaluate("""async () => {
                    const d = await crypto.subtle.digest('SHA-256', await new Blob(window.__written.parts).arrayBuffer());
                    return Array.from(new Uint8Array(d), x => x.toString(16).padStart(2, '0')).join('');
                }""")
                assert got == want, "streamed hash mismatch"
                sender_verified(s)
                print(f"ok   file, stream-to-disk path ({FILE_SIZE / dt / 1e6:.0f} MB/s)")
                print(f"       sender:   {s.text_content('#s-status')} {s.text_content('#s-path')}")
                print(f"       receiver: {r.text_content('#r-status')} {r.text_content('#r-path')}")

                # Unknown code.
                r = browser.new_page()
                pages[:] = [("recv", r)]
                r.goto(f"{base}#NOPE-NOPE-0000")
                r.wait_for_selector("#error", state="visible")
                assert "No transfer with that code" in r.text_content("#e-body")
                print("ok   unknown code")
            except Exception:
                for tag, pg in pages:
                    try:
                        print(f"  state of {tag}: {pg.evaluate(VISIBLE_STATE)}")
                    except Exception as e:
                        print(f"  state of {tag}: unavailable ({e})")
                raise
            finally:
                browser.close()
        print("all e2e tests passed")
    finally:
        if server:
            server.terminate()
            server.wait()
            httpd.shutdown()
        tmp.cleanup()


if __name__ == "__main__":
    sys.exit(main())
