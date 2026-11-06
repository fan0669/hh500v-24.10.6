/*
 * hh500v-atgw - tiny AT gateway for the TCL LINKHUB HH500V
 *
 * The GM800 stops answering on its USB serial ports (/dev/ttyUSBx) as soon
 * as a data session is up, which makes every AT based SMS front end (the
 * luci-app-sms-tool-js page, QModem's AT helpers, modemband, 3ginfo) show
 * nothing.  The MBIM proxy in this repository captures the real SMS PDUs
 * into /etc/hh500v-sms.txt; this little daemon exports that store through a
 * pty that is bind mounted over /dev/ttyUSB2, so the stock tools simply keep
 * working while the link stays up.
 *
 * Implemented commands: AT, ATE, ATI, AT&F, ATZ, ATV, ATX, AT&C, AT&D, AT&S,
 * AT+CPIN?, AT+CSQ, AT+CREG?/AT+CGREG?/AT+CEREG?, AT+COPS?, AT+CGSN, AT+CGMI,
 * AT+CGMM, AT+CPMS, AT+CMGF, AT+CSCS, AT+CNMI, AT+CMEE, AT+CFUN, AT+CMGL,
 * AT+CMGR, AT+CMGD, AT+CSMS.  Everything else answers ERROR quickly so that
 * callers never block.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <poll.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define SMS_DBED "/etc/hh500v-sms.txt"
#define IMEI_FILE "/etc/hh500v-imei"
#define MODEM_ID "/etc/hh500v-modem-id"
#define STATE_FILE "/etc/hh500v-modem-cfg"
#define BANDS_FILE "/etc/hh500v-modem-cfg"
/* raw AT+ZCELLINFO? reply saved by hh500v-sig-cache.sh: one line per camped
 * cell, which is the only carrier-aggregation visibility this firmware has */
#define CELLS_FILE "/etc/hh500v-cells.txt"
#define LOGFILE "/tmp/hh500v-atgw.log"
#define CTL_FIFO "/tmp/hh500v-mbimprox.ctl"
#define SEND_RESULT "/tmp/hh500v-mbimprox.send"
#define MAX_LINKS 4

/* one AT port we impersonate */
struct link {
	char dev[128];
	char pts[128];
	int master;
	int slave;
	char buf[4096];
	size_t used;
	long last_ms;	/* last command we answered, to flush stale replies */
	int cmgs;	/* 1 = next line is a PDU for AT+CMGS */
};

static struct link *g_cur;	/* link being serviced right now */

static struct link g_links[MAX_LINKS];
static int g_nlinks;
static int g_verbose;
static int g_no_ok;	/* chain mode: swallow the OK of a mid-chain command */
static int g_cops_fmt = -1;	/* AT+COPS=3,<n> selects the COPS? reply format */

static void lg(const char *fmt, ...)
{
	va_list ap;
	FILE *f;
	time_t t = time(NULL);
	struct tm tm;

	localtime_r(&t, &tm);
	f = fopen(LOGFILE, "a");
	if (!f)
		return;
	fprintf(f, "%02d:%02d:%02d ", tm.tm_hour, tm.tm_min, tm.tm_sec);
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fputc('\n', f);
	fclose(f);
}

/* ------------------------------------------------------------------ store */

struct sms {
	unsigned long epoch;
	unsigned idx;
	char sender[64];
	char text[1024];
};

static int sms_load(struct sms *out, int max)
{
	FILE *f = fopen(SMS_DBED, "r");
	char line[2048];
	int n = 0;

	if (!f)
		return 0;
	while (n < max && fgets(line, sizeof(line), f)) {
		char *p1, *p2, *p3, *p4;
		line[strcspn(line, "\r\n")] = 0;
		if (!line[0])
			continue;
		p1 = strchr(line, '|');
		if (!p1)
			continue;
		*p1++ = 0;
		p2 = strchr(p1, '|');
		if (!p2)
			continue;
		*p2++ = 0;
		p3 = strchr(p2, '|');
		if (!p3)
			continue;
		*p3++ = 0;
		p4 = strchr(p3, '|');
		if (!p4)
			continue;
		*p4++ = 0;
		out[n].epoch = strtoul(line, NULL, 10);
		out[n].idx = (unsigned)strtoul(p1, NULL, 10);
		snprintf(out[n].sender, sizeof(out[n].sender), "%s", p2);
		snprintf(out[n].text, sizeof(out[n].text), "%s", p4);
		n++;
	}
	fclose(f);
	return n;
}

static int sms_delete(int which)
{
	FILE *in = fopen(SMS_DBED, "r"), *out;
	char line[2048], tmp[] = SMS_DBED ".new";
	int n = 0, rc = -1;

	if (!in)
		return -1;
	out = fopen(tmp, "w");
	if (!out) {
		fclose(in);
		return -1;
	}
	while (fgets(line, sizeof(line), in)) {
		n++;
		if (n == which) {
			rc = 0;
			continue;
		}
		fputs(line, out);
	}
	fclose(in);
	fclose(out);
	if (rc == 0)
		rename(tmp, SMS_DBED);
	else
		unlink(tmp);
	return rc;
}

/* ------------------------------------------------------------- pdu build */

static int utf8_to_ucs2(const char *s, unsigned short *out, int max)
{
	int n = 0;
	const unsigned char *p = (const unsigned char *)s;

	while (*p && n < max) {
		unsigned cp;
		if (p[0] < 0x80) {
			cp = p[0];
			p += 1;
		} else if ((p[0] & 0xe0) == 0xc0 && p[1]) {
			cp = ((p[0] & 0x1fu) << 6) | (p[1] & 0x3fu);
			p += 2;
		} else if ((p[0] & 0xf0) == 0xe0 && p[1] && p[2]) {
			cp = ((p[0] & 0x0fu) << 12) | ((p[1] & 0x3fu) << 6) |
			     (p[2] & 0x3fu);
			p += 3;
		} else if ((p[0] & 0xf8) == 0xf0 && p[1] && p[2] && p[3]) {
			cp = 0xfffd;
			p += 4;
		} else {
			cp = 0xfffd;
			p += 1;
		}
		out[n++] = (unsigned short)cp;
	}
	return n;
}

static int bcd_swap(int v)
{
	return ((v % 10) << 4) | (v / 10);
}

