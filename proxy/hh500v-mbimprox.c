/*
 * hh500v-mbimprox 鈥?keep the MBIM session open so SMS can be read while
 * data keeps flowing, WITHOUT changing how netifd dials.
 *
 * How it works:
 *   1. open the real char device (/dev/wwan0mbim0) and keep it
 *   2. allocate a pty, put it in raw mode, bind-mount it over the device path
 *   3. relay bytes both ways; netifd/umbim see a normal MBIM device
 *   4. swallow the CLOSE that umbim sends after "connect" (so the session
 *      stays alive) and answer it with a synthetic CLOSE_DONE
 *   5. inject our own SMS_READ commands on that same session and swallow
 *      only the replies that carry our transaction ids
 *
 * Result: dialing path untouched (same device, same proto, same umbim),
 * yet SMS can be polled any time without tearing the bearer down.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <time.h>
#include <signal.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>

#define DEF_DEV   "/dev/wwan0mbim0"
#define DEF_PTS   "/dev/hh500v-mbim"
#define SMS_OUT   "/tmp/hh500v-sms-pdu.txt"
#define SMS_RAW   "/tmp/hh500v-sms-raw.txt"
#define FRAG_MAX  8192
static unsigned char g_frag[FRAG_MAX];
static size_t g_frag_len;
static uint32_t g_frag_txn, g_frag_total, g_frag_next;

#define MT_OPEN         0x00000001u
#define MT_CLOSE        0x00000002u
#define MT_COMMAND      0x00000003u
#define MT_OPEN_DONE    0x80000001u
#define MT_CLOSE_DONE   0x80000002u
#define MT_COMMAND_DONE 0x80000003u
#define MT_INDICATE     0x80000007u

#define OUR_TXN_BASE    0x40000000u

/* MbimSmsFormat in libmbim 1.30 (the one on this box, verified in
 * mbim-enums.h @ 1.30.0):  MBIM_SMS_FORMAT_PDU = 0, MBIM_SMS_FORMAT_CDMA = 1.
 * Sending 1 made the modem answer 0x68 = MBIM_STATUS_ERROR_SMS_FORMAT_NOT_SUPPORTED. */
#define SMS_FMT_PDU     0
#define SMS_FMT_CDMA    1

static int g_verbose;
static FILE *g_log;
static const char *g_dev = DEF_DEV;
static const char *g_pts = DEF_PTS;
static int g_interval = 60;

static void lg(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	if (g_log) {
		time_t t = time(NULL);
		struct tm tm;
		localtime_r(&t, &tm);
		fprintf(g_log, "%02d:%02d:%02d ", tm.tm_hour, tm.tm_min, tm.tm_sec);
		vfprintf(g_log, fmt, ap);
		fputc('\n', g_log);
		fflush(g_log);
	}
	if (g_verbose) {
		va_start(ap, fmt);
		vfprintf(stderr, fmt, ap);
		fputc('\n', stderr);
		va_end(ap);
	}
	va_end(ap);
}

