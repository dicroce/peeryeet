// peeryeet-signal: WebRTC signaling server.
//
// Single-threaded epoll loop speaking just enough WebSocket to pair two
// browsers and forward their signaling frames. This is the conventional
// baseline that later fast-path experiments (splice, kTLS, io_uring) are
// measured against. Protocol: docs/signaling-protocol.md.

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#define MAX_HANDSHAKE    8192
#define MAX_MESSAGE      65535  // fits a 16-bit WebSocket length
#define MAX_FRAME        (MAX_MESSAGE + 14)
#define MAX_OUT          (256 * 1024)

#define HANDSHAKE_SECS   10
#define IDLE_SECS        60
#define WAIT_SECS        600
#define PAIRED_SECS      300

#define CODE_MAX         24
#define SESSION_BUCKETS  65536
#define RATE_BUCKETS     4096
#define RATE_BURST       10
#define RATE_REFILL_SECS 6

enum { ST_HANDSHAKE, ST_OPEN };

struct session;

struct conn {
	int fd;
	uint8_t state;
	uint8_t trusted_proxy;  // TCP peer is loopback, so X-Forwarded-For is believed
	uint32_t ip_hash;
	int64_t deadline;
	struct session *sess;
	uint8_t *in;            // leftover partial input between reads (usually NULL)
	uint32_t in_len;
	uint8_t *out;           // unsent output after a short write (usually NULL)
	uint32_t out_len;
};

struct session {
	struct session *next;   // hash chain
	struct conn *peer[2];   // [0] creator, [1] joiner
	uint8_t code_len;
	char code[CODE_MAX];
};

struct rate {
	int32_t tokens;
	int64_t last;
};

static int ep;
static int64_t now;
static struct conn **conns;
static int conns_cap;
static int hi_fd;  // highest fd ever handed to a connection
static struct session *sessions[SESSION_BUCKETS];
static struct rate rates[RATE_BUCKETS];
static uint8_t rbuf[2 * MAX_FRAME];

static struct {
	uint64_t conns, sessions, paired, forwarded, forwarded_bytes, failed_joins;
	uint64_t open_conns, open_sessions;
} st;