static size_t build_pdu(const struct sms *m, unsigned char *buf, size_t bufsz)
{
	unsigned short ucs[600];
	int ncu = utf8_to_ucs2(m->text, ucs, 600);
	size_t o = 0;
	const char *s = m->sender;
	int digits = 0, i, half;
	time_t t = (time_t)m->epoch;
	struct tm tm;

	localtime_r(&t, &tm);
	memset(buf, 0, bufsz);

	buf[o++] = 0x00;              /* SMSC: use default */
	buf[o++] = 0x04;              /* SMS-DELIVER, no more messages */

	if (*s == '+')
		s += 1;
	digits = (int)strlen(s);
	half = (digits + 1) / 2;
	buf[o++] = (unsigned char)digits;
	buf[o++] = (*m->sender == '+') ? 0x91 : 0x81;
	for (i = 0; i < half; i++) {
		int lo = (2 * i < digits) ? s[2 * i] - '0' : 0x0f;
		int hi = (2 * i + 1 < digits) ? s[2 * i + 1] - '0' : 0x0f;
		/* GSM 03.40: the *first* digit lives in the low nibble */
		buf[o++] = (unsigned char)((lo & 0x0f) | ((hi & 0x0f) << 4));
	}

	buf[o++] = 0x00;              /* PID */
	buf[o++] = 0x08;              /* DCS: UCS2 */
	buf[o++] = (unsigned char)bcd_swap(tm.tm_year % 100);
	buf[o++] = (unsigned char)bcd_swap(tm.tm_mon + 1);
	buf[o++] = (unsigned char)bcd_swap(tm.tm_mday);
	buf[o++] = (unsigned char)bcd_swap(tm.tm_hour);
	buf[o++] = (unsigned char)bcd_swap(tm.tm_min);
	buf[o++] = (unsigned char)bcd_swap(tm.tm_sec);
	buf[o++] = 0x00;              /* timezone octet: 0 -> clients show the stored local time */

	buf[o++] = (unsigned char)(ncu * 2);
	for (i = 0; i < ncu; i++) {
		buf[o++] = (unsigned char)(ucs[i] >> 8);
		buf[o++] = (unsigned char)(ucs[i] & 0xff);
	}
	return o;
}

static void hex_str(const unsigned char *d, size_t n, char *out)
{
	static const char hx[] = "0123456789ABCDEF";
	size_t i;

	for (i = 0; i < n; i++) {
		out[2 * i] = hx[d[i] >> 4];
		out[2 * i + 1] = hx[d[i] & 0x0f];
	}
	out[2 * n] = 0;
}

/* --------------------------------------------------------------- answers */

static void reply(int fd, const char *s)
{
	if (write(fd, s, strlen(s)) < 0)
		lg("write: %s", strerror(errno));
}

static void reply_ok(int fd)
{
	if (g_no_ok)
		return;
	reply(fd, "\r\nOK\r\n");
}

static void reply_err(int fd)
{
	reply(fd, "\r\nERROR\r\n");
}

/* key=value file written by hh500v-at-port.sh during the boot AT window */
static void id_lookup(const char *key, char *out, size_t outsz)
{
	FILE *f = fopen(MODEM_ID, "r");
	char line[256];
	size_t kl = strlen(key);

	out[0] = 0;
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (strncmp(line, key, kl) == 0 && line[kl] == '=') {
			snprintf(out, outsz, "%s", line + kl + 1);
			break;
		}
	}
	fclose(f);
}

/*
 * Generic key=value store for the vendor state the module cannot answer itself
 * any more (dial mode, network preference, locked bands, chosen IMEI).  The
 * GM800 serves AT only before the data session starts, so these values have to
 * live here: read back what was last set instead of reporting ERROR forever.
 */
static int kv_get(const char *path, const char *key, char *out, size_t outsz)
{
	FILE *f = fopen(path, "r");
	char line[256];
	size_t kl = strlen(key);

	out[0] = 0;
	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (strncmp(line, key, kl) == 0 && line[kl] == '=') {
			snprintf(out, outsz, "%s", line + kl + 1);
			break;
		}
	}
	fclose(f);
	return out[0] != 0;
}

static void kv_set(const char *path, const char *key, const char *val)
{
	char tmp[768] = { 0 };
	char line[256];
	char newl[320];
	int done = 0;
	size_t kl = strlen(key);
	FILE *f = fopen(path, "r");
	FILE *o;

	if (f) {
		while (fgets(line, sizeof(line), f)) {
			line[strcspn(line, "\r\n")] = 0;
			if (!strncmp(line, key, kl) && line[kl] == '=') {
				snprintf(newl, sizeof(newl), "%s=%s\n", key, val);
				done = 1;
			} else {
				snprintf(newl, sizeof(newl), "%s\n", line);
			}
			strncat(tmp, newl, sizeof(tmp) - strlen(tmp) - 1);
		}
		fclose(f);
	}
	if (!done) {
		snprintf(newl, sizeof(newl), "%s=%s\n", key, val);
		strncat(tmp, newl, sizeof(tmp) - strlen(tmp) - 1);
	}
	o = fopen(path, "w");
	if (!o) {
		lg("kv_set %s: %s", path, strerror(errno));
		return;
	}
	fputs(tmp, o);
	fclose(o);
}

static int cmd_is(const char *line, const char *name)
{
	size_t n = strlen(name);

	return strncasecmp(line, name, n) == 0 &&
	       (line[n] == 0 || line[n] == '=' || line[n] == '?');
}

static int starts(const char *line, const char *name)
{
	return strncasecmp(line, name, strlen(name)) == 0;
}

/*
 * Hand an outbound PDU to hh500v-mbimprox (it owns the live MBIM session) and
 * wait for the verdict.  The wait blocks this process, which is fine: sending
 * a message is rare and the modem answers in a second or two.
 */
static int send_pdu_via_proxy(const char *hex, char *res, size_t ressz)
{
	char line[1024];
	time_t deadline;
	int fd;

	res[0] = 0;
	fd = open(CTL_FIFO, O_WRONLY | O_NONBLOCK);
	if (fd < 0) {
		snprintf(res, ressz, "err no-proxy");
		return -1;
	}
	unlink(SEND_RESULT);
	snprintf(line, sizeof(line), "send %s\n", hex);
	if (write(fd, line, strlen(line)) < 0) {
		close(fd);
		snprintf(res, ressz, "err fifo");
		return -1;
	}
	close(fd);

	deadline = time(NULL) + 30;
	while (time(NULL) < deadline) {
		FILE *f = fopen(SEND_RESULT, "r");

		if (f) {
			if (fgets(res, (int)ressz, f)) {
				fclose(f);
				res[strcspn(res, "\r\n")] = 0;
				return strncmp(res, "ok", 2) == 0 ? 0 : -1;
			}
			fclose(f);
		}
		usleep(200 * 1000);
	}
	snprintf(res, ressz, "err timeout");
	return -1;
}

/*
 * IPv4 of the data interface.  QModem's overview page decides "Connect Status"
 * from AT+CGACT? -> for every active context AT+CGPADDR=<cid>, so we have to
 * answer both with the address the kernel really got from the network.
 */
static int wwan_ipv4(char *out, size_t outsz)
{
	struct ifaddrs *ifa, *p;

	out[0] = 0;
	if (getifaddrs(&ifa) != 0)
		return -1;
	for (p = ifa; p; p = p->ifa_next) {
		struct sockaddr_in *sin;

		if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET)
			continue;
		if (strncmp(p->ifa_name, "wwan", 4) != 0)
			continue;
		sin = (struct sockaddr_in *)p->ifa_addr;
		if (inet_ntop(AF_INET, &sin->sin_addr, out, (socklen_t)outsz) == NULL)
			out[0] = 0;
		break;
	}
	freeifaddrs(ifa);
	return out[0] ? 0 : -1;
}

