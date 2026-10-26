// SockSound.c: audio and PTT through a local socket instead of ALSA (OpenX6X00)
//
// Selected by giving "SOCK:<path>" as the capture device (the playback device
// is then not used; give the same string):
//     ardopcf --nologfile 8515 SOCK:/run/ardop.sock SOCK:/run/ardop.sock
//
// The radio's GUI owns the audio and keys the transmitter. It listens on a
// UNIX SOCK_SEQPACKET socket at <path>; ardopcf connects to it, and tries
// again once a second while it is not there. One message per packet: a 4-byte
// header {uint8 type, uint8 arg, uint16 n} in host byte order, followed by n
// mono signed 16-bit samples at 12000 Hz. Protocol version 1:
//   'H' ardopcf -> GUI  hello, arg = protocol version, on each connect
//   'R' GUI -> ardopcf  received audio, 240 samples (20 ms) per message, not
//                       sent while the radio transmits
//   'T' ardopcf -> GUI  audio to transmit, n <= 1200
//   'P' ardopcf -> GUI  arg = 1: key now; arg = 0: play out every 'T' sent
//                       before it, then unkey. A 'P 0' can come without a
//                       'P 1' before it (ardopcf also unkeys "for insurance").

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define HANDLE int
#include "common/ardopcommon.h"

#define SOCK_VERSION 1
#define SOCK_LEAD_MS 100  // the most TX audio sent ahead of the wall clock

struct sock_msg {
	uint8_t type;  // 'H', 'R', 'T' or 'P'
	uint8_t arg;
	uint16_t n;  // samples in s[]
	short s[1200];
};

extern unsigned int pttOnTime;  // Modulate.c: when the transmitter was keyed

BOOL UseSockAudio = FALSE;

static struct sockaddr_un sock_addr;
static int sock_fd = -1;
static unsigned int sock_next_try;  // when to try to connect again (Now)
static unsigned int sock_tx_samples;  // sent since the transmitter was keyed
static short rxbuf[2400];  // received audio SoundCardRead() has not taken yet
static unsigned int rxlen;

static void sock_close(const char *why)
{
	if (sock_fd >= 0) {
		ZF_LOGW("Audio socket %s closed: %s", sock_addr.sun_path, why);
		close(sock_fd);
		sock_fd = -1;
	}
	rxlen = 0;
}

static void sock_send(uint8_t type, uint8_t arg, const short *s, int n)
{
	struct sock_msg m;

	if (sock_fd < 0)
		return;
	m.type = type;
	m.arg = arg;
	m.n = n;
	if (n)
		memcpy(m.s, s, n * sizeof(short));
	if (send(sock_fd, &m, offsetof(struct sock_msg, s) + n * sizeof(short),
		MSG_DONTWAIT | MSG_NOSIGNAL) < 0)
	{
		if (errno == EAGAIN)
			ZF_LOGW("Audio socket full, '%c' message dropped", type);
		else
			sock_close(strerror(errno));
	}
}

static void sock_connect(void)
{
	int fd;

	if (sock_fd >= 0 || (int)(Now - sock_next_try) < 0)
		return;
	sock_next_try = Now + 1000;
	if ((fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0)) < 0)
		return;
	if (connect(fd, (struct sockaddr *)&sock_addr, sizeof(sock_addr)) < 0) {
		close(fd);
		return;
	}
	sock_fd = fd;
	ZF_LOGI("Audio socket %s connected", sock_addr.sun_path);
	sock_send('H', SOCK_VERSION, NULL, 0);
}

// From InitSound(), for the capture device "SOCK:<path>"
int sock_init(const char *dev)
{
	const char *path = dev + 5;

	if (!path[0] || strlen(path) >= sizeof(sock_addr.sun_path)) {
		ZF_LOGE("Audio socket path '%s' is empty or too long", path);
		return FALSE;
	}
	sock_addr.sun_family = AF_UNIX;
	strcpy(sock_addr.sun_path, path);
	UseSockAudio = TRUE;
	RadioControl = TRUE;  // so that KeyPTT() calls RadioPTT(), which sends 'P'
	sock_connect();
	if (sock_fd < 0)
		ZF_LOGW("Audio socket %s is not there yet, trying again every second", path);
	return TRUE;
}

// From SoundCardRead(): nSamples of received audio, or 0 while fewer have
// come in. Never waits.
int sock_read(short *input, unsigned int nSamples)
{
	struct sock_msg m;
	ssize_t len;

	sock_connect();
	while (rxlen < nSamples && sock_fd >= 0) {
		len = recv(sock_fd, &m, sizeof(m), MSG_DONTWAIT);
		if (len < 0 && (errno == EAGAIN || errno == EINTR))
			break;
		if (len <= 0) {
			sock_close(len == 0 ? "the other end went away" : strerror(errno));
			break;
		}
		if (m.type != 'R' || len != (ssize_t)(offsetof(struct sock_msg, s) + m.n * sizeof(short)))
			continue;  // not received audio, or cut short
		if (rxlen + m.n > sizeof(rxbuf) / sizeof(short))
			rxlen = 0;
		memcpy(&rxbuf[rxlen], m.s, m.n * sizeof(short));
		rxlen += m.n;
	}
	if (rxlen < nSamples)
		return 0;
	memcpy(input, rxbuf, nSamples * sizeof(short));
	rxlen -= nSamples;
	memmove(rxbuf, &rxbuf[nSamples], rxlen * sizeof(short));
	return nSamples;
}

// From SendtoCard(), for each block of TX audio (at most 1200 samples). Paced
// to the wall clock as a sound card would be, at most SOCK_LEAD_MS ahead of
// it, waiting in txSleep() so that the host is still served during a frame.
void sock_write(const short *s, int n)
{
	while ((int)(sock_tx_samples / 12) - (int)(Now - pttOnTime) > SOCK_LEAD_MS)
		txSleep(10);
	sock_send('T', 0, s, n);
	sock_tx_samples += n;
}

// From StopCapture(), just after the transmitter was keyed
void sock_tx_begin(void)
{
	sock_tx_samples = 0;
}

// From RadioPTT()
void sock_ptt(int on)
{
	sock_connect();
	sock_send('P', on ? 1 : 0, NULL, 0);
}

// From SoundFlush(), after the unkey: what came in before is old audio (with
// ALSA the capture device is opened again here)
void sock_rx_flush(void)
{
	struct sock_msg m;

	while (sock_fd >= 0 && recv(sock_fd, &m, sizeof(m), MSG_DONTWAIT) > 0)
		;
	rxlen = 0;
}