static const char *words[] = {
	"ACORN", "ALPHA", "AMBER", "ANCHOR", "APPLE", "APRON", "ARROW", "ATLAS",
	"BACON", "BADGE", "BAGEL", "BANANA", "BANJO", "BARREL", "BASIL", "BEACH",
	"BEACON", "BEAR", "BEAVER", "BELL", "BERRY", "BISON", "BLADE", "BLOOM",
	"BOAT", "BONGO", "BRASS", "BREAD", "BRICK", "BRIDGE", "BROOK", "BUCKET",
	"BUNNY", "CABIN", "CACTUS", "CAMEL", "CANDLE", "CANOE", "CANYON", "CARGO",
	"CARROT", "CASTLE", "CEDAR", "CHALK", "CHERRY", "CHESS", "CIDER", "CIRCUS",
	"CLOUD", "CLOVER", "COBRA", "COCOA", "COMET", "CORAL", "COTTON", "COUCH",
	"COYOTE", "CRANE", "CRAYON", "CRYSTAL", "DAISY", "DELTA", "DESERT", "DINGO",
	"DOLPHIN", "DOMINO", "DONUT", "DRAGON", "DRUM", "EAGLE", "EASEL", "ECHO",
	"EMBER", "EMERALD", "FALCON", "FEATHER", "FERN", "FERRY", "FIDDLE", "FIG",
	"FLAME", "FLUTE", "FOREST", "FOSSIL", "FOX", "GALAXY", "GARDEN", "GECKO",
	"GEYSER", "GINGER", "GIRAFFE", "GLACIER", "GOBLIN", "GOOSE", "GRAPE", "GRAVY",
	"GUITAR", "HAMMER", "HAMSTER", "HARBOR", "HAZEL", "HELMET", "HERON", "HIPPO",
	"HONEY", "HORSE", "HUSKY", "IGLOO", "IGUANA", "ISLAND", "IVORY", "JACKAL",
	"JAGUAR", "JASMINE", "JELLY", "JESTER", "JUNGLE", "KAYAK", "KETTLE", "KIWI",
	"KOALA", "LADDER", "LAGOON", "LAMP", "LANTERN", "LASER", "LEMON", "LEOPARD",
	"LILAC", "LIME", "LION", "LLAMA", "LOBSTER", "LOTUS", "MAGNET", "MANGO",
	"MAPLE", "MARBLE", "MEADOW", "MELON", "METEOR", "MINT", "MIRROR", "MITTEN",
	"MOCHA", "MONKEY", "MOOSE", "MUFFIN", "NACHO", "NAPKIN", "NEBULA", "NECTAR",
	"NOODLE", "NUTMEG", "OCEAN", "OLIVE", "ONION", "OPAL", "ORANGE", "ORBIT",
	"OSPREY", "OTTER", "OYSTER", "PADDLE", "PANDA", "PAPAYA", "PARROT", "PEACH",
	"PEANUT", "PEBBLE", "PELICAN", "PEPPER", "PIANO", "PICKLE", "PILLOW", "PIRATE",
	"PIZZA", "PLANET", "PLUM", "POLAR", "PONY", "POPCORN", "POTATO", "PRISM",
	"PUFFIN", "PUMPKIN", "PUPPET", "QUAIL", "QUARTZ", "QUILL", "RABBIT", "RADAR",
	"RADISH", "RAVEN", "REEF", "RHINO", "RIBBON", "RIVER", "ROBIN", "ROCKET",
	"SADDLE", "SALMON", "SATURN", "SCARF", "SHERPA", "SHIELD", "SILVER", "SKETCH",
	"SLOTH", "SNAIL", "SPARROW", "SPHINX", "SPIDER", "SPRUCE", "SQUID", "STORM",
	"SUMMIT", "SUNSET", "SWAN", "TACO", "TANGO", "TEAPOT", "THUNDER", "TIGER",
	"TOAST", "TOMATO", "TORCH", "TOUCAN", "TRUMPET", "TULIP", "TUNDRA", "TURTLE",
	"UMBRELLA", "UNICORN", "VALLEY", "VELVET", "VIOLET", "VIOLIN", "VOLCANO", "WAFFLE",
	"WALNUT", "WALRUS", "WASABI", "WHALE", "WILLOW", "WIZARD", "WOMBAT", "YAK",
	"YETI", "YOGURT", "ZEBRA", "ZENITH", "ZEPHYR", "ZIGZAG", "ZINNIA", "ZUCCHINI",
};
_Static_assert(sizeof words / sizeof *words == 256, "codes index words with one random byte");

// ---- small utilities -------------------------------------------------------

static int64_t mono_secs(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
	return ts.tv_sec;
}

static uint32_t fnv1a(const void *p, size_t n)
{
	const uint8_t *b = p;
	uint32_t h = 2166136261u;
	while (n--)
		h = (h ^ *b++) * 16777619u;
	return h;
}

static void sha1(const uint8_t *msg, size_t len, uint8_t out[20])
{
	uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
	uint8_t block[64];
	uint64_t bits = (uint64_t)len * 8;
	size_t total = ((len + 8) / 64 + 1) * 64;

	for (size_t off = 0; off < total; off += 64) {
		for (size_t i = 0; i < 64; i++) {
			size_t j = off + i;
			if (j < len)
				block[i] = msg[j];
			else if (j == len)
				block[i] = 0x80;
			else if (j >= total - 8)
				block[i] = (uint8_t)(bits >> (8 * (total - 1 - j)));
			else
				block[i] = 0;
		}
		uint32_t w[80];
		for (int i = 0; i < 16; i++)
			w[i] = (uint32_t)block[4*i] << 24 | (uint32_t)block[4*i+1] << 16 |
			       (uint32_t)block[4*i+2] << 8 | block[4*i+3];
		for (int i = 16; i < 80; i++) {
			uint32_t x = w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16];
			w[i] = x << 1 | x >> 31;
		}
		uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
		for (int i = 0; i < 80; i++) {
			uint32_t f, k;
			if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999; }
			else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1; }
			else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
			else             { f = b ^ c ^ d;                   k = 0xCA62C1D6; }
			uint32_t t = (a << 5 | a >> 27) + f + e + k + w[i];
			e = d; d = c; c = b << 30 | b >> 2; b = a; a = t;
		}
		h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
	}
	for (int i = 0; i < 5; i++) {
		out[4*i]   = h[i] >> 24;
		out[4*i+1] = h[i] >> 16;
		out[4*i+2] = h[i] >> 8;
		out[4*i+3] = h[i];
	}
}