static void handle_single(int fd, char *line)
{
	struct sms list[100];
	int n, i;

	line[strcspn(line, "\r\n")] = 0;
	while (*line == ' ' || *line == '\t')
		line++;
	if (g_verbose)
		lg("cmd: %s", line);

	/* second half of AT+CMGS: this line is the PDU in hex */
	if (g_cur && g_cur->cmgs) {
		char res[64];
		size_t i, hx = 0;
		int bad = 0;

		for (i = 0; line[i]; i++) {
			int c = (unsigned char)line[i];

			if (isxdigit(c)) {
				hx++;
				continue;
			}
			if (c == ' ' || c == ':') {
				continue;
			}
			bad = 1;
			break;
		}
		/* Only take this line as the PDU when it really looks like one.
		 * Some clients send the next command before/without the PDU; in
		 * that case swallowing it would desync the whole channel. */
		if (bad || strncasecmp(line, "AT", 2) == 0 || hx < 20) {
			g_cur->cmgs = 0;
			lg("CMGS: no PDU on this line ('%.20s'), handling as command",
			   line);
		} else {
			g_cur->cmgs = 0;
			lg("CMGS pdu len=%zu head=%.24s tail=%.8s", strlen(line), line,
			   strlen(line) > 8 ? line + strlen(line) - 8 : line);
			if (send_pdu_via_proxy(line, res, sizeof(res)) == 0 &&
			    strncmp(res, "ok", 2) == 0) {
				reply(fd, "\r\n+CMGS: 12\r\n");
				reply_ok(fd);
			} else {
				lg("CMGS failed: %s", res);
				reply(fd, "\r\n+CMS ERROR: 500\r\n");
			}
			return;
		}
	}

	if (strncasecmp(line, "AT", 2) != 0) {
		reply_err(fd);
		return;
	}

	if (line[2] == 0 || !strcasecmp(line, "AT")) {
		reply_ok(fd);
		return;
	}

	if (cmd_is(line, "ATI")) {	/* the real firmware prints "IMEI: " empty */
		char im[64] = { 0 };
		FILE *fi = fopen(IMEI_FILE, "r");
		reply(fd, "\r\nTCL LINKHUB HH500V\r\nGM800\r\nRevision: HH500V-ATGW\r\n");
		if (fi) {
			if (!fgets(im, sizeof(im), fi))
				im[0] = 0;
			fclose(fi);
			im[strcspn(im, "\r\n")] = 0;
		}
		if (im[0]) {
			reply(fd, "IMEI: ");
			reply(fd, im);
			reply(fd, "\r\n");
		}
		reply_ok(fd);
		return;
	}
	if (starts(line, "AT+CFUN=1,1") || starts(line, "AT+CFUN=1, 1") ||
	    starts(line, "AT+CFUN=1,1,1")) {
		/* QModem's "重启调制解调器 -> soft" path. This module cannot be
		 * reset over AT (the real firmware answers "memory failure" and
		 * wedges the port), but the usual reason to press that button is
		 * a stuck PDP context, and a fresh dial fixes that. */
		lg("soft reboot requested -> re-dialing wwan");
		reply_ok(fd);
		if (fork() == 0) {
			execl("/bin/sh", "sh", "-c",
			      "sleep 2; ifdown wwan; sleep 5; ifup wwan; "
			      "logger -t hh500v-atgw 'soft reboot: wwan re-dialed'",
			      (char *)NULL);
			_exit(127);
		}
		return;
	}
	if (starts(line, "ATE") || starts(line, "ATV") || starts(line, "ATX") ||
	    starts(line, "AT&C") || starts(line, "AT&D") || starts(line, "AT&S") ||
	    starts(line, "AT&F") || starts(line, "ATZ") || starts(line, "ATQ") ||
	    starts(line, "ATS0") || starts(line, "AT+CMEE") ||
	    starts(line, "AT+CFUN") || starts(line, "AT+CREG=") ||
	    starts(line, "AT+CGREG=") || starts(line, "AT+CEREG=") ||
	    starts(line, "AT+CNMI=") || starts(line, "AT+CTZU") ||
	    starts(line, "AT+CMGF=") || starts(line, "AT+CSCS=") ||
	    starts(line, "AT+CPMS=") || starts(line, "AT+CGEREP")) {
		reply_ok(fd);
		return;
	}
	if (cmd_is(line, "AT+CPIN?")) {
		reply(fd, "\r\n+CPIN: READY\r\n");
		reply_ok(fd);
		return;
	}
	if (cmd_is(line, "AT+CSQ")) {
		char v[16] = { 0 };
		char b[48];

		kv_get(STATE_FILE, "csq", v, sizeof(v));
		snprintf(b, sizeof(b), "\r\n+CSQ: %s,99\r\n", v[0] ? v : "20");
		reply(fd, b);
		reply_ok(fd);
		return;
	}
	/* QModem's "Connect Status" (generic.sh:get_connect_status) walks every
	 * context reported active by AT+CGACT? and then asks AT+CGPADDR=<cid>
	 * for its address.  Answering ERROR here is what made the overview page
	 * say "No" for a link that was perfectly up. */
	if (starts(line, "AT+CGACT?")) {
		char ip[64], b[48];

		snprintf(b, sizeof(b), "\r\n+CGACT: 1,%d\r\n",
			 wwan_ipv4(ip, sizeof(ip)) == 0 ? 1 : 0);
		reply(fd, b);
		reply_ok(fd);
		return;
	}
	if (starts(line, "AT+CGACT=")) {
		reply_ok(fd);
		return;
	}
	if (starts(line, "AT+CGPADDR")) {
		char ip[64], b[96];

		if (wwan_ipv4(ip, sizeof(ip)) == 0)
			snprintf(b, sizeof(b), "\r\n+CGPADDR: 1,\"%s\"\r\n", ip);
		else
			snprintf(b, sizeof(b), "\r\n+CGPADDR: 1,\"0.0.0.0\"\r\n");
		reply(fd, b);
		reply_ok(fd);
		return;
	}
	if (starts(line, "AT+CGDCONT?")) {
		char apn[64] = { 0 }, b[128];

		kv_get(STATE_FILE, "apn", apn, sizeof(apn));
		snprintf(b, sizeof(b), "\r\n+CGDCONT: 1,\"IP\",\"%s\",\"0.0.0.0\",0,0\r\n",
			 apn[0] ? apn : "3gnet");
		reply(fd, b);
		reply_ok(fd);
		return;
	}
	if (cmd_is(line, "AT+CREG?") || cmd_is(line, "AT+CGREG?") ||
	    cmd_is(line, "AT+CEREG?") ||
	    /* 3ginfo also asks without the '?': OTX=$(... at "at+cereg").  Without
	     * this the TAC never shows up and the page prints "-" for it. */
	    cmd_is(line, "AT+CREG") || cmd_is(line, "AT+CGREG") ||
	    cmd_is(line, "AT+CEREG")) {
		/* The client may send these in lower case ("at+cereg"), and the
		 * dispatch above is case-insensitive while strstr() is not - so pick
		 * the answer name from an upper-cased copy, otherwise a CEREG query
		 * would be answered with the CREG form and the TAC would be lost. */
		char upl[64];
		size_t ui;
		for (ui = 0; ui + 1 < sizeof(upl) && line[ui]; ui++)
			upl[ui] = (char)toupper((unsigned char)line[ui]);
		upl[ui] = '\0';
		const char *n = strstr(upl, "CGREG") ? "+CGREG" :
				strstr(upl, "CEREG") ? "+CEREG" : "+CREG";
		char b[128], actv[16] = { 0 }, regv[16] = { 0 }, lacv[24] = { 0 };
		char tacu[24] = { 0 };

		/* A real modem with "<n>=2" answers stat,lac,ci,act; the 3ginfo
		 * parser wants the hex fields quoted. "act" is what decides whether
		 * the page says LTE or 5G, and it is the one value we cannot fake:
		 * it comes from the boot AT window (hh500v-sig-cache.sh), where the
		 * module still answers AT+ZPAS? / AT+C5GREG?. Default 13 (NR5G) is
		 * right for this box: it attaches on NR SA whenever coverage exists.
		 */
		kv_get(STATE_FILE, "act", actv, sizeof(actv));
		kv_get(STATE_FILE, "reg", regv, sizeof(regv));
		kv_get(STATE_FILE, "lac", lacv, sizeof(lacv));
		kv_get(STATE_FILE, "tac", tacu, sizeof(tacu));
		if (strchr(line, '?'))
			snprintf(b, sizeof(b),
				 "\r\n%s: 2,%s,\"%s\",\"1A2B3C4D\",%s\r\n", n,
				 regv[0] ? regv : "1",
				 lacv[0] ? lacv : "1A2B",
				 actv[0] ? actv : "13");
		else if (strstr(n, "CEREG"))	/* TAC is the third field here */
			snprintf(b, sizeof(b), "\r\n%s: 2,%s,\"%s\",\"1A2B3C4D\",%s\r\n",
				 n, regv[0] ? regv : "1",
				 tacu[0] ? tacu : (lacv[0] ? lacv : "1A2B"),
				 actv[0] ? actv : "13");
		else
			snprintf(b, sizeof(b), "\r\n%s: 2,%s\r\n", n,
				 regv[0] ? regv : "1");
		reply(fd, b);
		reply_ok(fd);
		return;
	}
	if (cmd_is(line, "AT+CESQ")) {
		/* <- AT+CESQ is where RSRP/RSRQ live, and 3ginfo shows them as "-"
		 * unless we answer. Wire format: rxlev,ber,rscp,ecno,rsrq,rsrp with
		 * rsrq = 2*rsrq_dB + 39 (0..34) and rsrp = rsrp_dBm + 140 (0..97);
		 * 99/255 mean "not known". Values come from the boot AT window. */
		char rq[16] = { 0 }, rp[16] = { 0 };
		char b[96];
		int rq_raw = 255, rp_raw = 255;

		kv_get(STATE_FILE, "rsrq", rq, sizeof(rq));
		kv_get(STATE_FILE, "rsrp", rp, sizeof(rp));
		if (rq[0]) {
			rq_raw = 2 * atoi(rq) + 39;
			if (rq_raw < 0 || rq_raw > 34)
				rq_raw = 255;
		}
		if (rp[0]) {
			rp_raw = atoi(rp) + 140;
			if (rp_raw < 0 || rp_raw > 97)
				rp_raw = 255;
		}
		snprintf(b, sizeof(b), "\r\n+CESQ: 99,99,255,255,%d,%d\r\n",
			 rq_raw, rp_raw);
		reply(fd, b);
		reply_ok(fd);
		return;
	}
	if (cmd_is(line, "AT+COPS?")) {
		/* act again: g_cops_fmt selects the operator-name vs numeric form,
		 * the trailing number is the AcT (3ginfo keys "5G/LTE/3G" off it). */
		char actv[16] = { 0 };

		kv_get(STATE_FILE, "act", actv, sizeof(actv));
		if (!actv[0])
			strcpy(actv, "13");
		if (g_cops_fmt == 2) {
			reply(fd, "\r\n+COPS: 0,2,\"46001\",");
			reply(fd, actv);
			reply(fd, "\r\n");
		} else {
			reply(fd, "\r\n+COPS: 0,0,\"CHN-UNICOM\",");
			reply(fd, actv);
			reply(fd, "\r\n");
		}
		reply_ok(fd);
		return;
	}
	if (starts(line, "AT+COPS=")) {
		/* AT+COPS=3,<fmt> remembers the format for the following query */
		if (strncmp(line, "AT+COPS=3,", 10) == 0)
			g_cops_fmt = atoi(line + 10);
		reply_ok(fd);
		return;
	}
	if (cmd_is(line, "AT+CGSN") || cmd_is(line, "AT+GSN")) {
		char b[64] = { 0 };
		FILE *f = fopen(IMEI_FILE, "r");
		if (f) {
			if (!fgets(b, sizeof(b), f))
				b[0] = 0;
			fclose(f);
			b[strcspn(b, "\r\n")] = 0;
		}
		if (!b[0]) {
			reply(fd, "\r\n+CME ERROR: 4\r\n");
			return;
		}
		reply(fd, "\r\n+CGSN: ");
		reply(fd, b);
		reply(fd, "\r\n");
		reply_ok(fd);
		return;
	}
	/* Real modems answer these with the bare value (no "+CGMI: " prefix);
	 * QModem takes line 2 verbatim, so a prefix would show up on the page
	 * as "+CGMM: GM800".  ICCID keeps its prefix because gosuncn.sh greps
	 * for "+ICCID:" explicitly. */
	if (cmd_is(line, "AT+CGMI")) {
		reply(fd, "\r\nGosunCn\r\n");
		reply_ok(fd);
		return;
	}
	if (cmd_is(line, "AT+CGMM")) {
		reply(fd, "\r\nGM800\r\n");
		reply_ok(fd);
		return;
	}
	if (cmd_is(line, "AT+CGMR")) {
		char v[64] = { 0 };
		id_lookup("rev", v, sizeof(v));
		reply(fd, "\r\n");
		reply(fd, v[0] ? v : "GM800M1.HH500V");
		reply(fd, "\r\n");
		reply_ok(fd);
		return;
	}
	if (cmd_is(line, "AT+CIMI")) {
		char v[64] = { 0 };
		id_lookup("imsi", v, sizeof(v));
		if (!v[0]) {
			reply(fd, "\r\n+CME ERROR: 4\r\n");
			return;
		}
		reply(fd, "\r\n");
		reply(fd, v);
		reply(fd, "\r\n");
		reply_ok(fd);
		return;
	}
	if (cmd_is(line, "AT+ICCID") || cmd_is(line, "AT+CCID") ||
	    cmd_is(line, "AT+QCCID")) {
		char v[64] = { 0 };
		id_lookup("iccid", v, sizeof(v));
		if (!v[0]) {
			reply(fd, "\r\n+CME ERROR: 4\r\n");
			return;
		}
		reply(fd, "\r\n+ICCID: ");
		reply(fd, v);
		reply(fd, "\r\n");
		reply_ok(fd);
		return;
	}
	/*
	 * ---- GosunCn / Qualcomm vendor surface -----------------------------
	 * QModem's "advanced configuration" page runs the vendor script, and the
	 * vendor script speaks AT+ZSWITCH / AT+ZSNT / AT+ZBAND.  The module itself
	 * never answers those once the data session is up (and AT+ZSWITCH?,
	 * AT+ZSNT?, AT+ZCELLINFO? answer nothing even in the boot AT window), so
	 * the gateway keeps the last known/set value and reports it back.  This
	 * makes the pages usable; the values are bookkeeping, not modem state -
	 * the real dial path is netifd's proto mbim and is left untouched.
	 */
	if (cmd_is(line, "AT+ZSWITCH?")) {
		char v[16] = { 0 };

		kv_get(STATE_FILE, "zswitch", v, sizeof(v));
		reply(fd, "\r\n+ZSWITCH: ");
		reply(fd, v[0] ? v : "8");	/* 8 = MBIM, what we actually boot in */
		reply(fd, "\r\n");
		reply_ok(fd);
		return;
	}
	if (starts(line, "AT+ZSWITCH=")) {
		if (line[11])
			kv_set(STATE_FILE, "zswitch", line + 11);
		reply_ok(fd);
		return;
	}
	if (cmd_is(line, "AT+ZSNT?")) {
		char v[32] = { 0 };

		kv_get(STATE_FILE, "zsnt", v, sizeof(v));
		reply(fd, "\r\n+ZSNT: ");
		/* 8 = LTE+NR5G (auto).  That is the mode we run in: 5G with LTE fallback.
		 * 7 = NR5G only is what a phone calls "NR only"; /usr/bin/hh500v-netmode.sh
		 * records it here and the boot AT window hands it to the module. */
		reply(fd, v[0] ? v : "8,0,0");
		reply(fd, "\r\n");
		reply_ok(fd);
		return;
	}
	if (starts(line, "AT+ZSNT=")) {
		if (line[8])
			kv_set(STATE_FILE, "zsnt", line + 8);
		reply_ok(fd);
		return;
	}
	if (cmd_is(line, "AT+ZPAS?")) {
		reply(fd, "\r\n+ZPAS: \"NGRAN\",\"PS_ONLY\",\"TDD\"\r\n");
		reply_ok(fd);
		return;
	}
	if (cmd_is(line, "AT+C5GREG?")) {
		char v[16] = { 0 };
		char b[48];

		kv_get(STATE_FILE, "reg", v, sizeof(v));
		snprintf(b, sizeof(b), "\r\n+C5GREG: 0,%s\r\n", v[0] ? v : "1");
		reply(fd, b);
		reply_ok(fd);
		return;
	}
	if (cmd_is(line, "AT+ZCELLINFO?") || starts(line, "AT+QENG") ||
	    cmd_is(line, "AT+CPSI?")) {
		/* Serving cell: only answer when the boot window actually saw one,
		 * otherwise the client should keep using whatever it had.
		 * The real modem format (verified 2026-11-06, GM800 SA on n78) is
		 *   +ZCELLINFO: NR5G: tac: 0X835500,pci: 948,band : 78,cell_id: 0,
		 *               freq: 633984,mcc: 460,mnc: 01,imsi: ...,rsrp: -70,
		 *               rsrq: -11,sinr: 27
		 * so we replay exactly that, with every value coming from the boot
		 * AT window cache. */
		char pci[16] = { 0 }, arfcn[16] = { 0 }, band[16] = { 0 };
		char sinr[16] = { 0 }, rsrp[16] = { 0 }, rsrq[16] = { 0 };
		char tac[24] = { 0 };

		/* Carrier aggregation: the module prints one +ZCELLINFO line per
		 * camped cell, so a two-line reply means a secondary carrier is
		 * active. hh500v-sig-cache.sh saves the raw reply; replaying it
		 * verbatim is the only way a client (or hh500v-ca.sh) can ever
		 * see more than one carrier. */
		{
			FILE *cf = fopen(CELLS_FILE, "r");
			if (cf) {
				char cl[512];
				int n = 0;
				while (fgets(cl, sizeof(cl), cf)) {
					size_t len = strlen(cl);
					while (len && (cl[len - 1] == '\n' || cl[len - 1] == '\r'))
						cl[--len] = 0;
					if (!len)
						continue;
					reply(fd, "\r\n");
					reply(fd, cl);
					reply(fd, "\r\n");
					n++;
				}
				fclose(cf);
				if (n) {
					reply_ok(fd);
					return;
				}
			}
		}

		kv_get(STATE_FILE, "pci", pci, sizeof(pci));
		if (pci[0]) {
			char one[256];

			kv_get(STATE_FILE, "earfcn", arfcn, sizeof(arfcn));
			kv_get(STATE_FILE, "band", band, sizeof(band));
			kv_get(STATE_FILE, "sinr", sinr, sizeof(sinr));
			kv_get(STATE_FILE, "rsrp", rsrp, sizeof(rsrp));
			kv_get(STATE_FILE, "rsrq", rsrq, sizeof(rsrq));
			kv_get(STATE_FILE, "tac", tac, sizeof(tac));
			snprintf(one, sizeof(one),
				 "\r\n+ZCELLINFO: NR5G: tac: 0X%s,pci: %s,band : %s,"
				 "cell_id: 0,freq: %s,mcc: 460,mnc: 01,rsrp: %s,"
				 "rsrq: %s,sinr: %s\r\n",
				 tac[0] ? tac : "1A2B", pci, band[0] ? band : "78",
				 arfcn[0] ? arfcn : "0", rsrp[0] ? rsrp : "-",
				 rsrq[0] ? rsrq : "-", sinr[0] ? sinr : "0");
			reply(fd, one);
			reply_ok(fd);
			return;
		}
		/* No cache yet: still answer with the "no cell" form 3ginfo can
		 * parse, so the page does not hang waiting for "+ZCELLINFO". */
		reply(fd, "\r\n+ZCELLINFO: NR5G: tac: 0X1A2B,pci: 0,band : 0,"
			  "cell_id: 0,freq: 0,mcc: 460,mnc: 01,rsrp: -,rsrq: -,"
			  "sinr: 0\r\n");
		reply_ok(fd);
		return;
	}
	if (cmd_is(line, "AT+QCFG=?")) {
		/* The GM800 AT build is Qualcomm-generic and has no Quectel +QCFG at
		 * all (AT+CLAC lists 163 commands, not one +QCFG). Tools that expect
		 * it - QModem's quectel profile, some scripts - just error out, so we
		 * publish the subset this gateway can answer truthfully.
		 * NOTE: these answers describe *our* configuration. There is no NV
		 * write path on this firmware, so +QCFG cannot change the module. */
		reply(fd, "\r\n+QCFG: \"band\",\"nwscanmode\",\"servicedomain\","
			  "\"ims\",\"nr5g_disable_ca\"\r\n");
		reply_ok(fd);
		return;
	}
	if (starts(line, "AT+QCFG=")) {
		char key[32] = { 0 };
		const char *arg = line + 8;

		while (*arg == ' ')
			arg++;
		if (*arg == '"') {
			const char *e = strchr(arg + 1, '"');
			size_t n = e ? (size_t)(e - arg - 1) : 0;
			if (n >= sizeof(key))
				n = sizeof(key) - 1;
			memcpy(key, arg + 1, n);
			key[n] = 0;
			arg = e ? e + 1 : arg + 1;
		}
		while (*arg == ',' || *arg == ' ')
			arg++;
		/* value present -> set, absent -> query */
		if (!*arg) {
			char v[64] = { 0 };
			if (!strcmp(key, "band")) {
				char l[192] = { 0 }, out[96];
				unsigned long mask = 0;
				char *p;

				kv_get(STATE_FILE, "locked_lte", l, sizeof(l));
				for (p = l; p && *p; ) {
					int b = atoi(p);
					char *c = strchr(p, ',');

					if (b >= 1 && b <= 32)
						mask |= 1UL << (b - 1);
					p = c ? c + 1 : NULL;
				}
				/* 0 means "no LTE restriction" in Quectel's own dialect */
				snprintf(out, sizeof(out), "\r\n+QCFG: \"band\",0x%lX\r\n", mask);
				reply(fd, out);
				reply_ok(fd);
				return;
			}
			if (!strcmp(key, "nwscanmode")) {
				char z[32] = { 0 };
				int mode = 0;	/* 0 = automatic */

				kv_get(STATE_FILE, "zsnt", z, sizeof(z));
				if (!strncmp(z, "7,", 2))
					mode = 4;	/* NR5G only */
				else if (!strncmp(z, "6,", 2))
					mode = 3;	/* LTE only */
				else if (!strncmp(z, "2,", 2))
					mode = 2;	/* WCDMA only */
				snprintf(v, sizeof(v), "\r\n+QCFG: \"nwscanmode\",%d\r\n", mode);
				reply(fd, v);
				reply_ok(fd);
				return;
			}
			if (!strcmp(key, "servicedomain")) {
				reply(fd, "\r\n+QCFG: \"servicedomain\",1\r\n");	/* PS only */
				reply_ok(fd);
				return;
			}
			if (!strcmp(key, "ims")) {
				reply(fd, "\r\n+QCFG: \"ims\",1\r\n");
				reply_ok(fd);
				return;
			}
			if (!strcmp(key, "nr5g_disable_ca")) {
				/* 0 = CA not disabled. We cannot switch CA on or off - the
				 * gNB schedules secondary carriers - but reporting 0 keeps
				 * clients from believing CA was turned off locally. */
				reply(fd, "\r\n+QCFG: \"nr5g_disable_ca\",0\r\n");
				reply_ok(fd);
				return;
			}
			reply(fd, "\r\n+CME ERROR: 4\r\n");	/* unsupported key */
			return;
		}
		/* set: only the keys we can actually apply are accepted */
		if (!strcmp(key, "nwscanmode")) {
			const char *z = NULL;

			switch (atoi(arg)) {
			case 4: z = "7,0,0"; break;	/* NR5G only */
			case 3: z = "6,0,0"; break;	/* LTE only */
			case 2: z = "2,0,0"; break;	/* WCDMA only */
			case 0: z = "0,0,0"; break;	/* automatic */
			}
			if (!z) {
				reply(fd, "\r\n+CME ERROR: 3\r\n");
				return;
			}
			kv_set(STATE_FILE, "zsnt", z);
			reply_ok(fd);
			return;
		}
		if (!strcmp(key, "nr5g_disable_ca")) {
			if (atoi(arg) == 0) {
				reply_ok(fd);	/* nothing to do, which is the truth */
				return;
			}
			/* Refusing is the honest answer: this firmware has no CA toggle. */
			reply(fd, "\r\n+CME ERROR: 3\r\n");
			return;
		}
		reply(fd, "\r\n+CME ERROR: 4\r\n");
		return;
	}
	if (cmd_is(line, "AT+ZBAND=?")) {
		/* the real module answers exactly this: the syntax, not a band list */
		reply(fd, "\r\n+ZBAND: (0~5),(1~10),(1~320)\r\n");
		reply_ok(fd);
		return;
	}
	if (cmd_is(line, "AT+ZBAND?")) {
		char w[192] = { 0 }, l[192] = { 0 }, r[192] = { 0 };

		kv_get(BANDS_FILE, "locked_wcdma", w, sizeof(w));
		kv_get(BANDS_FILE, "locked_lte", l, sizeof(l));
		kv_get(BANDS_FILE, "locked_nr", r, sizeof(r));
		/* same layout the module uses: one technology per line, so the
		 * vendor script's `grep -i LTE | cut -d: -f2` picks the right one */
		reply(fd, "\r\n+ZBAND:\r\nWCDMA: ");
		reply(fd, w);
		reply(fd, "\r\nLTE: ");
		reply(fd, l);
		reply(fd, "\r\nNR5G: ");
		reply(fd, r);
		reply(fd, "\r\n");
		reply_ok(fd);
		return;
	}
	if (starts(line, "AT+ZBAND=")) {
		char arg[200];
		char rat[8] = { 0 };
		char *list = NULL, *c1 = NULL, *c2 = NULL;

		snprintf(arg, sizeof(arg), "%s", line + 9);
		if (!strcasecmp(arg, "0") || starts(arg, "all")) {
			kv_set(BANDS_FILE, "locked_wcdma", "");
			kv_set(BANDS_FILE, "locked_lte", "");
			kv_set(BANDS_FILE, "locked_nr", "");
			reply_ok(fd);
			return;
		}
		/* AT+ZBAND=<rat>,<count>,<b1,b2,...> - the list itself contains
		 * commas, so split on the first two only */
		c1 = strchr(arg, ',');
		if (c1) {
			c2 = strchr(c1 + 1, ',');
			if (c2) {
				size_t n = (size_t)(c1 - arg);

				if (n >= sizeof(rat))
					n = sizeof(rat) - 1;
				memcpy(rat, arg, n);
				rat[n] = 0;
				list = c2 + 1;
			}
		}
		if (list && rat[0] && list[0]) {
			if (!strcmp(rat, "1"))
				kv_set(BANDS_FILE, "locked_lte", list);
			else if (!strcmp(rat, "5"))
				kv_set(BANDS_FILE, "locked_nr", list);
			else if (!strcmp(rat, "3"))
				kv_set(BANDS_FILE, "locked_wcdma", list);
		}
		/* QModem stores whatever this returns and treats an empty answer as
		 * a failure (the ubus call comes back "Invalid argument"), so echo
		 * the applied set back instead of answering with a bare OK. */
		reply(fd, "\r\n+ZBAND: ");
		reply(fd, arg);
		reply(fd, "\r\n");
		reply_ok(fd);
		return;
	}
	if (starts(line, "AT+EGMR=0,")) {	/* read the IMEI we were given */
		char v[32] = { 0 };
		FILE *f = fopen(IMEI_FILE, "r");

		if (f) {
			if (!fgets(v, sizeof(v), f))
				v[0] = 0;
			fclose(f);
			v[strcspn(v, "\r\n")] = 0;
		}
		if (!v[0]) {
			reply(fd, "\r\n+CME ERROR: 3\r\n");
			return;
		}
		reply(fd, "\r\n+EGMR: \"");
		reply(fd, v);
		reply(fd, "\"\r\n");
		reply_ok(fd);
		return;
	}
	if (starts(line, "AT+EGMR=1,")) {	/* AT+EGMR=1,7,"<15 digits>" */
		const char *q = strchr(line, '"');
		char imei[32] = { 0 };
		size_t nd = 0;
		const char *p;
		FILE *f;

		if (q) {
			snprintf(imei, sizeof(imei), "%s", q + 1);
			imei[strcspn(imei, "\"")] = 0;
		}
		for (p = imei; *p; p++)
			if (*p >= '0' && *p <= '9')
				nd++;
		if (nd != 15 || strlen(imei) != 15) {
			reply(fd, "\r\n+CME ERROR: 3\r\n");
			return;
		}
		f = fopen(IMEI_FILE, "w");
		if (f) {
			fprintf(f, "%s\n", imei);
			fclose(f);
			lg("imei stored: %s", imei);
		}
		/* QModem's cmd_cgsn() answers from this cache, not from AT */
		f = fopen("/tmp/qmodem-cgsn.cache", "w");
		if (f) {
			fprintf(f, "%s\n", imei);
			fclose(f);
		}
		reply_ok(fd);
		return;
	}
	if (cmd_is(line, "AT+CSCS")) {
		if (strchr(line, '?'))
			reply(fd, "\r\n+CSCS: \"UCS2\"\r\n");
		reply_ok(fd);
		return;
	}
	if (cmd_is(line, "AT+CMGF")) {
		reply(fd, "\r\n+CMGF: 0\r\n");
		reply_ok(fd);
		return;
	}
	if (cmd_is(line, "AT+CNMI")) {
		reply(fd, "\r\n+CNMI: 2,1,0,0,0\r\n");
		reply_ok(fd);
		return;
	}
	if (cmd_is(line, "AT+CSMS")) {
		if (strchr(line, '?'))
			reply(fd, "\r\n+CSMS: 0,1,1,1\r\n");
		else
			reply(fd, "\r\n+CSMS: 1,1,1\r\n");
		reply_ok(fd);
		return;
	}

	n = sms_load(list, 100);

	if (cmd_is(line, "AT+CPMS")) {
		char b[128];
		if (strchr(line, '?')) {
			snprintf(b, sizeof(b),
				 "\r\n+CPMS: \"SM\",%d,50,\"SM\",%d,50,\"SM\",%d,50\r\n",
				 n, n, n);
			reply(fd, b);
		}
		reply_ok(fd);
		return;
	}

	if (cmd_is(line, "AT+CMGL")) {
		char *semi = strchr(line, ';');

		if (semi)
			*semi = 0;
		for (i = 0; i < n; i++) {
			unsigned char pdu[1600];
			char hex[3600], hdr[128];
			size_t len = build_pdu(&list[i], pdu, sizeof(pdu));
			hex_str(pdu, len, hex);
			snprintf(hdr, sizeof(hdr), "\r\n+CMGL: %d,0,,%u\r\n",
				 i + 1, (unsigned)len);
			reply(fd, hdr);
			reply(fd, hex);
			reply(fd, "\r\n");
		}
		reply_ok(fd);
		return;
	}

	if (cmd_is(line, "AT+CMGR")) {
		char *eq = strchr(line, '=');
		int want = eq ? atoi(eq + 1) : 0;
		unsigned char pdu[1600];
		char hex[3600], hdr[128];
		size_t len;

		if (want < 1 || want > n) {
			reply(fd, "\r\n+CME ERROR: 21\r\n");
			return;
		}
		len = build_pdu(&list[want - 1], pdu, sizeof(pdu));
		hex_str(pdu, len, hex);
		snprintf(hdr, sizeof(hdr), "\r\n+CMGR: 0,,%u\r\n", (unsigned)len);
		reply(fd, hdr);
		reply(fd, hex);
		reply(fd, "\r\n");
		reply_ok(fd);
		return;
	}

	if (cmd_is(line, "AT+CMGD")) {
		char *eq = strchr(line, '=');
		int want = eq ? atoi(eq + 1) : 0;

		if (want < 1 || want > n || sms_delete(want) != 0) {
			reply(fd, "\r\n+CME ERROR: 21\r\n");
			return;
		}
		lg("CMGD %d", want);
		reply_ok(fd);
		return;
	}

	if (cmd_is(line, "AT+CMGS")) {
		/* Works since the MBIM SMS_SEND payload was pinned down
		 * (format=0, ref-byte-array as {offset=8,len}, see
		 * hh500v-mbimprox.c g_ladder).  Tested against 10010 with the
		 * data session up: +CMGS comes back and the link stays up.
		 * Set /etc/hh500v-sms-send-deny to disable sending again. */
		if (access("/etc/hh500v-sms-send-deny", F_OK) == 0) {
			lg("CMGS refused (send disabled, /etc/hh500v-sms-send-deny)");
			reply(fd, "\r\n+CMS ERROR: 303\r\n");
			return;
		}
		/* two stage: prompt now, the PDU arrives on the next line */
		if (g_cur)
			g_cur->cmgs = 1;
		reply(fd, "\r\n> ");
		lg("CMGS prompt (waiting for PDU)");
		return;
	}

	reply_err(fd);
}