static uint32_t rd32(const unsigned char *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr32(unsigned char *p, uint32_t v)
{
	p[0] = v & 0xff;
	p[1] = (v >> 8) & 0xff;
	p[2] = (v >> 16) & 0xff;
	p[3] = (v >> 24) & 0xff;
}

static void hexdump_str(const unsigned char *p, size_t n, char *out)
{
	static const char *hx = "0123456789abcdef";
	size_t i;
	for (i = 0; i < n; i++) {
		out[i * 2] = hx[p[i] >> 4];
		out[i * 2 + 1] = hx[p[i] & 0xf];
	}
	out[n * 2] = 0;
}

/* ---------- stream reassembly ---------- */

typedef struct {
	unsigned char b[131072];
	size_t len;
} sbuf_t;

static void sbuf_append(sbuf_t *s, const unsigned char *p, size_t n)
{
	if (n > sizeof(s->b) - s->len)
		n = sizeof(s->b) - s->len;
	memcpy(s->b + s->len, p, n);
	s->len += n;
}

static void sbuf_drop(sbuf_t *s, size_t n)
{
	if (n >= s->len) { s->len = 0; return; }
	memmove(s->b, s->b + n, s->len - n);
	s->len -= n;
}

/* ---------- global state ---------- */

static int g_real = -1;
static int g_master = -1;
static int g_slave = -1;
static char g_ptsname[128];
static int g_bound;
static int g_session;            /* we believe an MBIM session is open */
static uint32_t g_open_txn;
static uint32_t g_our_txn = OUR_TXN_BASE;   /* next injected transaction id */
static uint32_t g_our_pending;   /* transaction id awaiting reply, 0 = none */
static time_t g_next_poll;
/* number of consecutive SMS_READ failures (0x15 right after a new session) */
static int g_read_fail;
static int g_sms_seen;
static int g_probed;
static int g_svc_sent;           /* DEVICE_SERVICES already asked */
static time_t g_svc_at;          /* when to ask it (after the session settles) */

static void send_to_real(const unsigned char *p, size_t n)
{
	size_t off = 0;
	while (off < n) {
		ssize_t w = write(g_real, p + off, n - off);
		if (w < 0) {
			if (errno == EINTR) continue;
			lg("write real failed: %s", strerror(errno));
			return;
		}
		off += (size_t)w;
	}
}

static void send_to_client(const unsigned char *p, size_t n)
{
	size_t off = 0;
	while (off < n) {
		ssize_t w = write(g_master, p + off, n - off);
		if (w < 0) {
			if (errno == EINTR) continue;
			lg("write pty failed: %s", strerror(errno));
			return;
		}
		off += (size_t)w;
	}
}

static void make_reply(unsigned char *out, uint32_t type, uint32_t txn, uint32_t status)
{
	wr32(out + 0, type);
	wr32(out + 4, 16);
	wr32(out + 8, txn);
	wr32(out + 12, status);
}

/* ---------- SMS ---------- */

static const unsigned short gsm7_basic[128] = {
	0x0040, 0x00a3, 0x0024, 0x00a5, 0x00e8, 0x00e9, 0x00f9, 0x00ec,
	0x00f2, 0x00c7, 0x000a, 0x00d8, 0x00f8, 0x000d, 0x00c5, 0x00e5,
	0x0394, 0x005f, 0x03a6, 0x0393, 0x039b, 0x03a9, 0x03a0, 0x03a8,
	0x03a3, 0x0398, 0x039e, 0x0000, 0x00c6, 0x00e6, 0x00df, 0x00c9,
	0x0020, 0x0021, 0x0022, 0x0023, 0x00a4, 0x0025, 0x0026, 0x0027,
	0x0028, 0x0029, 0x002a, 0x002b, 0x002c, 0x002d, 0x002e, 0x002f,
	0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037,
	0x0038, 0x0039, 0x003a, 0x003b, 0x003c, 0x003d, 0x003e, 0x003f,
	0x00a1, 0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047,
	0x0048, 0x0049, 0x004a, 0x004b, 0x004c, 0x004d, 0x004e, 0x004f,
	0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057,
	0x0058, 0x0059, 0x005a, 0x00c4, 0x00d6, 0x00d1, 0x00dc, 0x00a7,
	0x00bf, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067,
	0x0068, 0x0069, 0x006a, 0x006b, 0x006c, 0x006d, 0x006e, 0x006f,
	0x0070, 0x0071, 0x0072, 0x0073, 0x0074, 0x0075, 0x0076, 0x0077,
	0x0078, 0x0079, 0x007a, 0x00e4, 0x00f6, 0x00f1, 0x00fc, 0x00e0
};

static size_t utf8_put(char *out, size_t sz, size_t pos, unsigned cp)
{
	if (cp < 0x80) {
		if (pos + 1 < sz)
			out[pos++] = (char)cp;
	} else if (cp < 0x800) {
		if (pos + 2 < sz) {
			out[pos++] = (char)(0xc0 | (cp >> 6));
			out[pos++] = (char)(0x80 | (cp & 0x3f));
		}
	} else if (cp < 0x10000) {
		if (pos + 3 < sz) {
			out[pos++] = (char)(0xe0 | (cp >> 12));
			out[pos++] = (char)(0x80 | ((cp >> 6) & 0x3f));
			out[pos++] = (char)(0x80 | (cp & 0x3f));
		}
	} else if (pos + 4 < sz) {
		out[pos++] = (char)(0xf0 | (cp >> 18));
		out[pos++] = (char)(0x80 | ((cp >> 12) & 0x3f));
		out[pos++] = (char)(0x80 | ((cp >> 6) & 0x3f));
		out[pos++] = (char)(0x80 | (cp & 0x3f));
	}
	return pos;
}

static void decode_gsm7(const unsigned char *d, size_t nbytes, size_t start_char,
			size_t nchars, char *out, size_t outsz)
{
	size_t pos = 0, i;

	out[0] = 0;
	for (i = start_char; i < start_char + nchars; i++) {
		size_t bit = i * 7;
		size_t byte = bit / 8;
		unsigned v;
		if (byte >= nbytes)
			break;
		v = d[byte] >> (bit % 8);
		if (byte + 1 < nbytes && (bit % 8) > 1)
			v |= (unsigned)d[byte + 1] << (8 - (bit % 8));
		v &= 0x7f;
		if (v == 0x1b) {	/* extension table */
			i++;
			bit = i * 7;
			byte = bit / 8;
			if (byte >= nbytes)
				break;
			v = d[byte] >> (bit % 8);
			if (byte + 1 < nbytes && (bit % 8) > 1)
				v |= (unsigned)d[byte + 1] << (8 - (bit % 8));
			v &= 0x7f;
			switch (v) {
			case 0x0a: pos = utf8_put(out, outsz, pos, 0x0c); break;
			case 0x14: pos = utf8_put(out, outsz, pos, '^'); break;
			case 0x28: pos = utf8_put(out, outsz, pos, '{'); break;
			case 0x29: pos = utf8_put(out, outsz, pos, '}'); break;
			case 0x2f: pos = utf8_put(out, outsz, pos, '\\'); break;
			case 0x3c: pos = utf8_put(out, outsz, pos, '['); break;
			case 0x3d: pos = utf8_put(out, outsz, pos, '~'); break;
			case 0x3e: pos = utf8_put(out, outsz, pos, ']'); break;
			case 0x40: pos = utf8_put(out, outsz, pos, '|'); break;
			case 0x65: pos = utf8_put(out, outsz, pos, 0x20ac); break;
			default: break;
			}
			continue;
		}
		if (v == 0)
			pos = utf8_put(out, outsz, pos, ' ');
		else
			pos = utf8_put(out, outsz, pos, (unsigned char)gsm7_basic[v]);
	}
	out[pos < outsz ? pos : outsz - 1] = 0;
}

static void decode_ucs2(const unsigned char *d, size_t nbytes, char *out, size_t outsz)
{
	size_t pos = 0, i;

	out[0] = 0;
	for (i = 0; i + 1 < nbytes; i += 2) {
		unsigned cp = ((unsigned)d[i] << 8) | d[i + 1];
		if (cp == 0xffff || cp == 0)
			continue;
		if (cp >= 0xd800 && cp <= 0xdbff && i + 3 < nbytes) {
			unsigned lo = ((unsigned)d[i + 2] << 8) | d[i + 3];
			if (lo >= 0xdc00 && lo <= 0xdfff) {
				cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
				i += 2;
			}
		}
		pos = utf8_put(out, outsz, pos, cp);
	}
	out[pos < outsz ? pos : outsz - 1] = 0;
}

static void decode_addr(const unsigned char *d, size_t ndigits, int toa,
			char *out, size_t outsz)
{
	size_t nbytes = (ndigits + 1) / 2, i, pos = 0;

	out[0] = 0;
	if ((toa & 0x70) == 0x50) {	/* alphanumeric */
		size_t nchars = (ndigits * 4) / 7;
		decode_gsm7(d, nbytes, 0, nchars, out, outsz);
		return;
	}
	if (toa & 0x70)
		pos = utf8_put(out, outsz, pos, '+');
	for (i = 0; i < nbytes && pos + 1 < outsz; i++) {
		unsigned lo = d[i] & 0x0f, hi = d[i] >> 4;
		if (i * 2 < ndigits && lo <= 9)
			out[pos++] = (char)('0' + lo);
		if (i * 2 + 1 < ndigits && hi <= 9)
			out[pos++] = (char)('0' + hi);
	}
	out[pos < outsz ? pos : outsz - 1] = 0;
}

static void decode_scts(const unsigned char *b, char *out, size_t outsz)
{
	unsigned v[7];
	int i;

	for (i = 0; i < 7; i++) {
		/* GSM 03.40 semi-octets are swapped: 0x21 means "12" */
		unsigned lo = b[i] & 0x0f, hi = b[i] >> 4;
		v[i] = (lo <= 9 ? lo : 0) * 10 + (hi <= 9 ? hi : 0);
	}
	snprintf(out, outsz, "20%02u-%02u-%02u %02u:%02u:%02u",
		 v[0], v[1], v[2], v[3], v[4], v[5]);
}

/* Decode one GSM 03.40 SMS-DELIVER PDU. Returns 0 on success.
 *
 * c_ref/c_total/c_seq (all optional) come back non-zero/-1 when the message is
 * one part of a concatenated SMS: the 8-bit UDH IE (0x00) or the 16-bit one
 * (0x08) both carry reference, total parts and this part's sequence number. */
static int pdu_decode(const unsigned char *p, size_t len, char *sender, size_t ssz,
		      char *ts, size_t tsz, char *text, size_t txsz,
		      int *c_ref, int *c_total, int *c_seq)
{
	size_t off, alen, nbytes, udl, ud_bytes, hdr_bytes = 0, skip_septets = 0;
	unsigned fo, toa, dcs;
	int udhi;
	const unsigned char *ud;

	*c_ref = 0;
	*c_total = 1;
	*c_seq = 0;
	if (len < 2)
		return -1;
	off = 1 + (size_t)p[0];		/* skip SMSC */
	if (off + 2 > len)
		return -1;
	fo = p[off++];
	if ((fo & 0x03) != 0x00)	/* only SMS-DELIVER */
		return -1;
	udhi = (fo & 0x40) != 0;
	alen = p[off++];
	nbytes = (alen + 1) / 2;
	if (off + 1 + nbytes + 2 + 7 + 1 > len)
		return -1;
	toa = p[off++];
	decode_addr(p + off, alen, (int)toa, sender, ssz);
	off += nbytes;
	off++;				/* PID */
	dcs = p[off++];
	decode_scts(p + off, ts, tsz);
	off += 7;
	udl = p[off++];
	ud = p + off;
	ud_bytes = len > off ? len - off : 0;

	if (udhi && ud_bytes > 0) {
		hdr_bytes = 1 + (size_t)ud[0];
		if (hdr_bytes > ud_bytes)
			hdr_bytes = ud_bytes;
		skip_septets = (hdr_bytes * 8 + 6) / 7;

		/* walk the information elements looking for the concatenation IE */
		{
			size_t ulen = ud[0], pos = 1;

			if (ulen + 1 > ud_bytes)
				ulen = ud_bytes - 1;
			while (pos + 2 <= ulen + 1) {
				unsigned iei = ud[pos], iel = ud[pos + 1];

				if (pos + 2 + iel > ulen + 1)
					break;
				if (iei == 0x00 && iel == 3) {
					*c_ref = ud[pos + 2];
					*c_total = ud[pos + 3];
					*c_seq = ud[pos + 4];
				} else if (iei == 0x08 && iel == 4) {
					*c_ref = (ud[pos + 2] << 8) | ud[pos + 3];
					*c_total = ud[pos + 4];
					*c_seq = ud[pos + 5];
				}
				pos += 2 + iel;
			}
		}
	}

	if ((dcs & 0x0c) == 0x08) {	/* UCS2 */
		size_t n = ud_bytes - hdr_bytes;
		decode_ucs2(ud + hdr_bytes, n, text, txsz);
	} else {
		size_t n = ud_bytes - hdr_bytes;
		decode_gsm7(ud, ud_bytes, skip_septets,
			    udl > skip_septets ? udl - skip_septets : 0, text, txsz);
		(void)n;
	}
	return 0;
}

#define SMS_DBED  "/etc/hh500v-sms.txt"
#define SMS_HASHES "/etc/hh500v-sms-hashes"

static uint32_t g_seen_hash[64];
static int g_seen_n;

/* hashes survive a proxy restart so that the read-at-session-start does not
 * append the same messages twice */
static void sms_load_hashes(void)
{
	FILE *f = fopen(SMS_HASHES, "r");
	char line[32];

	if (!f)
		return;
	while (g_seen_n < 64 && fgets(line, sizeof(line), f))
		g_seen_hash[g_seen_n++] = (uint32_t)strtoul(line, NULL, 10);
	fclose(f);
}

static uint32_t fnv1a(const unsigned char *d, size_t n)
{
	uint32_t h = 2166136261u;
	size_t i;

	for (i = 0; i < n; i++) {
		h ^= d[i];
		h *= 16777619u;
	}
	return h;
}

/* a long SMS arrives as N separate PDUs that share a concatenation reference;
 * hold the parts in RAM until the set is complete, then store one merged row */
#define PART_MAX 4
#define PART_SLOTS 8
#define PART_TEXT 900

struct part_group {
	int used;
	int ref, total, have;
	char sender[64];
	char ts[32];
	uint32_t index;
	char text[PART_SLOTS][PART_TEXT];
};

static struct part_group g_parts[PART_MAX];

/* one record = one line: no CR/LF/TAB and no '|' may survive in a field */
static void sanitize_field(char *s)
{
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;

		if (c == '\r' || c == '\n' || c == '\t' || c == '|')
			*s = ' ';
	}
}

