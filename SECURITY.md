# Security

If you find a security problem in PeerYeet, please report it privately
through GitHub: open the repository's **Security** tab and choose
**Report a vulnerability**. Please don't open a public issue for it.

Things that matter most:

- Any way for the signaling server, or anyone else, to read or alter
  transferred data.
- Any way to crash or take over the signaling server (`server/signal.c`
  parses untrusted HTTP and WebSocket input).
- Any way to join someone else's transfer without their code.

This is a free, one-person project, so there's no bug bounty, but every
report is read and credited in the fix if you'd like.