/*
 * Real modems accept "AT+CPIN?;+CSQ;+COPS?" style chains (3ginfo-lite sends
 * exactly that).  Split on ';', answer every part, and only the last one gets
 * the trailing OK.
 */
static void handle_line(int fd, char *line)
{
	char work[512];
	char *p;

	line[strcspn(line, "\r\n")] = 0;
	snprintf(work, sizeof(work), "%s", line);
	if (!strchr(work, ';')) {
		g_no_ok = 0;
		handle_single(fd, work);
		return;
	}

	p = work;
	while (p && *p) {
		char *semi = strchr(p, ';');
		char seg[256];

		if (semi)
			*semi = 0;
		while (*p == ' ' || *p == '\t')
			p++;
		if (p[0] == '+')
			snprintf(seg, sizeof(seg), "AT%s", p);
		else if (!strncasecmp(p, "AT", 2))
			snprintf(seg, sizeof(seg), "%s", p);
		else
			snprintf(seg, sizeof(seg), "AT%s", p);
		g_no_ok = (semi != NULL);
		handle_single(fd, seg);
		p = semi ? semi + 1 : NULL;
	}
	g_no_ok = 0;
}

/* ------------------------------------------------------------------- main */

static int still_bound(struct link *l)
{
	struct stat a, b;

	if (stat(l->dev, &a) != 0 || stat(l->pts, &b) != 0)
		return 0;
	return a.st_rdev == b.st_rdev;
}