static void sms_write_row(const char *sender, const char *ts, const char *text,
			  uint32_t index)
{
	FILE *f = fopen(SMS_DBED, "a");

	if (f) {
		/* The store is one "epoch|index|sender|ts|text" line per message, so
		 * a body that contains a newline would silently turn into extra
		 * broken rows (that is exactly how "价值：2560元" and
		 * "拒收请回复R" ended up as their own lines). Flatten first. */
		sanitize_field(text);
		fprintf(f, "%u|%u|%s|%s|%s\n", (unsigned)time(NULL), index, sender, ts, text);
		fclose(f);
	}
	g_sms_seen++;
}

/* returns 1 when the caller must not store the text (still buffering) */
static int sms_concat_put(const char *sender, const char *ts, const char *text,
			  uint32_t index, int ref, int total, int seq)
{
	struct part_group *g = NULL;
	int i, slot = -1;

	if (total < 2 || total > PART_SLOTS || seq < 1 || seq > total)
		return 0;

	for (i = 0; i < PART_MAX; i++) {
		if (g_parts[i].used && g_parts[i].ref == ref && g_parts[i].total == total &&
		    strcmp(g_parts[i].sender, sender) == 0) {
			g = &g_parts[i];
			break;
		}
	}
	if (!g) {
		for (i = 0; i < PART_MAX; i++) {
			if (!g_parts[i].used) {
				g = &g_parts[i];
				slot = i;
				break;
			}
		}
		if (!g)
			return 0;	/* no room: keep this part as a plain message */
		memset(g, 0, sizeof(*g));
		g->used = 1;
		g->ref = ref;
		g->total = total;
		snprintf(g->sender, sizeof(g->sender), "%s", sender);
		snprintf(g->ts, sizeof(g->ts), "%s", ts);
		g->index = index;
		(void)slot;
	}
	g->text[seq - 1][0] = 0;
	snprintf(g->text[seq - 1], PART_TEXT, "%s", text);
	g->have |= 1 << (seq - 1);
	lg("sms part ref=%d %d/%d have=0x%x", ref, seq, total, g->have);

	if (g->have != (1 << total) - 1)
		return 1;

	{
		char merged[2048];
		size_t o = 0;
		int k;

		merged[0] = 0;
		for (k = 0; k < total; k++) {
			size_t n = strlen(g->text[k]);

			if (o + n + 1 >= sizeof(merged))
				break;
			memcpy(merged + o, g->text[k], n);
			o += n;
			merged[o] = 0;
		}
		lg("sms merged ref=%d parts=%d from=%s text=%s", ref, total, g->sender, merged);
		sms_write_row(g->sender, g->ts, merged, g->index);
		g->used = 0;
	}
	return 1;
}

