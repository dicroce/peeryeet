# Signaling and Transfer Protocol (prototype)

Two protocols:

1. **Signaling** — browser ↔ signaling server, over a WebSocket. Pairs two
   browsers and relays their WebRTC offer/answer/ICE candidates.
2. **Transfer** — browser ↔ browser, over the WebRTC DataChannel. Moves the
   file or text. The server never sees any of it.

## 1. Signaling

Transport: WebSocket at `wss://<host>/ws`. Caddy terminates TLS and proxies
to the server on `localhost:9000` as plain `ws://`.

Every message is one unfragmented WebSocket frame (text). The first byte is
the message type; the rest is the payload. Maximum message size is 64 KiB.

### Client → server

| Msg | Payload | Meaning |
|-----|---------|---------|
| `C` | —       | CREATE a session. Only allowed if this connection has no session. |
| `J` | code    | JOIN the session with this code. |
| `S` | opaque  | SIGNAL: forward this whole frame, byte for byte, to the paired peer. |

CLOSE is implicit: closing the WebSocket destroys the session.

### Server → client

| Msg | Payload | Meaning |
|-----|---------|---------|
| `C` | code    | Session created; show this code. |
| `J` | —       | Join succeeded (sent to the joiner). |
| `P` | —       | A peer joined your session (sent to the creator). |
| `S` | opaque  | Signal from the peer (the peer's frame, unmodified). |
| `X` | —       | The peer disconnected; the session is gone. |
| `E` | text    | Error, human-readable. The connection may or may not be closed. |

The server parses only the first byte of each message. `S` payloads are
treated as opaque; the clients put JSON in them (`{"sdp":...}` or
`{"c":<ICE candidate>}`).

### Session codes

Format `WORD-WORD-NNNN`, e.g. `HORSE-LAMP-0427`: two words from a fixed list
of 256 plus four digits, which gives 256 × 256 × 10,000 ≈ 6.6 × 10⁸ codes
(~29 bits). Clients normalize what the user types (uppercase, any run of
non-alphanumerics becomes `-`) before sending `J`; the server compares bytes.

Codes travel in the URL fragment (`https://peeryeet.com/#HORSE-LAMP-0427`) for
links and QR codes, so they never appear in HTTP request lines or access logs.

### Lifetimes

| State | Limit |
|-------|-------|
| TCP connected, no WebSocket handshake | 10 s |
| WebSocket open, no session | 60 s |
| Session created, waiting for a joiner | 10 min |
| Session paired | 5 min |

Clients close the WebSocket as soon as the DataChannel opens, so a normal
session lasts a few seconds after pairing. The transfer itself does not
depend on the server.

### Guessing protection

Failed joins (unknown code, or a session that already has a joiner) are
rate-limited per client IP: a bucket of 10 attempts refilling one every 6
seconds. When it is empty the server sends `E` and closes the connection.
Behind Caddy, the client IP is the last `X-Forwarded-For` entry, trusted only
when the TCP peer is loopback.

Each session accepts exactly one joiner. If an attacker guesses a code first,
the real receiver gets "code already in use", so a stolen session is
visible rather than silent.

Not done yet (see design doc §11): a stronger secret in the QR code than in
the typed code, and explicit sender confirmation before data flows.

## 2. Transfer (DataChannel)

One reliable, ordered DataChannel per session. String messages are JSON
control messages; binary messages are file data.

```
sender                                  receiver
  │ {"t":"meta","kind","name","size","mime"} │
  │ ───────────────────────────────────────▶ │  shows Accept / Decline
  │ ◀─────────────────────── {"t":"accept"}  │  (or {"t":"decline"})
  │ binary chunk 0..N (64 KiB each, last     │
  │ may be short)                            │
  │ ───────────────────────────────────────▶ │
  │ ◀──────────────── {"t":"ack","n":bytes}  │  after bytes are written
  │ {"t":"end","hash":hex}                   │
  │ ───────────────────────────────────────▶ │
  │ ◀──────────────── {"t":"done","ok":bool} │
```

- `kind` is `"file"` or `"text"`. Text is sent as UTF-8 bytes through the
  same chunk path.
- **Offsets** are implicit: the channel is reliable and ordered, so the
  receiver's byte count is the offset.
- **Backpressure** works at two levels. The sender pauses when the
  DataChannel's `bufferedAmount` exceeds 8 MiB. It also never runs more than
  32 MiB ahead of the receiver's last `ack`. The receiver acks only after
  bytes are written, so a slow disk throttles the sender instead of filling
  the receiver's memory.
- **Integrity** uses a chained hash over chunks: `h₀ = 32 zero bytes`,
  `hᵢ₊₁ = SHA-256(hᵢ ‖ SHA-256(chunkᵢ))`. Both sides compute it as data
  flows, using WebCrypto (native speed, constant memory). The sender's final
  value goes in `end`, and the receiver compares.