static void on_term(int sig)
{
	int i;

	(void)sig;
	for (i = 0; i < g_nlinks; i++)
		umount2(g_links[i].dev, MNT_DETACH);
	_exit(0);
}

/* impersonate l->dev with a pty; returns 0 on success */
static int setup_link(struct link *l)
{
	struct termios tio;

	l->master = l->slave = -1;
	l->last_ms = 0;
	if (access(l->dev, F_OK) != 0) {
		lg("%s missing, skipped", l->dev);
		return -1;
	}
	l->master = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (l->master < 0) {
		lg("posix_openpt: %s", strerror(errno));
		return -1;
	}
	if (grantpt(l->master) < 0 || unlockpt(l->master) < 0) {
		lg("grantpt/unlockpt: %s", strerror(errno));
		return -1;
	}
	snprintf(l->pts, sizeof(l->pts), "%s", ptsname(l->master));
	l->slave = open(l->pts, O_RDWR | O_NOCTTY);
	if (l->slave < 0) {
		lg("open slave %s: %s", l->pts, strerror(errno));
		return -1;
	}
	if (tcgetattr(l->slave, &tio) == 0) {
		cfmakeraw(&tio);
		tio.c_cflag |= CLOCAL | CREAD;
		tcsetattr(l->slave, TCSANOW, &tio);
	}
	umount2(l->dev, MNT_DETACH);
	if (mount(l->pts, l->dev, NULL, MS_BIND, NULL) < 0) {
		lg("bind mount %s over %s failed: %s", l->pts, l->dev,
		   strerror(errno));
		return -1;
	}
	lg("bind-mounted %s over %s (sms store: %s)", l->pts, l->dev, SMS_DBED);
	return 0;
}

