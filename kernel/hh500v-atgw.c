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
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define SMS_DBED "/etc/hh500v-sms.txt"
#define IMEI_FILE "/etc/hh500v-imei"
#define LOGFILE "/tmp/hh500v-atgw.log"

static int g_master = -1, g_slave = -1, g_verbose;
static char g_ptsname[128], g_dev[128] = "/dev/ttyUSB2";

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
	reply(fd, "\r\nOK\r\n");
}

static void reply_err(int fd)
{
	reply(fd, "\r\nERROR\r\n");
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

static void handle_line(int fd, char *line)
{
	struct sms list[100];
	int n, i;

	line[strcspn(line, "\r\n")] = 0;
	while (*line == ' ' || *line == '\t')
		line++;
	if (g_verbose)
		lg("cmd: %s", line);
	if (strncasecmp(line, "AT", 2) != 0) {
		reply_err(fd);
		return;
	}

	if (line[2] == 0 || !strcasecmp(line, "AT")) {
		reply_ok(fd);
		return;
	}

	if (cmd_is(line, "ATI")) {
		reply(fd, "\r\nTCL LINKHUB HH500V\r\nGM800\r\nRevision: HH500V-ATGW\r\n");
		reply_ok(fd);
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
		reply(fd, "\r\n+CSQ: 20,99\r\n");
		reply_ok(fd);
		return;
	}
	if (cmd_is(line, "AT+CREG?") || cmd_is(line, "AT+CGREG?") ||
	    cmd_is(line, "AT+CEREG?")) {
		const char *n = strstr(line, "CGREG") ? "+CGREG" :
				strstr(line, "CEREG") ? "+CEREG" : "+CREG";
		char b[64];
		snprintf(b, sizeof(b), "\r\n%s: 0,1\r\n", n);
		reply(fd, b);
		reply_ok(fd);
		return;
	}
	if (cmd_is(line, "AT+COPS?")) {
		reply(fd, "\r\n+COPS: 0,0,\"CHN-UNICOM\",11\r\n");
		reply_ok(fd);
		return;
	}
	if (cmd_is(line, "AT+COPS=")) {
		reply_ok(fd);
		return;
	}
	if (cmd_is(line, "AT+CGSN")) {
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
		reply(fd, "\r\n");
		reply(fd, b);
		reply(fd, "\r\n");
		reply_ok(fd);
		return;
	}
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
		lg("CMGS refused (send not implemented)");
		reply(fd, "\r\nERROR\r\n");
		return;
	}

	reply_err(fd);
}

/* ------------------------------------------------------------------- main */

static void on_term(int sig)
{
	(void)sig;
	if (g_dev[0])
		umount2(g_dev, MNT_DETACH);
	_exit(0);
}

int main(int argc, char **argv)
{
	char buf[4096];
	size_t used = 0;
	struct termios tio;
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-d") && i + 1 < argc)
			snprintf(g_dev, sizeof(g_dev), "%s", argv[++i]);
		else if (!strcmp(argv[i], "-v"))
			g_verbose = 1;
		else {
			fprintf(stderr, "usage: %s [-d device] [-v]\n", argv[0]);
			return 1;
		}
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

	if (access(g_dev, F_OK) != 0) {
		lg("device %s missing", g_dev);
		return 1;
	}

	g_master = posix_openpt(O_RDWR | O_NOCTTY);
	if (g_master < 0) {
		lg("posix_openpt: %s", strerror(errno));
		return 1;
	}
	if (grantpt(g_master) < 0 || unlockpt(g_master) < 0) {
		lg("grantpt/unlockpt: %s", strerror(errno));
		return 1;
	}
	snprintf(g_ptsname, sizeof(g_ptsname), "%s", ptsname(g_master));
	g_slave = open(g_ptsname, O_RDWR | O_NOCTTY);
	if (g_slave < 0) {
		lg("open slave %s: %s", g_ptsname, strerror(errno));
		return 1;
	}
	if (tcgetattr(g_slave, &tio) == 0) {
		cfmakeraw(&tio);
		tio.c_cflag |= CLOCAL | CREAD;
		tcsetattr(g_slave, TCSANOW, &tio);
	}
	lg("pty %s ready", g_ptsname);

	umount2(g_dev, MNT_DETACH);
	if (mount(g_ptsname, g_dev, NULL, MS_BIND, NULL) < 0) {
		lg("bind mount %s over %s failed: %s", g_ptsname, g_dev,
		   strerror(errno));
		return 1;
	}
	lg("bind-mounted %s over %s (sms store: %s)", g_ptsname, g_dev, SMS_DBED);

	for (;;) {
		ssize_t r = read(g_master, buf + used, sizeof(buf) - used - 1);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			lg("read: %s", strerror(errno));
			break;
		}
		if (r == 0)
			continue;
		used += (size_t)r;
		buf[used] = 0;
		for (;;) {
			char *nl = strchr(buf, '\r');
			char *nl2 = strchr(buf, '\n');
			char *end = nl && (!nl2 || nl < nl2) ? nl : nl2;
			size_t linelen;

			if (!end)
				break;
			*end = 0;
			linelen = strlen(buf);
			if (linelen)
				handle_line(g_master, buf);
			/* drop the line terminator, keep the rest */
			{
				size_t consumed = (size_t)(end - buf) + 1;
				memmove(buf, buf + consumed, used - consumed);
				used -= consumed;
				buf[used] = 0;
			}
		}
		if (used > sizeof(buf) - 2)
			used = 0;
	}

	umount2(g_dev, MNT_DETACH);
	return 0;
}
