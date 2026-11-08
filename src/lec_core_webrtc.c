/*
 * lec_core implementation on WebRTC AEC2 (webrtc-audio-processing 0.3.x, C API).
 *
 * Copyright (C) 2026 asterisk-sip-echo-cancellation contributors
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Extended filter + delay-agnostic mode, so the bulk echo delay is estimated internally.
 * Processing is in 10 ms blocks (80 samples at 8 kHz).
 *
 * The distribution packages ship no headers for this C API. The declarations below
 * are taken from webrtc-audio-processing 0.3 (BSD-3-Clause, Copyright (c) 2012 The
 * WebRTC project authors): webrtc/modules/audio_processing/aec/include/echo_cancellation.h
 * and aec/aec_core.h. They must match the installed library version.
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include "lec_core.h"

typedef struct {
	int16_t nlpMode;
	int16_t skewMode;
	int16_t metricsMode;
	int delay_logging;
} AecConfig;
struct AecCore;
void *WebRtcAec_Create(void);
void WebRtcAec_Free(void *aecInst);
int32_t WebRtcAec_Init(void *aecInst, int32_t sampFreq, int32_t scSampFreq);
int32_t WebRtcAec_BufferFarend(void *aecInst, const float *farend, size_t nrOfSamples);
int32_t WebRtcAec_Process(void *aecInst, const float *const *nearend, size_t num_bands,
	float *const *out, size_t nrOfSamples, int16_t msInSndCardBuf, int32_t skew);
int WebRtcAec_set_config(void *handle, AecConfig config);
int WebRtcAec_GetDelayMetrics(void *handle, int *median, int *std, float *fraction_poor_delays);
struct AecCore *WebRtcAec_aec_core(void *handle);
void WebRtcAec_enable_delay_agnostic(struct AecCore *self, int enable);
void WebRtcAec_enable_extended_filter(struct AecCore *self, int enable);

#define QUEUE_LEN 8000
#define FRAME 80
#define TXPOW_LEN 2400

struct lec {
	void *aec;
	int hint_ms;
	int16_t q[QUEUE_LEN];
	int qhead, qlen, tx_started, deficit;
	int16_t inbuf[FRAME], txbuf[FRAME], outbuf[FRAME];
	int infill, outpos, outfill;
	double txpow;
	int16_t txwin[TXPOW_LEN];
	int txwpos;
	struct lec_stats st;
};

lec_t *lec_new(int hint_ms, int nlp)
{
	lec_t *l;
	AecConfig cfg;

	l = calloc(1, sizeof(*l));
	if (!l) {
		return NULL;
	}
	l->aec = WebRtcAec_Create();
	if (!l->aec || WebRtcAec_Init(l->aec, 8000, 8000) != 0) {
		if (l->aec) {
			WebRtcAec_Free(l->aec);
		}
		free(l);
		return NULL;
	}
	WebRtcAec_enable_extended_filter(WebRtcAec_aec_core(l->aec), 1);
	WebRtcAec_enable_delay_agnostic(WebRtcAec_aec_core(l->aec), 1);
	memset(&cfg, 0, sizeof(cfg));
	cfg.nlpMode = (int16_t)(nlp < 0 ? 0 : nlp > 2 ? 2 : nlp);
	cfg.metricsMode = 1;
	cfg.delay_logging = 1;
	if (WebRtcAec_set_config(l->aec, cfg) != 0) {
		WebRtcAec_Free(l->aec);
		free(l);
		return NULL;
	}
	l->hint_ms = hint_ms < 0 ? 0 : hint_ms > 500 ? 500 : hint_ms;
	return l;
}

void lec_free(lec_t *l)
{
	if (!l) {
		return;
	}
	WebRtcAec_Free(l->aec);
	free(l);
}

void lec_tx(lec_t *l, const int16_t *s, int n)
{
	int i;

	l->tx_started = 1;
	for (i = 0; i < n; i++) {
		if (l->deficit > 0) {
			l->deficit--;
			continue;
		}
		if (l->qlen == QUEUE_LEN) {
			l->qhead = (l->qhead + 1) % QUEUE_LEN;
			l->qlen--;
			l->st.overflow++;
		}
		l->q[(l->qhead + l->qlen) % QUEUE_LEN] = s[i];
		l->qlen++;
	}
	l->st.tx_samples += (uint64_t) n;
}

static int16_t pop_tx(lec_t *l)
{
	int16_t v;

	if (l->qlen == 0) {
		if (l->tx_started && l->deficit < QUEUE_LEN) {
			l->deficit++;
		}
		l->st.underrun++;
		return 0;
	}
	v = l->q[l->qhead];
	l->qhead = (l->qhead + 1) % QUEUE_LEN;
	l->qlen--;
	return v;
}

static void process_frame(lec_t *l)
{
	float fin[FRAME], ftx[FRAME], fout[FRAME];
	const float *nb[1] = { fin };
	float *ob[1] = { fout };
	double f_in = 0, f_out = 0, txmean;
	int i;

	for (i = 0; i < FRAME; i++) {
		fin[i] = l->inbuf[i];
		ftx[i] = l->txbuf[i];
	}
	WebRtcAec_BufferFarend(l->aec, ftx, FRAME);
	if (WebRtcAec_Process(l->aec, nb, 1, ob, FRAME, (int16_t) l->hint_ms, 0) != 0) {
		memcpy(fout, fin, sizeof(fout)); /* on error pass audio through unchanged */
	}
	for (i = 0; i < FRAME; i++) {
		float v = fout[i];
		int16_t old = l->txwin[l->txwpos];

		l->outbuf[i] = (int16_t)(v > 32767.f ? 32767 : v < -32768.f ? -32768 : lrintf(v));
		l->txpow += (double) l->txbuf[i] * l->txbuf[i] - (double) old * old;
		l->txwin[l->txwpos] = l->txbuf[i];
		l->txwpos = (l->txwpos + 1) % TXPOW_LEN;
		f_in += (double) l->inbuf[i] * l->inbuf[i];
		f_out += (double) l->outbuf[i] * l->outbuf[i];
	}
	if (l->txpow < 0) {
		l->txpow = 0;
	}
	txmean = l->txpow / TXPOW_LEN;
	if (txmean > 300.0 * 300.0 && (f_in / FRAME) * 10.0 < txmean) {
		l->st.echo_frames++;
		l->st.echo_in_energy += f_in;
		l->st.echo_out_energy += f_out;
	}
	l->infill = 0;
	l->outpos = 0;
	l->outfill = FRAME;
}

