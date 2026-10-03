# P2P File Sharing — Concept and Architecture

**Status:** Idea / prototype candidate
**Date:** October 2026

> This file is the master copy. It was imported from the Google Doc
> "P2P File Sharing — Concept and Architecture" on 2026-10-03; edit it here.

## 1. Product Idea

Build an extremely simple website for transferring files and clipboard content directly between two computers using a WebRTC DataChannel. The core job is broader than file sharing: move something from this computer to that computer.

The product should make moving files or pasted content between computers feel almost trivial:

1. Open the site on the sending computer.
2. Drop a file onto the page or paste text/clipboard content into the send area.
3. Receive a short human-readable session code and QR code.
4. Open the site on the receiving computer and enter the code, or scan the QR code.
5. The browsers negotiate a direct WebRTC connection.
6. The selected file or pasted content moves directly from sender to receiver.
7. When either side closes the page, the ephemeral session disappears.

The defining constraint is:

**P2P OR NOTHING.**

The service does not upload or store user payloads, proxy files or clipboard content, or provide TURN relay service. If the two browsers cannot establish a direct peer-to-peer WebRTC connection, the transfer fails cleanly and explains why.

## 2. Product Philosophy

This is deliberately not another cloud file-sharing or synchronization service.

There are:

- No accounts.
- No sign-in.
- No cloud file storage.
- No upload step.
- No transfer history.
- No persistent share links.
- No offline delivery.
- No server-side file-size limit imposed by storage.
- No file or clipboard contents passing through the service.
- No background clipboard synchronization.

Both computers must be online at the same time. The bytes go from one browser to the other.

The central user-facing promise should be architectural rather than contractual:

> “What you send never touches our servers.”

The UI should reinforce this by displaying “Direct connection established” once WebRTC negotiation succeeds.

## 3. User Experience

Sender:

```
Drop something here to send it.
—or—
Paste text here to send it.

vacation-photos.zip
2.7 GB

Send this file with:
HORSE-LAMP-27
[QR CODE]

Keep this window open until the transfer finishes.
```

Receiver:

```
Receive a file
[ HORSE-LAMP-27 ]
Connect
```

After connection:

```
Connected directly to sender
vacation-photos.zip
2.7 GB
██████████████░░░░░░ 71%
184 MB/s
```

The service should avoid terminology such as “upload” because nothing is being uploaded to the service.

Clipboard transfer should require an explicit user action. The product should not silently monitor or synchronize the system clipboard. A simple “Paste here to send” interaction keeps the security model understandable and avoids accidentally capturing sensitive clipboard contents such as passwords.

The product transfers things; it does not synchronize things.

## 4. High-Level Architecture

The system has two radically different data paths.

```
CONTROL PATH

Browser A ──────┐
                │ signaling
                ▼
          Signaling Server
                ▲
                │ signaling
Browser B ──────┘

FILE DATA PATH

Browser A ═════════════════════════════ Browser B
             WebRTC DataChannel
```

The server participates only in discovery and WebRTC signaling. Once the peer-to-peer connection is established, file bytes never traverse the server.

A 500 GB transfer should therefore cost the service approximately the same infrastructure resources as a tiny transfer after negotiation is complete.

## 5. Signaling Server

The server should be intentionally tiny.

Conceptually it needs operations equivalent to:

```
CREATE
  Allocate a random ephemeral session.

JOIN <session>
  Associate a second connection with that session.

SIGNAL <opaque payload>
  Forward WebRTC signaling information to the paired peer.

CLOSE
  Destroy the ephemeral session.
```

The server should understand as little as possible.

Once two peers have been authenticated and paired, signaling payloads should preferably be treated as opaque bytes rather than parsed application objects.

Sessions should have short lifetimes and disappear automatically when peers disconnect or a timeout expires.

## 6. Extreme Server Efficiency as a First-Class Goal

Infrastructure efficiency is part of the project's identity.

The challenge is not merely to make the server “fast enough.” The goal is to determine how many users can be supported by hardware that sounds absurdly inadequate.

Desired characteristics:

- One tiny Linux binary.
- Very small resident memory.
- Minimal allocations.
- Minimal copies.
- Minimal parsing.
- Minimal syscalls.
- Event-driven networking.
- No database in the critical path.
- Ephemeral in-memory session state.
- Linux-specific optimizations are welcome.
- Portability is secondary to simplicity and efficiency.

An explicit benchmark should be:

**CONCURRENT SIGNALING SESSIONS PER MB OF RAM**

Other useful measurements:

- Maximum simultaneous connected clients.
- Memory per connection.
- Memory per paired session.
- CPU cost per signaling message.
- Signaling messages per second.
- Connection establishment rate.
- File bytes transferred peer-to-peer versus bytes handled by the server.

A fun eventual public metric would be:

```
This service is running on:
1 vCPU
256 MB RAM

Transfers initiated today: 12,481
Data transferred directly between users: 8.7 TB
File data transferred through this server: 0 bytes
```

Those numbers are illustrative; real published numbers must come from actual measurement.

## 7. splice() Experiment

A specific engineering goal is to investigate whether Linux `splice()` can be used in the production signaling hot path.

After two clients have been paired, the idealized server becomes a bidirectional byte-stream connector:

```
Peer A socket
     │
     │ splice()
     ▼
    pipe
     │
     │ splice()
     ▼
Peer B socket
```

and simultaneously in the opposite direction.

Linux `splice()` requires a pipe as one endpoint, so the conceptual forwarding operation is socket → pipe → socket rather than directly socket → socket.

If the protocol can transition from an application-understood setup phase into an opaque paired-stream phase, the server may be able to stop copying signaling payloads into ordinary userspace buffers.

This is an optimization experiment, not a requirement for the MVP. It should be benchmarked against a conventional event-driven forwarding implementation.