static void sms_store_pdu(const unsigned char *pdu, size_t len, uint32_t index,
			  uint32_t status)
{
	char sender[64], ts[32], text[2048];
	uint32_t h = fnv1a(pdu, len);
	int i, dup = 0, ref = 0, total = 1, seq = 0;

	for (i = 0; i < g_seen_n; i++)
		if (g_seen_hash[i] == h)
			dup = 1;
	if (dup) {
		lg("sms dup idx=%u", index);
		return;
	}
	for (i = 63; i > 0; i--)
		g_seen_hash[i] = g_seen_hash[i - 1];
	g_seen_hash[0] = h;
	if (g_seen_n < 64)
		g_seen_n++;
	{
		FILE *hf = fopen(SMS_HASHES, "a");
		if (hf) {
			fprintf(hf, "%u\n", h);
			fclose(hf);
		}
	}

	sender[0] = ts[0] = text[0] = 0;
	if (pdu_decode(pdu, len, sender, sizeof(sender), ts, sizeof(ts),
		       text, sizeof(text), &ref, &total, &seq) != 0) {
		char hex[1024];
		hexdump_str(pdu, len < 400 ? len : 400, hex);
		sender[0] = 0;
		snprintf(text, sizeof(text), "(undecoded) %s", hex);
		snprintf(ts, sizeof(ts), "-");
		total = 1;
	}
	lg("SMS idx=%u st=%u from=%s at=%s text=%s", index, status, sender, ts, text);

	if (total > 1 && sms_concat_put(sender, ts, text, index, ref, total, seq))
		return;

	sms_write_row(sender, ts, text, index);
}

/* MBIM SMS message list: u32[0] reserved, u32[1] message count, then a
 * count-sized array of {offset, length} references pointing at records of
 * {u32 index, u32 status, u32 0x10, u32 pdu_len, u8 pdu[pdu_len]} */
static void sms_parse_records(const unsigned char *info, uint32_t info_len)
{
	uint32_t count = rd32(info + 4);

	lg("sms count=%u info_len=%u", count, info_len);
	if (count == 0 || count > 64)
		return;

	for (uint32_t i = 0; i < count; i++) {
		size_t ref_off = 8 + (size_t)i * 8;
		if (ref_off + 8 > info_len)
			break;
		uint32_t roff = rd32(info + ref_off);
		uint32_t rlen = rd32(info + ref_off + 4);
		if ((size_t)roff + rlen > info_len || rlen < 16)
			continue;
		const unsigned char *rec = info + roff;
		uint32_t idx = rd32(rec);
		uint32_t st = rd32(rec + 4);
		uint32_t pdu_len = rd32(rec + 12);
		if (pdu_len == 0 || pdu_len > rlen - 16)
			continue;
		sms_store_pdu(rec + 16, pdu_len, idx, st);
	}
}

/* the modem pushes arriving messages as an SMS-service indication that
 * already carries the PDU, so nothing has to be read back while the data
 * session is up */
static void sms_handle_indication(const unsigned char *frame, size_t len)
{
	const unsigned char *info;
	uint32_t info_len, cid = rd32(frame + 36);

	if (len < 44)
		return;
	info_len = rd32(frame + 40);
	info = frame + 44;
	if (info_len > len - 44)
		info_len = (uint32_t)(len - 44);
	lg("SMS URC cid=%u info_len=%u", cid, info_len);
	if (info_len < 12)
		return;
	{
		char raw[4096];
		if (info_len * 2 + 1 < sizeof(raw)) {
			hexdump_str(info, info_len, raw);
			FILE *f = fopen(SMS_RAW, "a");
			if (f) {
				fprintf(f, "urc cid=%u info=%u hex=%s\n", cid,
					info_len, raw);
				fclose(f);
			}
		}
	}
	sms_parse_records(info, info_len);
}

static void sms_handle_done(const unsigned char *frame, size_t len)
{
	/* header(12) + frag_total(4) + frag_cur(4) + svc(16) + cid(4) + status(4) + info_len(4) */
	const unsigned char *info;
	uint32_t status, info_len;
	char raw[4096];

	if (len < 48) {
		lg("short COMMAND_DONE (%zu)", len);
		return;
	}
	status = rd32(frame + 40);
	info_len = rd32(frame + 44);
	info = frame + 48;
	if (info_len > len - 48)
		info_len = (uint32_t)(len - 48);

	lg("sms reply txn=0x%x cid=%u status=0x%x info_len=%u", rd32(frame + 8),
	   rd32(frame + 36), status, info_len);
	/* a failing SMS_READ is a timing thing, not a permanent error: retry soon */
	if (rd32(frame + 36) == 2) {
		if (status != 0) {
			if (g_read_fail < 6)
				g_read_fail++;
			g_next_poll = time(NULL) + 20;
			lg("sms read failed (0x%x), retry in 20s (fail #%d)", status,
			   g_read_fail);
		} else {
			g_read_fail = 0;
			g_next_poll = time(NULL) + g_interval;
		}
	}
	{
		char whole[1024];
		size_t n = len < 200 ? len : 200;
		hexdump_str(frame, n, whole);
		FILE *f = fopen(SMS_RAW, "a");
		if (f) {
			fprintf(f, "cid=%u status=0x%x info_len=%u frame=%s\n",
				rd32(frame + 36), status, info_len, whole);
			fclose(f);
		}
	}
	if (info_len && info_len * 2 + 1 < sizeof(raw)) {
		hexdump_str(info, info_len, raw);
		FILE *f = fopen(SMS_RAW, "a");
		if (f) {
			fprintf(f, "status=0x%x info=%u hex=%s\n", status, info_len, raw);
			fclose(f);
		}
	}
	if (status != 0 || info_len < 12)
		return;
	sms_parse_records(info, info_len);
}

