/*
 * lec_replay: offline test. Feed one recorded trunk call (pcap with G.711 a-law/u-law
 * RTP, captured on the PBX's trunk interface) through lec_core in real packet arrival
 * order, and report the echo reduction.
 *
 * Copyright (C) 2026 asterisk-sip-echo-cancellation contributors
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Usage: lec_replay file.pcap <carrier_media_ip> <hint_ms> <mode> [out.raw [mix.raw]]
 *   hint_ms: delay hint in ms, 0 = let the canceller find it.
 *   mode:    0 = pass-through (baseline), 1 = conservative, 2 = moderate, 3 = aggressive.
 *   out.raw: cleaned rx audio, signed 16-bit 8 kHz mono.
 *   mix.raw: optional s16 8 kHz audio added to rx before cancelling (to simulate the far
 *            end talking, a double-talk test); the mixed input is written to <out.raw>.in.
 * Packets from carrier_media_ip are rx; packets to it are tx. Only the first RTP pair is used.
 * Supported capture link types: Ethernet (1) and Linux cooked (113).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <arpa/inet.h>
#include "lec_core.h"

#define MAXPKT 65536

/* ITU-T G.711 decoders. */
static int16_t alaw_to_linear(uint8_t a)
{
	int t, seg;

	a ^= 0x55;
	t = (a & 0x0f) << 4;
	seg = (a & 0x70) >> 4;
	if (seg == 0) {
		t += 8;
	} else {
		t = (t + 0x108) << (seg - 1);
	}
	return (int16_t)((a & 0x80) ? t : -t);
}

static int16_t ulaw_to_linear(uint8_t u)
{
	int t;

	u = ~u;
	t = (((u & 0x0f) << 3) + 0x84) << ((u & 0x70) >> 4);
	return (int16_t)((u & 0x80) ? (0x84 - t) : (t - 0x84));
}

static int decode(int pt, const uint8_t *p, int n, int16_t *out)
{
	int i;

	for (i = 0; i < n; i++) {
		out[i] = pt == 8 ? alaw_to_linear(p[i]) : ulaw_to_linear(p[i]);
	}
	return n;
}