## 8. TLS / WebSocket Complication

TLS makes zero-copy forwarding substantially more interesting.

If TLS terminates in a normal userspace TLS library, the application generally needs to participate in encryption/decryption, which interferes with a simple `splice()`-based opaque forwarding path.

Linux kTLS should therefore be investigated.

Questions for the prototype:

- Can kTLS be used for the relevant HTTPS/WSS connection model?
- What combinations of kTLS and `splice()` are supported in the target Linux kernel?
- Can a connection transition into an efficient opaque forwarding phase after pairing?
- Does WebSocket framing prevent the desired byte-stream forwarding model?
- Would a simpler signaling transport after initial HTTPS bootstrap make more sense?
- Is the measurable benefit worth the added complexity?

No architectural claim should be made until this is tested experimentally.

Importantly, `splice()` applies only to signaling/control traffic. It must never become an excuse to relay file data through the server.

## 9. WebRTC and NAT Traversal

The clients should use STUN/ICE to attempt direct connectivity.

TURN is intentionally excluded.

TURN would make difficult NAT/firewall configurations work by relaying traffic, but that would put file bytes into the service's infrastructure and destroy several core properties of the product:

- Server bandwidth would scale with file size.
- Infrastructure cost would increase dramatically.
- “Your files never touch our servers” would no longer be strictly true.
- The wonderfully simple architecture would become a conventional relay service.

Therefore:

```
DIRECT CONNECTION SUCCEEDS → TRANSFER FILE.
DIRECT CONNECTION FAILS    → EXPLAIN FAILURE.
```

One of the first prototype measurements should be the real-world percentage of attempted transfers that successfully establish direct connections across common home, office, cellular, VPN, and CGNAT environments.

## 10. File Transfer Protocol

WebRTC DataChannel provides the transport, but the application still needs a small file-transfer protocol.

Initial protocol needs:

- File name.
- File size.
- MIME type if useful.
- Chunk sequence/offset.
- End-of-file indication.
- Integrity hash.
- Receiver acceptance before transfer.
- Backpressure.

Large files must not be loaded entirely into browser memory. The sender should read incrementally and respect DataChannel buffering/backpressure.

The receiver should stream data toward browser-supported persistent output where practical rather than accumulating an entire multi-gigabyte file in RAM.

Resume/reconnect is explicitly not required for the first MVP.

## 11. Security Model

The session code is part of the security boundary and should not be treated casually.

Requirements:

- Session identifiers must have enough entropy to resist guessing.
- Human-readable codes should avoid ambiguous characters.
- Sessions should expire quickly.
- Rate-limit code guessing.
- A receiver should not silently join and begin receiving data.
- The sender should visibly know when a peer connects.
- Consider an explicit sender confirmation before file transfer begins.
- The server should retain no file metadata longer than required for the ephemeral session.
- WebRTC transport encryption should remain enabled normally.
- No analytics should capture filenames or signaling payloads.

The QR code can encode a sufficiently strong join secret even if the manually typed representation is optimized for usability. The exact pairing/authentication design should be threat-modeled before implementation.

## 12. MVP

The first version should remain aggressively small.

MVP features:

- Static web application.
- Drag-and-drop one file.
- Paste text or supported clipboard content to send explicitly.
- One-click Copy action on received text.
- Create ephemeral session.
- Human-readable pairing code.
- QR code.
- Direct WebRTC DataChannel.
- Progress indicator.
- Transfer rate.
- Clear “direct connection established” state.
- Clear failure when direct connectivity cannot be established.
- Tiny signaling server.
- STUN support.
- No TURN.

Explicitly defer:

- Accounts.
- Multiple recipients.
- Persistent links.
- Offline delivery.
- Cloud storage.
- Transfer history.
- Folder synchronization.
- Background clipboard synchronization.
- Resume.
- Native applications.
- TURN.

## 13. Prototype / Research Plan

**Phase 1 — Prove the Product.**
Build the smallest browser-to-browser transfer possible: first files, then explicit pasted text/clipboard content. Verify large-file behavior and simple copy/paste UX.

**Phase 2 — Measure Direct Connectivity.**
Test across home networks, office networks, cellular hotspots, VPNs, IPv4, IPv6, CGNAT, and restrictive firewalls. Determine whether “P2P or nothing” succeeds often enough to be a useful product.

**Phase 3 — Build the Tiny Signaling Server.**
Implement the minimum ephemeral session state and benchmark memory and CPU consumption.

**Phase 4 — Abuse the Server.**
Generate tens of thousands of synthetic signaling connections. Measure memory per connection/session, CPU behavior, file-descriptor limits, network stack limits, and connection churn.

**Phase 5 — Explore Linux Fast Paths.**
Experiment with `splice()`, pipes, `io_uring`/`epoll` as appropriate, kTLS, and alternative signaling framing. Keep optimizations only when benchmarks justify them.

**Phase 6 — Find the Ridiculous VM.**
Progressively reduce VM resources until the service stops meeting its targets.

The result should be a concrete claim such as:

> “X simultaneous users on Y MB of RAM and Z vCPU.”

## 14. Success Criteria

The project succeeds if:

1. Moving a file or pasted content between two computers is almost frictionless.
2. No account is required.
3. File and clipboard contents never pass through service infrastructure.
4. Direct peer-to-peer connectivity works for a useful fraction of real-world users.
5. Multi-gigabyte files can transfer without excessive browser memory use.
6. The signaling service can support surprisingly large concurrency on an extremely small VM.
7. The implementation remains understandable and small despite aggressive optimization.

## 15. Guiding Principle

Both computers are online.
They connect directly.
Bytes go from one to the other.

That is the product.

The server should do almost nothing — and do that almost nothing exceptionally well.