static size_t base64(const uint8_t *in, size_t n, char *out)
{
	static const char tab[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t o = 0;
	for (size_t i = 0; i < n; i += 3) {
		uint32_t v = (uint32_t)in[i] << 16;
		if (i + 1 < n) v |= (uint32_t)in[i+1] << 8;
		if (i + 2 < n) v |= in[i+2];
		out[o++] = tab[v >> 18 & 63];
		out[o++] = tab[v >> 12 & 63];
		out[o++] = i + 1 < n ? tab[v >> 6 & 63] : '=';
		out[o++] = i + 2 < n ? tab[v & 63] : '=';
	}
	out[o] = 0;
	return o;
}

// ---- output ----------------------------------------------------------------

// Something went wrong writing to c. Don't free it here (callers may be in
// the middle of handling another connection); shutting the socket down makes
// epoll report it and the normal read path closes it.
static void mark_dead(struct conn *c)
{
	shutdown(c->fd, SHUT_RDWR);
}

static void want_write(struct conn *c, int on)
{
	struct epoll_event ev = {.events = EPOLLIN | (on ? EPOLLOUT : 0), .data.fd = c->fd};
	epoll_ctl(ep, EPOLL_CTL_MOD, c->fd, &ev);
}

// Write the iovecs now if nothing is queued; keep whatever doesn't fit.
static void send_iov(struct conn *c, struct iovec *iov, int n)
{
	size_t total = 0;
	for (int i = 0; i < n; i++)
		total += iov[i].iov_len;

	size_t done = 0;
	if (c->out_len == 0) {
		ssize_t r = writev(c->fd, iov, n);
		if (r < 0 && errno != EAGAIN) {
			mark_dead(c);
			return;
		}
		done = r < 0 ? 0 : (size_t)r;
		if (done == total)
			return;
	}
	if (c->out_len + (total - done) > MAX_OUT) {
		mark_dead(c);
		return;
	}
	uint8_t *p = realloc(c->out, c->out_len + (total - done));
	if (!p) {
		mark_dead(c);
		return;
	}
	c->out = p;
	int was_empty = c->out_len == 0;
	for (int i = 0; i < n; i++) {
		size_t len = iov[i].iov_len;
		const uint8_t *base = iov[i].iov_base;
		if (done >= len) {
			done -= len;
			continue;
		}
		memcpy(c->out + c->out_len, base + done, len - done);
		c->out_len += len - done;
		done = 0;
	}
	if (was_empty)
		want_write(c, 1);
}

static void flush_out(struct conn *c)
{
	ssize_t r = write(c->fd, c->out, c->out_len);
	if (r < 0) {
		if (errno != EAGAIN)
			mark_dead(c);
		return;
	}
	c->out_len -= r;
	if (c->out_len) {
		memmove(c->out, c->out + r, c->out_len);
		return;
	}
	free(c->out);
	c->out = NULL;
	want_write(c, 0);
}

static void send_frame(struct conn *c, uint8_t opcode, const void *a, size_t alen,
		       const void *b, size_t blen)
{
	uint8_t hdr[4];
	size_t len = alen + blen, hl = 2;
	hdr[0] = 0x80 | opcode;
	if (len < 126) {
		hdr[1] = len;
	} else {
		hdr[1] = 126;
		hdr[2] = len >> 8;
		hdr[3] = len;
		hl = 4;
	}
	struct iovec iov[3] = {
		{hdr, hl}, {(void *)a, alen}, {(void *)b, blen},
	};
	send_iov(c, iov, blen ? 3 : 2);
}

static void send_text(struct conn *c, char type, const char *payload)
{
	send_frame(c, 0x1, &type, 1, payload, payload ? strlen(payload) : 0);
}

// ---- sessions --------------------------------------------------------------

static struct session **bucket(const char *code, size_t n)
{
	return &sessions[fnv1a(code, n) & (SESSION_BUCKETS - 1)];
}

static struct session *find_session(const char *code, size_t n)
{
	for (struct session *s = *bucket(code, n); s; s = s->next)
		if (s->code_len == n && memcmp(s->code, code, n) == 0)
			return s;
	return NULL;
}

static struct session *new_session(struct conn *creator)
{
	struct session *s = calloc(1, sizeof *s);
	if (!s)
		return NULL;
	do {
		uint8_t r[6];
		if (getrandom(r, sizeof r, 0) != sizeof r) {
			free(s);
			return NULL;
		}
		uint32_t num = ((uint32_t)r[2] << 24 | (uint32_t)r[3] << 16 |
				(uint32_t)r[4] << 8 | r[5]) % 10000;
		s->code_len = snprintf(s->code, sizeof s->code, "%s-%s-%04u",
				       words[r[0]], words[r[1]], num);
	} while (find_session(s->code, s->code_len));

	struct session **b = bucket(s->code, s->code_len);
	s->next = *b;
	*b = s;
	s->peer[0] = creator;
	st.sessions++;
	st.open_sessions++;
	return s;
}

static void free_session(struct session *s)
{
	struct session **p = bucket(s->code, s->code_len);
	while (*p != s)
		p = &(*p)->next;
	*p = s->next;
	free(s);
	st.open_sessions--;
}

// The failed-join bucket for this client, refilled up to now.
static struct rate *rate_get(struct conn *c)
{
	struct rate *r = &rates[c->ip_hash & (RATE_BUCKETS - 1)];
	if (r->last == 0) {
		r->tokens = RATE_BURST;
		r->last = now;
	}
	int64_t refill = (now - r->last) / RATE_REFILL_SECS;
	if (refill) {
		r->tokens = r->tokens + refill > RATE_BURST ? RATE_BURST : r->tokens + refill;
		r->last += refill * RATE_REFILL_SECS;
	}
	return r;
}

// ---- connections -----------------------------------------------------------

static void close_conn(struct conn *c)
{
	struct session *s = c->sess;
	if (s) {
		struct conn *other = s->peer[0] == c ? s->peer[1] : s->peer[0];
		if (other) {
			send_text(other, 'X', NULL);
			other->sess = NULL;
			other->deadline = now + IDLE_SECS;
		}
		free_session(s);
	}
	epoll_ctl(ep, EPOLL_CTL_DEL, c->fd, NULL);
	close(c->fd);
	conns[c->fd] = NULL;
	free(c->in);
	free(c->out);
	free(c);
	st.open_conns--;
}

// Returns -1 if the connection should be closed.
static int on_message(struct conn *c, uint8_t *p, size_t n)
{
	if (n == 0)
		return -1;

	switch (p[0]) {
	case 'C': {
		if (c->sess) {
			send_text(c, 'E', "already in a session");
			return 0;
		}
		struct session *s = new_session(c);
		if (!s)
			return -1;
		c->sess = s;
		c->deadline = now + WAIT_SECS;
		send_frame(c, 0x1, "C", 1, s->code, s->code_len);
		return 0;
	}
	case 'J': {
		if (c->sess) {
			send_text(c, 'E', "already in a session");
			return 0;
		}
		struct rate *r = rate_get(c);
		if (r->tokens <= 0) {
			send_text(c, 'E', "Too many attempts. Wait a minute and try again.");
			return -1;
		}
		struct session *s = find_session((char *)p + 1, n - 1);
		if (!s || s->peer[1]) {
			st.failed_joins++;
			r->tokens--;
			send_text(c, 'E', s ? "That code is already in use."
					    : "No transfer with that code. Check it and try again.");
			return 0;
		}
		s->peer[1] = c;
		c->sess = s;
		c->deadline = s->peer[0]->deadline = now + PAIRED_SECS;
		st.paired++;
		send_text(c, 'J', NULL);
		send_text(s->peer[0], 'P', NULL);
		return 0;
	}
	case 'S': {
		struct session *s = c->sess;
		if (!s || !s->peer[1]) {
			send_text(c, 'E', "not paired");
			return 0;
		}
		struct conn *other = s->peer[0] == c ? s->peer[1] : s->peer[0];
		st.forwarded++;
		st.forwarded_bytes += n;
		send_frame(other, 0x1, p, n, NULL, 0);
		return 0;
	}
	default:
		send_text(c, 'E', "bad message");
		return 0;
	}
}

static const char *find_header(const char *req, const char *name, size_t *len)
{
	size_t nlen = strlen(name);
	for (const char *line = strstr(req, "\r\n"); line; line = strstr(line, "\r\n")) {
		line += 2;
		if (strncasecmp(line, name, nlen) == 0 && line[nlen] == ':') {
			const char *v = line + nlen + 1;
			while (*v == ' ' || *v == '\t')
				v++;
			const char *end = strstr(v, "\r\n");
			while (end > v && (end[-1] == ' ' || end[-1] == '\t'))
				end--;
			*len = end - v;
			return v;
		}
	}
	return NULL;
}

static int on_handshake(struct conn *c, char *req)
{
	static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
	static const char bad[] =
		"HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";

	size_t klen, ulen;
	const char *key = find_header(req, "Sec-WebSocket-Key", &klen);
	const char *upg = find_header(req, "Upgrade", &ulen);
	if (strncmp(req, "GET ", 4) != 0 || !key || klen == 0 || klen > 64 || !upg ||
	    ulen != 9 || strncasecmp(upg, "websocket", 9) != 0) {
		struct iovec iov = {(void *)bad, sizeof bad - 1};
		send_iov(c, &iov, 1);
		return -1;
	}

	if (c->trusted_proxy) {
		size_t flen;
		const char *fwd = find_header(req, "X-Forwarded-For", &flen);
		if (fwd) {
			// The last entry is the one our own proxy appended.
			const char *last = fwd;
			for (const char *q = fwd; q < fwd + flen; q++)
				if (*q == ',')
					last = q + 1;
			while (*last == ' ')
				last++;
			c->ip_hash = fnv1a(last, fwd + flen - last);
		}
	}

	char buf[64 + sizeof guid];
	memcpy(buf, key, klen);
	memcpy(buf + klen, guid, sizeof guid - 1);
	uint8_t digest[20];
	sha1((uint8_t *)buf, klen + sizeof guid - 1, digest);
	char accept[32];
	base64(digest, 20, accept);

	char resp[160];
	int n = snprintf(resp, sizeof resp,
			 "HTTP/1.1 101 Switching Protocols\r\n"
			 "Upgrade: websocket\r\n"
			 "Connection: Upgrade\r\n"
			 "Sec-WebSocket-Accept: %s\r\n\r\n", accept);
	struct iovec iov = {resp, n};
	send_iov(c, &iov, 1);
	c->state = ST_OPEN;
	c->deadline = now + IDLE_SECS;
	return 0;
}

static void unmask(uint8_t *p, size_t n, const uint8_t mask[4])
{
	uint64_t m8;
	uint8_t m[8] = {mask[0], mask[1], mask[2], mask[3], mask[0], mask[1], mask[2], mask[3]};
	memcpy(&m8, m, 8);
	size_t i = 0;
	for (; i + 8 <= n; i += 8) {
		uint64_t v;
		memcpy(&v, p + i, 8);
		v ^= m8;
		memcpy(p + i, &v, 8);
	}
	for (; i < n; i++)
		p[i] ^= mask[i & 3];
}

// Consume as much of data as forms complete units. Returns bytes consumed,
// or -1 if the connection should be closed.
static ssize_t process(struct conn *c, uint8_t *data, size_t len)
{
	size_t off = 0;

	if (c->state == ST_HANDSHAKE) {
		uint8_t *end = memmem(data, len, "\r\n\r\n", 4);
		if (!end)
			return len > MAX_HANDSHAKE ? -1 : 0;
		end[2] = 0;  // headers become a C string; every line still ends in \r\n
		if (on_handshake(c, (char *)data) < 0)
			return -1;
		off = end - data + 4;
	}

	while (len - off >= 2) {
		uint8_t *f = data + off;
		uint8_t fin = f[0] & 0x80, rsv = f[0] & 0x70, op = f[0] & 0x0f;
		uint64_t plen = f[1] & 0x7f;
		size_t hl = 2;
		if (rsv || !(f[1] & 0x80))  // no extensions; clients must mask
			return -1;
		if (plen == 126) {
			if (len - off < 4)
				break;
			plen = (uint64_t)f[2] << 8 | f[3];
			hl = 4;
		} else if (plen == 127) {
			if (len - off < 10)
				break;
			plen = 0;
			for (int i = 0; i < 8; i++)
				plen = plen << 8 | f[2 + i];
			hl = 10;
		}
		if (plen > MAX_MESSAGE)
			return -1;
		if (len - off < hl + 4 + plen)
			break;

		uint8_t *payload = f + hl + 4;
		unmask(payload, plen, f + hl);
		off += hl + 4 + plen;

		if (!fin || op == 0x0)  // fragmented messages aren't supported
			return -1;
		switch (op) {
		case 0x1:
		case 0x2:
			if (on_message(c, payload, plen) < 0)
				return -1;
			break;
		case 0x8:
			send_frame(c, 0x8, payload, plen < 2 ? plen : 2, NULL, 0);
			return -1;
		case 0x9:
			send_frame(c, 0xA, payload, plen, NULL, 0);
			break;
		case 0xA:
			break;
		default:
			return -1;
		}
	}
	return off;
}

static void on_readable(struct conn *c)
{
	size_t have = c->in_len;
	if (have > MAX_FRAME) {  // can't happen: process() caps leftovers
		close_conn(c);
		return;
	}
	if (have)
		memcpy(rbuf, c->in, have);
	ssize_t r = read(c->fd, rbuf + have, sizeof rbuf - have);
	if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)) {
		close_conn(c);
		return;
	}
	if (r < 0)
		return;

	size_t len = have + r;
	ssize_t used = process(c, rbuf, len);
	if (used < 0) {
		if (c->out_len)  // best effort: get the error/close frame out first
			flush_out(c);
		close_conn(c);
		return;
	}

	size_t left = len - used;
	if (left != c->in_len) {
		uint8_t *p = left ? realloc(c->in, left) : NULL;
		if (left && !p) {
			close_conn(c);
			return;
		}
		if (!left)
			free(c->in);
		c->in = p;
	}
	if (left)
		memcpy(c->in, rbuf + used, left);
	c->in_len = left;
}