static int mbim_cmd_full(const unsigned char *uuid, uint32_t cid,
			 uint32_t cmd_type, const unsigned char *info,
			 size_t info_len, const char *label)
{
	unsigned char frame[512];
	uint32_t txn = ++g_our_txn;
	size_t total = 48 + info_len;

	if (total > sizeof(frame))
		return -1;
	memset(frame, 0, sizeof(frame));
	wr32(frame + 0, MT_COMMAND);
	wr32(frame + 4, (uint32_t)total);
	wr32(frame + 8, txn);
	wr32(frame + 12, 1);            /* fragment total */
	wr32(frame + 16, 0);            /* fragment current */
	memcpy(frame + 20, uuid, 16);
	wr32(frame + 36, cid);
	wr32(frame + 40, cmd_type);     /* 0 = query, 1 = set */
	wr32(frame + 44, (uint32_t)info_len);
	if (info_len)
		memcpy(frame + 48, info, info_len);
	send_to_real(frame, total);
	g_our_pending = txn;
	lg("injected %s cid=%u type=%u txn=0x%x", label, cid, cmd_type, txn);
	return 0;
}

static int mbim_cmd_uuid(const unsigned char *uuid, uint32_t cid,
			 const unsigned char *info, size_t info_len, const char *label)
{
	return mbim_cmd_full(uuid, cid, 0, info, info_len, label);
}

/* Service UUIDs are written in text order (this is how libmbim/umbim put them
 * on the wire - see the basic-connect uuid in captured umbim frames).
 * Values extracted from libmbim-glib.so.4.8.0 (.rodata table at 0xbef93):
 *   basic connect   a289cc33-bcbb-8b4f-b6b0-133ec2aae6df
 *   sms             533fbeeb-14fe-4467-9f90-33a223e56c3f
 *   ussd            e550a0c8-5e82-479e-82f7-10abf4c3351f
 */
static const unsigned char SMS_UUID[16] = {
	0x53, 0x3f, 0xbe, 0xeb, 0x14, 0xfe, 0x44, 0x67,
	0x9f, 0x90, 0x33, 0xa2, 0x23, 0xe5, 0x6c, 0x3f
};

static const unsigned char DEV_UUID[16] = {
	0xa2, 0x89, 0xcc, 0x0d, 0x4c, 0xae, 0x11, 0xdf,
	0xb0, 0xad, 0x00, 0x02, 0x5e, 0x0a, 0xe8, 0x2a
};

/* same service, old GUID byte order - kept only to probe the modem */
static const unsigned char SMS_UUID_SWAP[16] = {
	0xeb, 0xbe, 0x3f, 0x53, 0xfe, 0x14, 0x67, 0x44,
	0x9f, 0x90, 0x33, 0xa2, 0x23, 0xe5, 0x6a, 0x97
};

static const unsigned char DEV_UUID_SWAP[16] = {
	0x0d, 0xcc, 0x89, 0xa2, 0xae, 0x4c, 0xdf, 0x11,
	0xb0, 0xad, 0x00, 0x02, 0x5e, 0x0a, 0xe8, 0x2a
};

static const unsigned char BASIC_UUID[16] = {
	0xa2, 0x89, 0xcc, 0x33, 0xbc, 0xbb, 0x8b, 0x4f,
	0xb6, 0xb0, 0x13, 0x3e, 0xc2, 0xaa, 0xe6, 0xdf
};

/* MS Basic Connect Extensions (uuid_ms_basic_connect_extensions in
 * libmbim's mbim-uuid.c).  CID 11 = Base Stations Info - the only MBIM command
 * that reports RSRP/RSRQ/SNR and, on MBIMEx 3.0, the NR serving-cell array
 * (i.e. the carriers that would show up as PCC/SCC). */
static const unsigned char MSX_UUID[16] = {
	0x3d, 0x01, 0xdc, 0xc5, 0xfe, 0xf5, 0x4d, 0x05,
	0x0d, 0x3a, 0xbe, 0xf7, 0x05, 0x8e, 0x9a, 0xaf
};

static int mbim_cmd(uint32_t cid, const unsigned char *info, size_t info_len,
		    const char *label)
{
	return mbim_cmd_uuid(SMS_UUID, cid, info, info_len, label);
}

static int sms_send_query(void)
{
	/* format 0 = MBIM_SMS_FORMAT_PDU (libmbim 1.30).  Sending 1 makes the
	 * modem answer 0x15 = INVALID_PARAMETERS on read and 0x68 =
	 * SMS_FORMAT_NOT_SUPPORTED on send - verified 2026-11-05.
	 * The payload is {format, flag, message_index} - 12 bytes; with only 8
	 * bytes the modem answers 0x15 as well.
	 * Only one command may be outstanding at a time: blasting several SMS
	 * commands in a row makes the modem answer all but one with 0x15. */
	static unsigned char read_pdu[12] = { 0 };

	if (!g_probed) {
		g_probed = 1;
		mbim_cmd(1, NULL, 0, "SMS_CFG");
	}
	return mbim_cmd(2, read_pdu, sizeof(read_pdu), "SMS_READ");
}

/* ---------- SMS send (driven by hh500v-atgw over a fifo) ---------- */

#define CTL_FIFO "/tmp/hh500v-mbimprox.ctl"
#define SEND_RESULT "/tmp/hh500v-mbimprox.send"

static int g_ctl = -1;
static char g_ctl_buf[2048];
static size_t g_ctl_used;
static uint32_t g_send_txn;
static int g_send_layout;
static unsigned char g_send_pdu[400];
static size_t g_send_len;

static void send_result(const char *s)
{
	FILE *f = fopen(SEND_RESULT, "w");

	if (!f)
		return;
	fprintf(f, "%s\n", s);
	fclose(f);
	lg("sms send result: %s", s);
}

