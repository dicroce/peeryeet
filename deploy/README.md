# Deployment

Host: AWS t3.micro, Amazon Linux 2023, `ssh peeryeet` (see `~/.ssh/config`).

## Manual AWS setup (not in code)

- EC2 instance with an Elastic IP associated.
- Security group inbound: 22 from my IP only; 80 and 443 from anywhere.
- Route 53 (domain registered there): A records for `peeryeet.com` and `www` -> Elastic IP.
- SSH key `~/.ssh/peeryeet-prod.pem` (never committed; `*.pem` is gitignored).

## Server setup

From the repo root on the server (or after copying the repo over):

    sudo ./deploy/setup.sh

Installs Caddy, which serves `web/` from `/srv/peeryeet`, gets Let's Encrypt
certificates automatically, and proxies `/ws` to the signaling server on
`localhost:9000`. Builds `server/` (installing gcc if needed) and runs it as
the `peeryeet-signal` systemd service.

Re-run the same command after pulling changes; it's idempotent.

Signaling server stats are logged to the journal about once a minute:

    journalctl -u peeryeet-signal -f