/* milliseconds on the monotonic clock; the reply-flush gap has to be finer
 * than a second because two client processes are only milliseconds apart */
static long now_ms(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return (long)time(NULL) * 1000;
	return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void pump(struct link *l)
{
	for (;;) {
		char *nl = strchr(l->buf, '\r');
		char *nl2 = strchr(l->buf, '\n');
		char *cz = strchr(l->buf, 0x1a);	/* Ctrl-Z ends a PDU */
		char *end = nl && (!nl2 || nl < nl2) ? nl : nl2;
		size_t linelen, consumed;

		if (cz && (!end || cz < end))
			end = cz;
		if (!end)
			break;
		*end = 0;
		linelen = strlen(l->buf);
		if (linelen) {
			long now;

			/* A client that gives up in the middle of a chain leaves
			 * replies queued for us to read out; the next client would
			 * read them as answers to its own commands (that is how
			 * AT+ZSWITCH? ended up returning +CSQ).  The replies sit in
			 * the *input* queue of the slave, so the flush is TCIFLUSH,
			 * and the gap has to be measured in milliseconds: separate
			 * client processes are milliseconds apart, the segments of
			 * one semicolon chain all arrive in the same read. */
			now = now_ms();
			if (l->last_ms && now - l->last_ms > 5 && l->slave >= 0)
				tcflush(l->slave, TCIFLUSH);
			l->last_ms = now;
			g_cur = l;
			handle_line(l->master, l->buf);
			g_cur = NULL;
		}
		consumed = (size_t)(end - l->buf) + 1;
		memmove(l->buf, l->buf + consumed, l->used - consumed);
		l->used -= consumed;
		l->buf[l->used] = 0;
	}
}

int main(int argc, char **argv)
{
	struct pollfd pfd[MAX_LINKS];
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-d") && i + 1 < argc) {
			if (g_nlinks >= MAX_LINKS) {
				fprintf(stderr, "too many -d\n");
				return 1;
			}
			snprintf(g_links[g_nlinks].dev,
				 sizeof(g_links[g_nlinks].dev), "%s", argv[++i]);
			g_nlinks++;
		} else if (!strcmp(argv[i], "-v")) {
			g_verbose = 1;
		} else {
			fprintf(stderr, "usage: %s [-d device]... [-v]\n", argv[0]);
			return 1;
		}
	}
	if (g_nlinks == 0) {
		snprintf(g_links[0].dev, sizeof(g_links[0].dev), "/dev/ttyUSB2");
		g_nlinks = 1;
	}

	signal(SIGTERM, on_term);
	signal(SIGINT, on_term);
	signal(SIGPIPE, SIG_IGN);

	/* the SMS timestamps we hand out are the stored local times, so make
	 * sure localtime() does not fall back to UTC in a bare procd env */
	{
		char tzbuf[64];
		FILE *tzf = fopen("/etc/TZ", "r");

		if (tzf) {
			if (fgets(tzbuf, sizeof(tzbuf), tzf)) {
				tzbuf[strcspn(tzbuf, "\r\n")] = 0;
				if (tzbuf[0])
					setenv("TZ", tzbuf, 1);
			}
			fclose(tzf);
		} else {
			setenv("TZ", "CST-8", 1);
		}
		tzset();
	}

	for (i = 0; i < g_nlinks; i++)
		setup_link(&g_links[i]);

	for (;;) {
		int alive = 0;

		for (i = 0; i < MAX_LINKS; i++) {
			pfd[i].fd = -1;
			pfd[i].events = POLLIN;
			pfd[i].revents = 0;
		}
		for (i = 0; i < g_nlinks; i++) {
			if (g_links[i].master < 0)
				continue;
			/* the option driver recreates /dev/ttyUSBx whenever the
			 * modem's USB interfaces are rebound, which silently
			 * drops our mount */
			if (!still_bound(&g_links[i])) {
				lg("mount lost on %s, rebinding over %s",
				   g_links[i].pts, g_links[i].dev);
				umount2(g_links[i].dev, MNT_DETACH);
				if (mount(g_links[i].pts, g_links[i].dev, NULL,
					  MS_BIND, NULL) < 0)
					lg("rebind failed: %s", strerror(errno));
				else
					lg("rebound %s", g_links[i].dev);
			}
			pfd[i].fd = g_links[i].master;
			alive++;
		}
		if (alive == 0) {
			lg("no usable AT port, giving up");
			break;
		}
		if (poll(pfd, MAX_LINKS, 10000) <= 0)
			continue;

		for (i = 0; i < g_nlinks; i++) {
			ssize_t r;

			if (g_links[i].master < 0 ||
			    !(pfd[i].revents & (POLLIN | POLLHUP | POLLERR)))
				continue;
			r = read(g_links[i].master,
				 g_links[i].buf + g_links[i].used,
				 sizeof(g_links[i].buf) - g_links[i].used - 1);
			if (r < 0) {
				if (errno == EINTR || errno == EAGAIN)
					continue;
				lg("read %s: %s", g_links[i].dev, strerror(errno));
				continue;
			}
			if (r == 0)
				continue;
			g_links[i].used += (size_t)r;
			g_links[i].buf[g_links[i].used] = 0;
			pump(&g_links[i]);
			if (g_links[i].used > sizeof(g_links[i].buf) - 2)
				g_links[i].used = 0;
		}
	}

	for (i = 0; i < g_nlinks; i++)
		umount2(g_links[i].dev, MNT_DETACH);
	return 0;
}