static void on_accept(int lfd)
{
	for (;;) {
		struct sockaddr_storage sa;
		socklen_t slen = sizeof sa;
		int fd = accept4(lfd, (struct sockaddr *)&sa, &slen, SOCK_NONBLOCK | SOCK_CLOEXEC);
		if (fd < 0) {
			if (errno != EAGAIN && errno != EINTR)
				perror("accept4");
			return;
		}
		if (fd >= conns_cap) {
			close(fd);
			continue;
		}
		struct conn *c = calloc(1, sizeof *c);
		if (!c) {
			close(fd);
			continue;
		}
		int one = 1;
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

		c->fd = fd;
		c->state = ST_HANDSHAKE;
		c->deadline = now + HANDSHAKE_SECS;
		if (sa.ss_family == AF_INET) {
			struct sockaddr_in *in = (struct sockaddr_in *)&sa;
			c->ip_hash = fnv1a(&in->sin_addr, 4);
			c->trusted_proxy = (ntohl(in->sin_addr.s_addr) >> 24) == 127;
		} else if (sa.ss_family == AF_INET6) {
			struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)&sa;
			c->ip_hash = fnv1a(&in6->sin6_addr, 8);  // per /64
			c->trusted_proxy = IN6_IS_ADDR_LOOPBACK(&in6->sin6_addr);
		}

		struct epoll_event ev = {.events = EPOLLIN, .data.fd = fd};
		if (epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev) < 0) {
			close(fd);
			free(c);
			continue;
		}
		conns[fd] = c;
		if (fd > hi_fd)
			hi_fd = fd;
		st.conns++;
		st.open_conns++;
	}
}

