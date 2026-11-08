/*
 * lec_core: network/line echo canceller for one call leg (8 kHz, signed 16-bit).
 *
 * Copyright (C) 2026 asterisk-sip-echo-cancellation contributors
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tx = audio we send to the carrier (the reference signal).
 * rx = audio coming back from the carrier (far-end speech plus our echo).
 *
 * tx samples are queued and each rx sample is paired with tx sample number i, so
 * jitter in frame arrival never moves the alignment. If rx runs ahead of tx (tx
 * late), the rx sample is paired with silence and the late tx sample is dropped on
 * arrival. Lost rx packets can be reported with lec_rx_gap().
 *
 * The backend is WebRTC AEC2 (webrtc-audio-processing 0.3.x) with the extended
 * filter and delay-agnostic mode, which finds the bulk echo delay by itself.
 *
 * Not thread safe: the caller serializes all calls on one instance.
 */
#ifndef LEC_CORE_H
#define LEC_CORE_H

#include <stdint.h>

typedef struct lec lec_t;

struct lec_stats {
	uint64_t rx_samples;      /* rx samples processed */
	uint64_t tx_samples;      /* tx samples queued */
	uint64_t gap_samples;     /* rx samples reported lost */
	uint64_t underrun;        /* rx samples paired with silence (tx late or not started) */
	uint64_t overflow;        /* tx samples dropped because the queue was full */
	uint64_t echo_frames;     /* 10 ms frames counted as "echo only" (we talk, far end quiet) */
	double echo_in_energy;    /* rx energy before cancelling, echo-only frames */
	double echo_out_energy;   /* rx energy after cancelling, echo-only frames */
	int delay_median_ms;      /* backend's echo delay estimate (-1 if unknown) */
	int delay_std_ms;
};

/* Suppression (non-linear processing) levels. */
#define LEC_NLP_CONSERVATIVE 0
#define LEC_NLP_MODERATE     1
#define LEC_NLP_AGGRESSIVE   2

/* hint_ms: optional delay hint in ms (0 = let the canceller find it).
 * nlp: LEC_NLP_*. Returns NULL on failure. */
lec_t *lec_new(int hint_ms, int nlp);
void lec_free(lec_t *l);
void lec_tx(lec_t *l, const int16_t *s, int n);
void lec_rx(lec_t *l, int16_t *s, int n);
void lec_rx_gap(lec_t *l, int n);
void lec_get_stats(const lec_t *l, struct lec_stats *out);
/* Echo reduction in dB over echo-only frames, or -1000 if not enough data yet. */
double lec_erle_db(const lec_t *l);

#endif