static int hexval(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static size_t parse_hex(const char *s, unsigned char *out, size_t max)
{
	size_t n = 0;
	int hi = -1;

	while (*s && n < max) {
		int v = hexval(*s);

		s++;
		if (v < 0)
			continue;	/* tolerate spaces, quotes, stray prompts */
		if (hi < 0) {
			hi = v;
			continue;
		}
		out[n++] = (unsigned char)((hi << 4) | v);
		hi = -1;
	}
	return n;
}

/* SMS_SEND payload per data/mbim-service-sms.json (libmbim):
 *   set = [ Format (u32), PduMessage (MbimSmsPduSendRecord{PduData ref-byte-array}),
 *           CdmaMessage (MbimSmsCdmaSendRecord) ]
 * On the wire this box wants the ref-byte-array as {u32 offset, u32 length}
 * followed by the bytes - see g_ladder below. */
/* Order in which the payload layouts are tried.  Layout 4 is the one this
 * modem accepts (verified 2026-11-05): format, offset=8, len, pdu, i.e. the
 * ref-byte-array is serialised as {u32 offset, u32 length} + data, exactly like
 * the SMS_CFG response does for its ScAddress string. */
static const int g_ladder[] = { 4, 0, 1, 2, 3, 5 };

static void sms_send_frame(void)
{
	unsigned char info[512];
	size_t o, ilen, pad;
	int L = g_ladder[g_send_layout];

	switch (L) {
	case 0:		/* format, len, pdu          <- spec layout */
		wr32(info + 0, SMS_FMT_PDU);
		wr32(info + 4, (uint32_t)g_send_len);
		o = 8;
		break;
	case 1:		/* format, len, pdu, 0-padding to 4 bytes */
		wr32(info + 0, SMS_FMT_PDU);
		wr32(info + 4, (uint32_t)g_send_len);
		o = 8;
		break;
	case 2:		/* format, reference, len, pdu */
		wr32(info + 0, SMS_FMT_PDU);
		wr32(info + 4, 0);
		wr32(info + 8, (uint32_t)g_send_len);
		o = 12;
		break;
	case 3:		/* format, len, pdu, smsc_format, smsc_len */
		wr32(info + 0, SMS_FMT_PDU);
		wr32(info + 4, (uint32_t)g_send_len);
		o = 8;
		break;
	case 4:		/* format, offset(=8, struct-relative), len, pdu
		 * This is how libmbim serialises a ref-byte-array inside a
		 * struct: the SMS_CFG response on this box is
		 *   storage,format,max,cdma, 24,28,"+8613010982500"
		 * i.e. {u32 offset, u32 len} then the data. */
		wr32(info + 0, SMS_FMT_PDU);
		wr32(info + 4, 8);
		wr32(info + 8, (uint32_t)g_send_len);
		o = 12;
		break;
	default:	/* format, offset(=12, buffer-relative), len, pdu */
		wr32(info + 0, SMS_FMT_PDU);
		wr32(info + 4, 12);
		wr32(info + 8, (uint32_t)g_send_len);
		o = 12;
		break;
	}
	memcpy(info + o, g_send_pdu, g_send_len);
	o += g_send_len;
	if (L == 1) {
		pad = (4 - (o & 3)) & 3;
		while (pad--)
			info[o++] = 0;
	}
	if (L == 3) {
		wr32(info + o, 1);	/* smsc format = text */
		o += 4;
		wr32(info + o, 0);	/* smsc empty: use the one on the SIM */
		o += 4;
	}
	ilen = o;
	mbim_cmd_full(SMS_UUID, 3, 1, info, ilen, "SMS_SEND");
	g_send_txn = g_our_pending;
	lg("sms send attempt layout=%d pdu_len=%zu txn=0x%x", L, g_send_len,
	   g_send_txn);
}

static void sms_send_done(const unsigned char *f, size_t len)
{
	uint32_t status = len >= 44 ? rd32(f + 40) : 0xffffffffu;

	if (status == 0) {
		send_result("ok");
		g_send_txn = 0;
		return;
	}
	lg("sms send failed layout=%d status=0x%x", g_send_layout, status);
	g_send_txn = 0;
	if (g_send_layout < 5) {		g_send_layout++;
		sms_send_frame();
		return;
	}
	{
		char b[64];
		snprintf(b, sizeof(b), "err 0x%x", status);
		send_result(b);
	}
}

static void ctl_drain(void)
{
	ssize_t r;

	for (;;) {
		if (g_ctl_used >= sizeof(g_ctl_buf) - 1)
			g_ctl_used = 0;
		r = read(g_ctl, g_ctl_buf + g_ctl_used,
			 sizeof(g_ctl_buf) - 1 - g_ctl_used);
		if (r <= 0)
			break;
		g_ctl_used += (size_t)r;
		g_ctl_buf[g_ctl_used] = 0;
	}
	for (;;) {
		char *nl = strchr(g_ctl_buf, '\n');
		size_t used;

		if (!nl)
			break;
		*nl = 0;
		if (!strncmp(g_ctl_buf, "send ", 5)) {
			size_t n = parse_hex(g_ctl_buf + 5, g_send_pdu,
					     sizeof(g_send_pdu));

			if (n > 10) {
				g_send_len = n;
				g_send_layout = 0;
				sms_send_frame();
			} else {
				send_result("err bad-pdu");
			}
		} else if (!strncmp(g_ctl_buf, "raw ", 4)) {
			/* "raw <cid> <cmd_type> <info hex>" - probe a layout once
			 * without rebuilding; the reply lands in the log. */
			char *p = g_ctl_buf + 4;
			unsigned long cid = strtoul(p, &p, 0);
			unsigned long ctype = strtoul(p, &p, 0);
			unsigned char info[400];
			size_t n = parse_hex(p, info, sizeof(info));

			mbim_cmd_full(SMS_UUID, (uint32_t)cid, (uint32_t)ctype,
				      n ? info : NULL, n, "RAW");
			send_result("raw-sent");
		} else if (!strncmp(g_ctl_buf, "msq ", 4)) {
			/* "msq <cid> <info hex>" - query the MS Basic Connect
			 * Extensions service (cid 11 = Base Stations Info, which is
			 * the only place RSRP/RSRQ/SNR and the NR serving-cell list
			 * could come from). */
			char *p = g_ctl_buf + 4;
			unsigned long cid = strtoul(p, &p, 0);
			unsigned char info[400];
			size_t n = parse_hex(p, info, sizeof(info));

			mbim_cmd_full(MSX_UUID, (uint32_t)cid, 0,
				      n ? info : NULL, n, "MSX");
			send_result("msq-sent");
		} else if (!strncmp(g_ctl_buf, "layout ", 7)) {
			g_send_layout = (int)strtoul(g_ctl_buf + 7, NULL, 0);
			lg("send layout set to %d", g_send_layout);
		}
		used = (size_t)(nl - g_ctl_buf) + 1;
		memmove(g_ctl_buf, g_ctl_buf + used, g_ctl_used - used);
		g_ctl_used -= used;
		g_ctl_buf[g_ctl_used] = 0;
	}
}

/* ---------- frame routing ---------- */

static void log_frame(const char *dir, const unsigned char *f, size_t len)
{
	char hx[512];
	size_t n = len < 160 ? len : 160;

	if (!g_verbose || len < 12)
		return;
	hexdump_str(f, n, hx);
	lg("%s type=0x%x txn=%u cid=%u len=%zu %s", dir, rd32(f), rd32(f + 8),
	   (rd32(f) == MT_COMMAND && len >= 40) ? rd32(f + 36) : 0, len, hx);
}

static void handle_client_frame(const unsigned char *f, size_t len)
{
	uint32_t type = rd32(f);
	uint32_t txn = rd32(f + 8);

	log_frame("cli->", f, len);
	if (type == MT_OPEN) {
		g_open_txn = txn;
		if (g_session) {
			/* MBIM_OPEN_DONE carries one field: MaxTransferSize (u32), so the
			 * frame is 20 bytes, not 16.  A 16-byte reply makes libmbim fail
			 * with "message too short" -> umbim reports NO_SUBSCRIBER and
			 * ifup wwan never comes up (seen 2026-11-06 17:07). */
			unsigned char rep[20];

			make_reply(rep, MT_OPEN_DONE, txn, 0);
			wr32(rep + 16, 0x1000);	/* 4096, what the modem uses */
			send_to_client(rep, 20);
			lg("swallowed OPEN txn=%u (session already alive)", txn);
			return;
		}
		g_session = 1;
		g_read_fail = 0;
		/* the SMS subsystem answers 0x15 for the first ~20s after a new
		 * session, so give it a moment before the first poll */
		g_next_poll = time(NULL) + 30;
		g_svc_at = time(NULL) + 5;
		send_to_real(f, len);
		lg("forwarded OPEN txn=%u (session establishing)", txn);
		return;
	}
	if (type == MT_CLOSE) {
		if (g_session) {
			unsigned char rep[16];
			make_reply(rep, MT_CLOSE_DONE, txn, 0);
			send_to_client(rep, 16);
			lg("swallowed CLOSE txn=%u (keeping session alive)", txn);
			return;
		}
		send_to_real(f, len);
		return;
	}
	send_to_real(f, len);
}

/* ---------- MBIM service discovery (for the signal/CA hunt) ----------
 *
 * DEVICE_SERVICES (UUID a289cc0d-..., cid 2) is asked by umbim right after OPEN
 * and its reply lists every service the firmware implements.  We keep it in
 * /tmp/hh500v-services.txt so we know whether extension services (MS Basic
 * Connect Extensions, Intel, ...) exist before writing code against them.
 * Layout: u32 ServiceCount, u32 MaxDssSessions, then ServiceCount x 16-byte UUID.
 */
#define SERVICES_FILE "/tmp/hh500v-services.txt"
static int g_svc_done;

static void dump_services(const unsigned char *f, size_t len)
{
	uint32_t cid, st, il;
	FILE *fp;
	size_t i;

	if (g_svc_done || len < 48)
		return;
	if (memcmp(f + 20, DEV_UUID, 16) != 0)
		return;
	cid = rd32(f + 36);
	if (cid != 2)
		return;
	st = rd32(f + 40);
	il = rd32(f + 44);
	if (st != 0 || il < 8 || il > len - 48)
		return;
	fp = fopen(SERVICES_FILE, "w");
	if (!fp)
		return;
	fprintf(fp, "# status=%u info_len=%u count=%u\n", st, il, rd32(f + 48));
	for (i = 0; i + 16 <= il - 8; i += 16) {
		const unsigned char *u = f + 56 + i;
		fprintf(fp, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
			"%02x%02x%02x%02x%02x%02x\n",
			u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7],
			u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
	}
	fclose(fp);
	g_svc_done = 1;
	lg("device services dumped to %s (%u services)", SERVICES_FILE,
	   rd32(f + 48));
}

static void handle_real_frame(const unsigned char *f, size_t len)
{
	uint32_t type = rd32(f);
	uint32_t txn = rd32(f + 8);

	if (type == MT_COMMAND_DONE && len >= 48)
		dump_services(f, len);

	if (type == MT_INDICATE && len >= 44) {
		uint32_t cid = rd32(f + 36);
		uint32_t ilen = rd32(f + 40);
		char hx[400];

		hexdump_str(f + 20, 16, hx);
		if (len > 44 && ilen > 0 && ilen <= len - 44) {
			char px[400];
			hexdump_str(f + 44, ilen < 160 ? ilen : 160, px);
			lg("indication svc=%s cid=%u info_len=%u %s", hx, cid, ilen, px);
		} else {
			lg("indication svc=%s cid=%u info_len=%u (empty)", hx, cid, ilen);
		}
		if (memcmp(f + 20, SMS_UUID, 16) == 0 && cid == 2)
			sms_handle_indication(f, len);
	}
	if (type == MT_COMMAND_DONE && (txn & 0xF0000000u) == OUR_TXN_BASE) {
		/* A fragmented reply is useless to us: the modem either repeats the
		 * full header (single frame) or sends continuation frames whose
		 * payload starts right after the 20-byte header.  Parsing such a
		 * continuation as a header yields garbage (verified: cid=41,
		 * status=0x31689108 ...), so drop them instead of mis-reading. */
		if (len >= 20 && rd32(f + 12) > 1) {
			/* Fragmented reply: 20-byte header (type, len, txn, total,
			 * cur) then raw payload.  Fragment 0 starts with the
			 * service uuid, cid, status and info_len, the rest is
			 * continuation of the info buffer.  Verified 2026-11-05:
			 * 6 fragments x 492 bytes = the whole SIM store. */
			uint32_t total = rd32(f + 12), cur = rd32(f + 16);
			size_t chunk = len - 20;

			if (txn == g_our_pending)
				g_our_pending = 0;
			if (cur == 0 || txn != g_frag_txn || total != g_frag_total ||
			    cur != g_frag_next) {
				g_frag_len = 0;
				g_frag_txn = txn;
				g_frag_total = total;
				g_frag_next = 0;
			}
			if (cur == 0 && g_frag_len == 0 && chunk + 28 < FRAG_MAX) {
				memcpy(g_frag + g_frag_len, f + 20, chunk);
				g_frag_len += chunk;
				g_frag_next = 1;
				if (total == 1)
					goto frag_done;
				return;
			}
			if (txn == g_frag_txn && cur == g_frag_next &&
			    g_frag_len + chunk < FRAG_MAX) {
				memcpy(g_frag + g_frag_len, f + 20, chunk);
				g_frag_len += chunk;
				g_frag_next++;
				if (cur + 1 < total)
					return;
			} else {
				lg("frag gap txn=0x%x got=%u want=%u", txn, cur,
				   g_frag_next);
				g_frag_len = 0;
				g_frag_next = 0;
				return;
			}
frag_done:
			if (g_frag_len > 28 && rd32(g_frag + 16) == 2) {
				uint32_t st = rd32(g_frag + 20);
				uint32_t il = rd32(g_frag + 24);

				if (st == 0 && il <= g_frag_len - 28) {
					lg("assembled SMS_READ frags=%u info_len=%u",
					   total, il);
					g_read_fail = 0;
					g_next_poll = time(NULL) + g_interval;
					sms_parse_records(g_frag + 28, il);
				} else {
					lg("assembled SMS_READ status=0x%x info_len=%u",
					   st, il);
				}
			}
			g_frag_len = 0;
			g_frag_next = 0;
			return;
		}
		if (g_send_txn && txn == g_send_txn) {
			if (txn == g_our_pending)
				g_our_pending = 0;
			sms_send_done(f, len);
			return;
		}
		if (txn == g_our_pending)
			g_our_pending = 0;
		sms_handle_done(f, len);
		return;
	}
	if (type == MT_CLOSE_DONE && g_session) {
		/* umbim's CLOSE was swallowed, so this should not arrive; ignore it */
		lg("dropping unexpected CLOSE_DONE from modem");
		return;
	}
	log_frame("real->", f, len);
	send_to_client(f, len);
}

/* ---------- pty setup ---------- */

static int setup_pty(void)
{
	struct termios tio;
	char *name;

	g_master = posix_openpt(O_RDWR | O_NOCTTY);
	if (g_master < 0) {
		lg("posix_openpt: %s", strerror(errno));
		return -1;
	}
	if (grantpt(g_master) < 0 || unlockpt(g_master) < 0) {
		lg("grantpt/unlockpt: %s", strerror(errno));
		return -1;
	}
	name = ptsname(g_master);
	if (!name) {
		lg("ptsname failed");
		return -1;
	}
	snprintf(g_ptsname, sizeof(g_ptsname), "%s", name);

	g_slave = open(g_ptsname, O_RDWR | O_NOCTTY);
	if (g_slave < 0) {
		lg("open slave %s: %s", g_ptsname, strerror(errno));
		return -1;
	}
	if (tcgetattr(g_slave, &tio) == 0) {
		cfmakeraw(&tio);
		tio.c_cc[VMIN] = 1;
		tio.c_cc[VTIME] = 0;
		tcsetattr(g_slave, TCSANOW, &tio);
	}
	lg("pty %s ready", g_ptsname);
	return 0;
}

static void on_signal(int sig)
{
	(void)sig;
	lg("signal, cleaning up");
	if (g_bound)
		umount2(g_dev, MNT_DETACH);
	unlink(g_pts);
	_exit(0);
}

int main(int argc, char **argv)
{
	int c, real_flags;
	sbuf_t from_client = { .len = 0 }, from_real = { .len = 0 };

	while ((c = getopt(argc, argv, "d:p:i:vh")) != -1) {
		switch (c) {
		case 'd': g_dev = optarg; break;
		case 'p': g_pts = optarg; break;
		case 'i': g_interval = atoi(optarg); break;
		case 'v': g_verbose = 1; break;
		default:
			fprintf(stderr, "usage: %s [-d device] [-p ptslink] [-i poll_seconds] [-v]\n", argv[0]);
			return 2;
		}
	}

	g_log = fopen("/tmp/hh500v-mbimprox.log", "a");
	lg("=== start v3 (dev=%s interval=%ds) ===", g_dev, g_interval);
	sms_load_hashes();
	/* ask for the service list 8s in, whether or not we see a fresh OPEN
	 * (umbim's CLOSE is swallowed, so after a restart the session is already
	 * alive and the OPEN branch below is never taken) */
	g_svc_at = time(NULL) + 8;

	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	signal(SIGPIPE, SIG_IGN);

	g_real = open(g_dev, O_RDWR | O_NOCTTY);
	if (g_real < 0) {
		lg("open %s: %s", g_dev, strerror(errno));
		return 1;
	}
	real_flags = fcntl(g_real, F_GETFL, 0);
	fcntl(g_real, F_SETFL, real_flags | O_NONBLOCK);

	if (setup_pty() < 0)
		return 1;

	unlink(g_pts);
	if (symlink(g_ptsname, g_pts) < 0)
		lg("symlink %s -> %s: %s", g_pts, g_ptsname, strerror(errno));

	/* hide the real device behind our pty; our fd above stays valid */
	if (mount(g_ptsname, g_dev, NULL, MS_BIND, NULL) < 0) {
		lg("bind mount failed: %s", strerror(errno));
		return 1;
	}
	g_bound = 1;
	lg("bind-mounted %s over %s", g_ptsname, g_dev);

	g_next_poll = time(NULL) + 5;

	/* hh500v-atgw hands outbound PDUs to us through this fifo */
	mkfifo(CTL_FIFO, 0666);
	g_ctl = open(CTL_FIFO, O_RDWR | O_NONBLOCK);
	if (g_ctl < 0)
		lg("open %s: %s", CTL_FIFO, strerror(errno));
	else
		g_ctl_used = 0;

	for (;;) {
		struct pollfd pfd[3];
		int n;
		time_t now;

		pfd[0].fd = g_master; pfd[0].events = POLLIN;
		pfd[1].fd = g_real;   pfd[1].events = POLLIN;
		pfd[2].fd = g_ctl;    pfd[2].events = POLLIN;
		n = poll(pfd, 3, 1000);

		if (n > 0) {
			if (pfd[2].revents & POLLIN)
				ctl_drain();
			if (pfd[0].revents & POLLIN) {
				unsigned char tmp[4096];
				ssize_t r = read(g_master, tmp, sizeof(tmp));
				if (r > 0) sbuf_append(&from_client, tmp, (size_t)r);
				else if (r < 0 && errno != EAGAIN && errno != EINTR)
					lg("read pty: %s", strerror(errno));
			}
			if (pfd[1].revents & POLLIN) {
				unsigned char tmp[8192];
				ssize_t r = read(g_real, tmp, sizeof(tmp));
				if (r > 0) sbuf_append(&from_real, tmp, (size_t)r);
				else if (r < 0 && errno != EAGAIN && errno != EINTR)
					lg("read real: %s", strerror(errno));
			}
		}

		/* drain complete frames from the client side */
		while (from_client.len >= 12) {
			uint32_t flen = rd32(from_client.b + 4);
			if (flen < 12 || flen > sizeof(from_client.b)) {
				sbuf_drop(&from_client, 1);
				continue;
			}
			if (from_client.len < flen)
				break;
			handle_client_frame(from_client.b, flen);
			sbuf_drop(&from_client, flen);
		}
		while (from_real.len >= 12) {
			uint32_t flen = rd32(from_real.b + 4);
			if (flen < 12 || flen > sizeof(from_real.b)) {
				sbuf_drop(&from_real, 1);
				continue;
			}
			if (from_real.len < flen)
				break;
			handle_real_frame(from_real.b, flen);
			sbuf_drop(&from_real, flen);
		}

		now = time(NULL);
		/* One-shot: ask the modem which MBIM services it implements.  umbim
		 * does not send DEVICE_SERVICES itself on this firmware, so we have to
		 * do it after the session is up; the reply is written to
		 * /tmp/hh500v-services.txt by dump_services(). */
		if (!g_svc_done && !g_svc_sent && now >= g_svc_at) {
			g_svc_sent = 1;
			mbim_cmd_full(DEV_UUID, 2, 0, NULL, 0, "DEVICE_SERVICES");
		}
		if (g_session && !g_our_pending && now >= g_next_poll) {
			sms_send_query();
			g_next_poll = now + g_interval;
		}
	}
	return 0;
}