static void sweep(int maxfd)
{
	for (int fd = 0; fd <= maxfd; fd++) {
		struct conn *c = conns[fd];
		if (!c || c->deadline > now)
			continue;
		if (c->state == ST_OPEN) {
			send_text(c, 'E', c->sess ? "This transfer code expired." : "Timed out.");
			if (c->out_len)
				flush_out(c);
		}
		close_conn(c);
	}
}

static void log_stats(void)
{
	fprintf(stderr,
		"stats: open_conns=%lu open_sessions=%lu | total conns=%lu sessions=%lu "
		"paired=%lu forwarded=%lu (%lu bytes) failed_joins=%lu\n",
		st.open_conns, st.open_sessions, st.conns, st.sessions, st.paired,
		st.forwarded, st.forwarded_bytes, st.failed_joins);
}

int main(int argc, char **argv)
{
	const char *addr = "127.0.0.1";
	int port = 9000, opt;
	while ((opt = getopt(argc, argv, "l:p:")) != -1) {
		switch (opt) {
		case 'l': addr = optarg; break;
		case 'p': port = atoi(optarg); break;
		default:
			fprintf(stderr, "usage: %s [-l listen_addr] [-p port]\n", argv[0]);
			return 2;
		}
	}

	signal(SIGPIPE, SIG_IGN);

	struct rlimit rl;
	getrlimit(RLIMIT_NOFILE, &rl);
	rl.rlim_cur = rl.rlim_max;
	setrlimit(RLIMIT_NOFILE, &rl);
	conns_cap = rl.rlim_cur > (1 << 22) ? (1 << 22) : (int)rl.rlim_cur;
	conns = calloc(conns_cap, sizeof *conns);
	if (!conns) {
		perror("calloc");
		return 1;
	}

	struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons(port)};
	if (inet_pton(AF_INET, addr, &sa.sin_addr) != 1) {
		fprintf(stderr, "bad listen address: %s\n", addr);
		return 2;
	}
	int lfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	int one = 1;
	setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	if (bind(lfd, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(lfd, 4096) < 0) {
		perror("bind/listen");
		return 1;
	}

	ep = epoll_create1(EPOLL_CLOEXEC);
	struct epoll_event lev = {.events = EPOLLIN, .data.fd = lfd};
	epoll_ctl(ep, EPOLL_CTL_ADD, lfd, &lev);
	fprintf(stderr, "listening on %s:%d (max %d fds)\n", addr, port, conns_cap);

	int64_t last_sweep = 0, last_log = 0;
	uint64_t logged_conns = 0;
	struct epoll_event evs[256];
	for (;;) {
		int n = epoll_wait(ep, evs, 256, 1000);
		now = mono_secs();
		for (int i = 0; i < n; i++) {
			int fd = evs[i].data.fd;
			if (fd == lfd) {
				on_accept(lfd);
				continue;
			}
			struct conn *c = conns[fd];
			if (!c)
				continue;
			if (evs[i].events & (EPOLLIN | EPOLLHUP | EPOLLERR))
				on_readable(c);
			if (conns[fd] == c && c->out_len && (evs[i].events & EPOLLOUT))
				flush_out(c);
		}
		if (now != last_sweep) {
			sweep(hi_fd);
			last_sweep = now;
		}
		if (now - last_log >= 60 && st.conns != logged_conns) {
			log_stats();
			logged_conns = st.conns;
			last_log = now;
		}
	}
}