static void push_rx(lec_t *l, int16_t in)
{
	l->txbuf[l->infill] = pop_tx(l);
	l->inbuf[l->infill++] = in;
	if (l->infill == FRAME) {
		process_frame(l);
	}
}

void lec_rx(lec_t *l, int16_t *s, int n)
{
	int i;

	if (l->infill == 0 && l->outfill - l->outpos == 0 && n % FRAME == 0) {
		/* Fast path: whole 10 ms blocks, no added latency. */
		int b;

		for (b = 0; b < n; b += FRAME) {
			for (i = 0; i < FRAME; i++) {
				push_rx(l, s[b + i]);
			}
			memcpy(s + b, l->outbuf, sizeof(l->outbuf));
			l->outpos = l->outfill = 0;
		}
	} else {
		for (i = 0; i < n; i++) {
			int16_t out = 0;

			if (l->outpos < l->outfill) {
				out = l->outbuf[l->outpos++];
			}
			push_rx(l, s[i]);
			s[i] = out;
		}
	}
	l->st.rx_samples += (uint64_t) n;
}

void lec_rx_gap(lec_t *l, int n)
{
	int i;

	for (i = 0; i < n; i++) {
		if (l->outpos < l->outfill) {
			l->outpos++;
		}
		push_rx(l, 0);
	}
	if (l->outpos >= l->outfill) {
		l->outpos = l->outfill = 0;
	}
	l->st.gap_samples += (uint64_t) n;
}

void lec_get_stats(const lec_t *l, struct lec_stats *out)
{
	int median = 0, std = 0;
	float poor = 0;

	*out = l->st;
	WebRtcAec_GetDelayMetrics(l->aec, &median, &std, &poor);
	out->delay_median_ms = median;
	out->delay_std_ms = std;
}

double lec_erle_db(const lec_t *l)
{
	if (l->st.echo_frames < 50 || l->st.echo_out_energy <= 0) {
		return -1000;
	}
	return 10.0 * log10(l->st.echo_in_energy / l->st.echo_out_energy);
}
