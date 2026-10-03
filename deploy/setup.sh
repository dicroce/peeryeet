#!/bin/sh
# Idempotent setup for the peeryeet.com host (Amazon Linux 2023, x86_64).
# Run on the server from the repo's deploy/ directory: sudo ./setup.sh
set -eu

cd "$(dirname "$0")"

# Caddy: static binary from the official download API
if [ ! -x /usr/local/bin/caddy ]; then
	curl -fsSL "https://caddyserver.com/api/download?os=linux&arch=amd64" -o /tmp/caddy
	install -m 0755 /tmp/caddy /usr/local/bin/caddy
	rm -f /tmp/caddy
fi

# Service user and directories
getent group caddy >/dev/null || groupadd --system caddy
id caddy >/dev/null 2>&1 || useradd --system --gid caddy --create-home --home-dir /var/lib/caddy --shell /usr/sbin/nologin caddy
mkdir -p /etc/caddy /srv/peeryeet

# Config and unit
install -m 0644 Caddyfile /etc/caddy/Caddyfile
install -m 0644 caddy.service /etc/systemd/system/caddy.service

# Static site
cp -r ../web/. /srv/peeryeet/
chown -R caddy:caddy /srv/peeryeet

# Signaling server, built from source
command -v cc >/dev/null || dnf install -y gcc make
make -C ../server
install -m 0755 ../server/peeryeet-signal /usr/local/bin/peeryeet-signal
install -m 0644 peeryeet-signal.service /etc/systemd/system/peeryeet-signal.service

systemctl daemon-reload
systemctl enable caddy peeryeet-signal
systemctl restart peeryeet-signal caddy
systemctl --no-pager status peeryeet-signal caddy
