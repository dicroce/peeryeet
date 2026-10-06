# PeerYeet

**Send a file or text from one computer to another, directly.** No upload, no
account, no copies left on a server.

**Try it: https://peeryeet.com**

1. On the sending computer, drop a file on the page (or paste some text).
2. You get a short code like `HORSE-LAMP-0427` and a QR code.
3. On the receiving computer or phone, open the site and type the code or scan
   the QR code.
4. The receiver clicks **Accept**, and the bytes go straight from one browser
   to the other.

## Direct, or not at all

Most "send a file" sites upload your file to their server, and the other side
downloads it from there. PeerYeet doesn't. The server only introduces the two
browsers. After that, they talk to each other over a WebRTC data channel,
which is encrypted end to end.

```
setup (a few small messages)        the transfer itself

 Browser A ─┐       ┌─ Browser B     Browser A ══════════════ Browser B
            ▼       ▼                         direct WebRTC
         signaling server
```

There is deliberately no relay (TURN) server. If the two browsers can't reach
each other directly, the transfer fails and the page explains why, rather than
quietly routing your data through someone else's machine. That mostly happens
on strict office or school networks, some VPNs, and some mobile carriers.

Because the server never carries file data, a 500 GB transfer costs it the
same as a 5 KB one. That's what lets the site stay free.

## Privacy and security

- **Your data never touches the server.** It only relays the short setup
  messages that let the browsers find each other.
- **No logs of who you are.** The server keeps a few anonymous totals: page
  loads, codes created, receivers connected, and whether each transfer
  connected directly and was delivered. It doesn't store IP addresses (only an
  in-memory hash, for rate limiting), and the web server writes no access logs.
  The code goes in the URL fragment (`#HORSE-LAMP-0427`), which browsers never
  send to the server.
- **Codes are hard to guess.** Codes have about 29 bits of randomness, failed
  join attempts are rate-limited per client, and each code accepts exactly one
  receiver. If someone else does join first, the real receiver sees "code
  already in use" instead of silently missing the transfer.
- **Delivery is verified.** Both sides compute a SHA-256 hash chain over the
  data as it moves, and the receiver checks it against the sender's at the end.
- **Nothing happens without consent.** The receiver sees the name and size and
  must click Accept. The page reads your clipboard only when you paste.

One limit to be aware of: the signaling server passes along the messages that
set up the encrypted connection, so you are trusting it not to tamper with
them. A server that did tamper could put itself in the middle of the
connection. If that's a concern, [host your own](#hosting-your-own). Stronger
pairing is an open item in [the design doc](docs/concept-and-architecture.md)
(§11).

You don't have to take any of this on faith. The logic is under 1,700 lines
in two files, [web/app.js](web/app.js) and [server/signal.c](server/signal.c).

## Large files

PeerYeet reads and writes files in chunks, so big transfers work. In Chrome,
Edge and other Chromium browsers, the receiver picks where to save first and
data streams straight to disk. Other browsers keep the file in memory until
you save it, and the page warns you about that for files over 1 GB.

## How it's built

There are no frameworks and no build step.

| Path | What it is |
|------|------------|
| [web/](web/) | The site: plain HTML, CSS and JavaScript. Served as is. |
| [server/](server/) | The signaling server: one C file, a single-threaded `epoll` loop with just enough WebSocket to pair two browsers. Linux only. |
| [deploy/](deploy/) | Caddy and systemd setup for the production host. |
| [docs/concept-and-architecture.md](docs/concept-and-architecture.md) | The design doc: what PeerYeet is, why, and what's next. |
| [docs/signaling-protocol.md](docs/signaling-protocol.md) | The wire protocols, both browser-to-server and browser-to-browser. |

## Running it locally

You need a C compiler, `make` and Python 3.

    make -C server
    ./server/peeryeet-signal &                 # signaling on localhost:9000
    python3 -m http.server 8000 -d web         # site on localhost:8000

Open http://localhost:8000 in two windows; make the second one private so the
two don't share state. When the page is served from a non-default port, it
looks for signaling on port 9000 of the same host. Add
`?ws=ws://host:port/ws` to the URL to point it somewhere else.

Browsers allow Web Crypto (and the save-to-disk picker) only on secure
origins, so testing between two machines over plain `http://<lan-ip>` won't
work. Use an HTTPS deployment for that.

### Tests

    python3 server/test_signal.py              # signaling server tests
    .venv/bin/python tests/e2e.py              # two headless browsers, real WebRTC

The end-to-end test needs Playwright; setup steps are at the top of
[tests/e2e.py](tests/e2e.py).

## Hosting your own

[deploy/](deploy/) contains the full setup for a single small Linux host:
Caddy serves the site, gets HTTPS certificates automatically and forwards
`/ws` to the signaling server, which runs as a locked-down systemd service. A
`t3.micro` is plenty. Follow [deploy/README.md](deploy/README.md).

To use your own domain, change `peeryeet.com` in
[deploy/Caddyfile](deploy/Caddyfile) and in the "go to" hint in
[web/index.html](web/index.html). The small ad at the bottom of the page is
for the author's math app; feel free to remove it.

## Contributing

Issues and pull requests are welcome. Please keep to the spirit of the
project: direct transfers only, no build step, and as few dependencies as
possible (right now there's one, a vendored QR code library).

To report a security problem, please don't open a public issue. See
[SECURITY.md](SECURITY.md).

## License

MIT; see [LICENSE](LICENSE). [web/qrcode.js](web/qrcode.js) is by Kazuhiko
Arase, also under MIT.

---

Also by the author: [Wyrm](https://dicroce.github.io/wyrm/home.html), an app
for learning algebra by touch. Drag terms, cancel factors, and you can't break
the math. iOS and Android.