int main(int argc, char **argv)
{
	FILE *f, *fo = NULL, *fm = NULL;
	int16_t *mix = NULL;
	size_t mixlen = 0, rxpos = 0;
	uint8_t gh[24], ph[16];
	static uint8_t pkt[MAXPKT];
	uint32_t carrier, magic, linktype;
	int swap, hint_ms, mode, passthru, have_seq = 0;
	uint16_t last_seq = 0;
	uint16_t tx_port = 0, rx_port = 0;
	lec_t *l;
	struct lec_stats st;
	uint64_t ntx = 0, nrx = 0;

	if (argc < 5) {
		fprintf(stderr, "usage: %s file.pcap carrier_ip hint_ms mode(0-3) [out.raw [mix.raw]]\n", argv[0]);
		return 2;
	}
	if (inet_pton(AF_INET, argv[2], &carrier) != 1) {
		fprintf(stderr, "bad ip\n");
		return 2;
	}
	hint_ms = atoi(argv[3]);
	mode = atoi(argv[4]);
	if (mode < 0 || mode > 3) {
		fprintf(stderr, "mode must be 0-3\n");
		return 2;
	}
	f = fopen(argv[1], "rb");
	if (!f || fread(gh, 1, 24, f) != 24) {
		fprintf(stderr, "cannot read pcap\n");
		return 1;
	}
	memcpy(&magic, gh, 4);
	if (magic == 0xa1b2c3d4) {
		swap = 0;
	} else if (magic == 0xd4c3b2a1) {
		swap = 1;
	} else {
		fprintf(stderr, "unsupported pcap magic\n");
		return 1;
	}
#define U32(b) (swap ? (uint32_t)((b)[0] << 24 | (b)[1] << 16 | (b)[2] << 8 | (b)[3]) \
                     : (uint32_t)((b)[3] << 24 | (b)[2] << 16 | (b)[1] << 8 | (b)[0]))
	linktype = U32(gh + 20);
	if (linktype != 1 && linktype != 113) {
		fprintf(stderr, "unsupported linktype %u\n", linktype);
		return 1;
	}
	if (argc > 5 && !(fo = fopen(argv[5], "wb"))) {
		fprintf(stderr, "cannot open output\n");
		return 1;
	}
	if (argc > 6) {
		/* mix.raw: s16 mono 8 kHz added to rx; the mixed input is saved to <out>.in */
		FILE *fx = fopen(argv[6], "rb");
		long sz;
		char nm[512];

		if (!fx || fseek(fx, 0, SEEK_END) != 0 || (sz = ftell(fx)) <= 0 || fseek(fx, 0, SEEK_SET) != 0) {
			fprintf(stderr, "cannot read mix file\n");
			return 1;
		}
		mixlen = (size_t) sz / sizeof(int16_t);
		mix = malloc(mixlen * sizeof(int16_t));
		if (!mix || fread(mix, sizeof(int16_t), mixlen, fx) != mixlen) {
			fprintf(stderr, "cannot read mix file\n");
			return 1;
		}
		fclose(fx);
		snprintf(nm, sizeof(nm), "%s.in", argc > 5 ? argv[5] : "mix");
		fm = fopen(nm, "wb");
	}
	passthru = mode == 0;
	l = lec_new(hint_ms, passthru ? LEC_NLP_MODERATE : mode - 1);
	if (!l) {
		fprintf(stderr, "lec_new failed\n");
		return 1;
	}
	while (fread(ph, 1, 16, f) == 16) {
		uint32_t incl = U32(ph + 8);
		int l2, ihl, n, pt, cc;
		uint16_t proto, sport, dport, seq;
		uint32_t src, dst;
		const uint8_t *ip, *udp, *rtp;
		int16_t pcm[2048];

		if (incl > MAXPKT || fread(pkt, 1, incl, f) != incl) {
			break;
		}
		l2 = linktype == 113 ? 16 : 14;
		if ((int) incl < l2 + 20 + 8 + 12) {
			continue;
		}
		proto = (uint16_t)(pkt[l2 - 2] << 8 | pkt[l2 - 1]);
		ip = pkt + l2;
		if (proto != 0x0800 || ip[9] != 17) {
			continue;
		}
		ihl = (ip[0] & 15) * 4;
		if ((int) incl < l2 + ihl + 8 + 12) {
			continue;
		}
		memcpy(&src, ip + 12, 4);
		memcpy(&dst, ip + 16, 4);
		udp = ip + ihl;
		sport = (uint16_t)(udp[0] << 8 | udp[1]);
		dport = (uint16_t)(udp[2] << 8 | udp[3]);
		rtp = udp + 8;
		if ((rtp[0] >> 6) != 2) {
			continue;
		}
		pt = rtp[1] & 0x7f;
		if (pt != 0 && pt != 8) {
			continue;
		}
		cc = rtp[0] & 15;
		seq = (uint16_t)(rtp[2] << 8 | rtp[3]);
		n = (int) incl - (int)(rtp - pkt) - 12 - 4 * cc;
		if (n <= 0 || n > 2048) {
			continue;
		}
		decode(pt, rtp + 12 + 4 * cc, n, pcm);
		if (dst == carrier) {
			if (!tx_port) {
				tx_port = sport;
			}
			if (sport != tx_port) {
				continue;
			}
			lec_tx(l, pcm, n);
			ntx++;
		} else if (src == carrier) {
			if (!rx_port) {
				rx_port = dport;
			}
			if (dport != rx_port) {
				continue;
			}
			if (have_seq) {
				uint16_t miss = (uint16_t)(seq - last_seq - 1);
				if (miss > 0 && miss < 50) {
					lec_rx_gap(l, miss * n);
				} else if (miss >= 50 && miss < 65000) {
					continue; /* reordered/old packet: drop */
				}
			}
			have_seq = 1;
			last_seq = seq;
			if (mix) {
				/* Test only: add fake far-end speech, indexed by rx sample position. */
				int i;

				for (i = 0; i < n; i++) {
					long v = pcm[i];

					if (rxpos + (size_t) i < mixlen) {
						v += mix[rxpos + (size_t) i];
					}
					pcm[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
				}
				if (fm) {
					fwrite(pcm, sizeof(int16_t), (size_t) n, fm);
				}
			}
			rxpos += (size_t) n;
			if (!passthru) {
				lec_rx(l, pcm, n);
			}
			nrx++;
			if (fo) {
				fwrite(pcm, sizeof(int16_t), (size_t) n, fo);
			}
		}
	}
	lec_get_stats(l, &st);
	printf("hint %d ms mode %d: tx pkts %llu, rx pkts %llu, gaps %llu samples, underrun %llu, overflow %llu\n",
		hint_ms, mode, (unsigned long long) ntx, (unsigned long long) nrx,
		(unsigned long long) st.gap_samples, (unsigned long long) st.underrun, (unsigned long long) st.overflow);
	printf("echo-only frames %llu, ERLE %.1f dB, backend delay median %d ms std %d\n",
		(unsigned long long) st.echo_frames, lec_erle_db(l), st.delay_median_ms, st.delay_std_ms);
	if (fm) {
		fclose(fm);
	}
	free(mix);
	lec_free(l);
	fclose(f);
	if (fo) {
		fclose(fo);
	}
	return 0;
}
