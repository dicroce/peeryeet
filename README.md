# PeerYeet

Move a file or text from this computer to that one over a direct WebRTC
connection. The server only introduces the two browsers; the data never
touches it.

- [docs/concept-and-architecture.md](docs/concept-and-architecture.md): what and why (master design doc)
- [docs/signaling-protocol.md](docs/signaling-protocol.md): wire protocols
- `web/`: the static site (plain HTML/CSS/JS, no build step)
- `server/`: the signaling server (C, epoll, Linux)
- `deploy/`: Caddy + systemd setup for the production host

## Local development

    make -C server
    ./server/peeryeet-signal &                 # signaling on localhost:9000
    python3 -m http.server 8000 -d web         # site on localhost:8000

Open http://localhost:8000 in two windows (use a private window for the
second). When served from a non-default port, the page connects to signaling
on port 9000 of the same host; `?ws=ws://host:port/ws` overrides that.

Browsers only allow Web Crypto (and the save-to-disk picker) in secure
contexts, so testing between two machines over plain `http://<lan-ip>` won't
work. Use the deployed https site for that.

## Tests

    python3 server/test_signal.py              # signaling server smoke tests
    .venv/bin/python tests/e2e.py              # two headless browsers, real WebRTC

The e2e test needs Playwright; setup steps are at the top of
[tests/e2e.py](tests/e2e.py).
