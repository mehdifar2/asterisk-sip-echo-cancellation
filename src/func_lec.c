/*
 * func_lec: line (network) echo canceller for Asterisk SIP / PJSIP trunk legs.
 *
 * Copyright (C) 2026 asterisk-sip-echo-cancellation contributors
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * This program is free software, distributed under the terms of the GNU General
 * Public License Version 2. See the LICENSE file at the top of the source tree.
 *
 * Attach on the carrier (trunk) channel:
 *   Set(LEC()=on)            moderate suppression (default)
 *   Set(LEC()=aggressive)    stronger suppression
 *   Set(LEC()=conservative)  weaker suppression
 *   Set(LEC()=off)           detach
 *   ${LEC()}                 current per-call metrics
 *
 * On that channel, WRITE frames (audio sent to the carrier) are the reference and
 * READ frames (audio from the carrier) are cleaned. The canceller is WebRTC AEC2
 * (lec_core_webrtc.c) and always runs at 8 kHz. The audiohook delivers slin at the
 * highest rate on the call (16 kHz with G.722, 48 kHz with Opus), so other rates are
 * resampled to 8 kHz and the cleaned audio back (speexdsp resampler).
 * One NOTICE line with metrics is logged when the channel goes away.
 */

/*** MODULEINFO
	<depend>speexdsp</depend>
	<support_level>extended</support_level>
 ***/

/*** DOCUMENTATION
	<function name="LEC" language="en_US">
		<synopsis>
			Cancel network (line) echo on a SIP trunk channel.
		</synopsis>
		<syntax>
			<parameter name="mode" required="false">
				<para>Value written: <literal>on</literal> (moderate suppression),
				<literal>aggressive</literal>, <literal>conservative</literal> or
				<literal>off</literal> (detach).</para>
			</parameter>
		</syntax>
		<description>
			<para>Attach to the carrier (trunk) channel, for example from a Dial pre-dial
			handler <literal>b()</literal>. Audio written to the carrier is the reference;
			audio read from the carrier has the echo removed (WebRTC AEC2).</para>
			<para>Reading <literal>${LEC()}</literal> returns the per-call metrics, or
			<literal>off</literal>. One NOTICE line with the metrics is logged at hangup.</para>
		</description>
	</function>
 ***/

#include "asterisk.h"

#include "asterisk/module.h"
#include "asterisk/channel.h"
#include "asterisk/pbx.h"
#include "asterisk/utils.h"
#include "asterisk/audiohook.h"
#include "asterisk/format_cache.h"
#include "asterisk/lock.h"
#include "asterisk/logger.h"
#include "asterisk/strings.h"

#include <speex/speex_resampler.h>

#include "lec_core.h"

#define LEC_RATE 8000
#define MAX_IN 1920        /* largest frame handled: 40 ms at 48 kHz */
#define UPQ_LEN 4800       /* rx output queue: 100 ms at 48 kHz */
#define RS_QUALITY 5

struct lec_info {
	struct ast_audiohook audiohook;
	ast_mutex_t lock;
	lec_t *lec;
	int nlp;
	int rate;                      /* current audiohook rate; 8000 = no resampling */
	SpeexResamplerState *tx_down, *rx_down, *rx_up;
	int16_t upq[UPQ_LEN];          /* cleaned rx at call rate, waiting to be written back */
	int upq_len;
	unsigned int resampled_frames;
	int have_seq;
	unsigned int last_seq;
	unsigned int rx_frames_seq;    /* rx frames that carried a sequence number */
	unsigned int rx_frames_noseq;  /* rx frames without one (gaps cannot be detected) */
	unsigned int skipped_format;   /* frames ignored because not 8 kHz slin */
	char chan_name[AST_CHANNEL_NAME];
};

static void lec_format_stats(struct lec_info *li, char *buf, size_t len)
{
	struct lec_stats st;
	double erle;

	lec_get_stats(li->lec, &st);
	erle = lec_erle_db(li->lec);
	snprintf(buf, len,
		"nlp=%d rx_s=%llu tx_s=%llu gaps=%llu underrun=%llu overflow=%llu echo_frames=%llu "
		"erle_db=%.1f delay_ms=%d delay_std=%d seq=%u noseq=%u badfmt=%u rate=%d resampled=%u",
		li->nlp,
		(unsigned long long) (st.rx_samples / 8000), (unsigned long long) (st.tx_samples / 8000),
		(unsigned long long) st.gap_samples, (unsigned long long) st.underrun,
		(unsigned long long) st.overflow, (unsigned long long) st.echo_frames,
		erle > -999 ? erle : 0.0, st.delay_median_ms, st.delay_std_ms,
		li->rx_frames_seq, li->rx_frames_noseq, li->skipped_format, li->rate, li->resampled_frames);
}

static void free_resamplers(struct lec_info *li)
{
	if (li->tx_down) {
		speex_resampler_destroy(li->tx_down);
	}
	if (li->rx_down) {
		speex_resampler_destroy(li->rx_down);
	}
	if (li->rx_up) {
		speex_resampler_destroy(li->rx_up);
	}
	li->tx_down = li->rx_down = li->rx_up = NULL;
}

/* Switch to a new audiohook rate. The canceller itself keeps running at 8 kHz. 0 = ok. */
static int set_rate(struct lec_info *li, int rate)
{
	int err = 0;

	if (rate == li->rate) {
		return 0;
	}
	free_resamplers(li);
	li->rate = rate;
	li->upq_len = 0;
	if (rate == LEC_RATE) {
		return 0;
	}
	if (rate < LEC_RATE || rate > 48000 || rate % 100) {
		return -1;
	}
	li->tx_down = speex_resampler_init(1, (spx_uint32_t) rate, LEC_RATE, RS_QUALITY, &err);
	li->rx_down = speex_resampler_init(1, (spx_uint32_t) rate, LEC_RATE, RS_QUALITY, &err);
	li->rx_up = speex_resampler_init(1, LEC_RATE, (spx_uint32_t) rate, RS_QUALITY, &err);
	if (!li->tx_down || !li->rx_down || !li->rx_up) {
		free_resamplers(li);
		return -1;
	}
	/* 10 ms of silence up front, so resampler output jitter never leaves the frame short. */
	li->upq_len = rate / 100;
	memset(li->upq, 0, sizeof(int16_t) * (size_t) li->upq_len);
	ast_verb(3, "LEC %s: audio at %d Hz, resampling to 8 kHz\n", li->chan_name, rate);
	return 0;
}

/* rx frame at a rate above 8 kHz: down to 8 kHz, cancel, back up, written in place. */
static void rx_resampled(struct lec_info *li, int16_t *data, int samples)
{
	int16_t low[MAX_IN];
	spx_uint32_t inl = (spx_uint32_t) samples, outl = MAX_IN, upl;
	int i, n;

	speex_resampler_process_int(li->rx_down, 0, data, &inl, low, &outl);
	lec_rx(li->lec, low, (int) outl);
	inl = outl;
	upl = (spx_uint32_t)(UPQ_LEN - li->upq_len);
	speex_resampler_process_int(li->rx_up, 0, low, &inl, li->upq + li->upq_len, &upl);
	li->upq_len += (int) upl;
	n = samples < li->upq_len ? samples : li->upq_len;
	memcpy(data, li->upq, sizeof(int16_t) * (size_t) n);
	for (i = n; i < samples; i++) {
		data[i] = 0;
	}
	li->upq_len -= n;
	memmove(li->upq, li->upq + n, sizeof(int16_t) * (size_t) li->upq_len);
	/* Bound the queue (latency) to 40 ms; drop the oldest if it grows past that. */
	if (li->upq_len > li->rate / 25) {
		int drop = li->upq_len - li->rate / 50;

		memmove(li->upq, li->upq + drop, sizeof(int16_t) * (size_t)(li->upq_len - drop));
		li->upq_len -= drop;
	}
	li->resampled_frames++;
}

static void tx_resampled(struct lec_info *li, const int16_t *data, int samples)
{
	int16_t low[MAX_IN];
	spx_uint32_t inl = (spx_uint32_t) samples, outl = MAX_IN;

	speex_resampler_process_int(li->tx_down, 0, data, &inl, low, &outl);
	lec_tx(li->lec, low, (int) outl);
}

static void destroy_callback(void *data)
{
	struct lec_info *li = data;
	char stats[512];

	if (!li) {
		return;
	}
	lec_format_stats(li, stats, sizeof(stats));
	ast_log(LOG_NOTICE, "LEC %s: %s\n", li->chan_name, stats);
	ast_audiohook_destroy(&li->audiohook);
	lec_free(li->lec);
	free_resamplers(li);
	ast_mutex_destroy(&li->lock);
	ast_free(li);
	ast_module_unref(ast_module_info->self);
}

static const struct ast_datastore_info lec_datastore = {
	.type = "lec",
	.destroy = destroy_callback,
};

static int lec_callback(struct ast_audiohook *audiohook, struct ast_channel *chan,
	struct ast_frame *frame, enum ast_audiohook_direction direction)
{
	struct ast_datastore *datastore;
	struct lec_info *li;
	int changed = -1;

	if (audiohook->status == AST_AUDIOHOOK_STATUS_DONE) {
		return -1;
	}
	if (frame->frametype != AST_FRAME_VOICE || !frame->data.ptr || frame->samples <= 0) {
		return -1;
	}
	datastore = ast_channel_datastore_find(chan, &lec_datastore, NULL);
	if (!datastore || !datastore->data) {
		return -1;
	}
	li = datastore->data;

	ast_mutex_lock(&li->lock);
	if (!ast_format_cache_is_slinear(frame->subclass.format) || frame->samples > MAX_IN
		|| set_rate(li, (int) ast_format_get_sample_rate(frame->subclass.format))) {
		li->skipped_format++;
	} else if (direction == AST_AUDIOHOOK_DIRECTION_WRITE) {
		/* Audio going to the carrier: reference only, frame unchanged. */
		if (li->rate == LEC_RATE) {
			lec_tx(li->lec, frame->data.ptr, frame->samples);
		} else {
			tx_resampled(li, frame->data.ptr, frame->samples);
		}
	} else if (direction == AST_AUDIOHOOK_DIRECTION_READ) {
		int samples8 = (int)((long) frame->samples * LEC_RATE / li->rate);

		if (ast_test_flag(frame, AST_FRFLAG_HAS_SEQUENCE_NUMBER)) {
			unsigned int seq = (unsigned int) frame->seqno & 0xffff;

			li->rx_frames_seq++;
			if (li->have_seq) {
				unsigned int miss = (seq - li->last_seq - 1) & 0xffff;

				/* A few lost packets: keep alignment. Large jumps: stream reset, ignore. */
				if (miss > 0 && miss < 50) {
					lec_rx_gap(li->lec, (int) miss * samples8);
				}
			}
			li->have_seq = 1;
			li->last_seq = seq;
		} else {
			li->rx_frames_noseq++;
		}
		if (li->rate == LEC_RATE) {
			lec_rx(li->lec, frame->data.ptr, frame->samples);
		} else {
			rx_resampled(li, frame->data.ptr, frame->samples);
		}
		changed = 0;
	}
	ast_mutex_unlock(&li->lock);

	return changed;
}

static int parse_mode(const char *value)
{
	if (ast_strlen_zero(value) || ast_false(value)) {
		return -1;
	}
	if (!strcasecmp(value, "aggressive")) {
		return 2;
	}
	if (!strcasecmp(value, "conservative")) {
		return 0;
	}
	if (ast_true(value) || !strcasecmp(value, "moderate")) {
		return 1;
	}
	return -2;
}

static int lec_detach(struct ast_channel *chan)
{
	struct ast_datastore *datastore;
	struct lec_info *li;

	ast_channel_lock(chan);
	datastore = ast_channel_datastore_find(chan, &lec_datastore, NULL);
	if (datastore) {
		ast_channel_datastore_remove(chan, datastore);
	}
	ast_channel_unlock(chan);
	if (!datastore) {
		return 0;
	}
	li = datastore->data;
	ast_audiohook_remove(chan, &li->audiohook);
	ast_audiohook_detach(&li->audiohook);
	ast_datastore_free(datastore);
	return 0;
}

static int lec_write(struct ast_channel *chan, const char *cmd, char *data, const char *value)
{
	struct ast_datastore *datastore;
	struct lec_info *li;
	int nlp;

	if (!chan) {
		ast_log(LOG_WARNING, "No channel was provided to %s function.\n", cmd);
		return -1;
	}
	nlp = parse_mode(value);
	if (nlp == -2) {
		ast_log(LOG_WARNING, "%s: unknown value '%s' (use on|off|aggressive|conservative)\n", cmd, value);
		return -1;
	}
	if (nlp == -1) {
		return lec_detach(chan);
	}

	ast_channel_lock(chan);
	datastore = ast_channel_datastore_find(chan, &lec_datastore, NULL);
	ast_channel_unlock(chan);
	if (datastore) {
		return 0; /* already attached */
	}

	li = ast_calloc(1, sizeof(*li));
	if (!li) {
		return -1;
	}
	li->lec = lec_new(0, nlp);
	if (!li->lec) {
		ast_log(LOG_ERROR, "%s: could not create echo canceller on %s\n", cmd, ast_channel_name(chan));
		ast_free(li);
		return -1;
	}
	li->nlp = nlp;
	li->rate = LEC_RATE;
	ast_mutex_init(&li->lock);
	ast_copy_string(li->chan_name, ast_channel_name(chan), sizeof(li->chan_name));
	/* No MANIPULATE_ALL_RATES flag: frames arrive as 8 kHz slin. */
	ast_audiohook_init(&li->audiohook, AST_AUDIOHOOK_TYPE_MANIPULATE, "lec", 0);
	li->audiohook.manipulate_callback = lec_callback;

	datastore = ast_datastore_alloc(&lec_datastore, NULL);
	if (!datastore) {
		ast_audiohook_destroy(&li->audiohook);
		lec_free(li->lec);
		ast_mutex_destroy(&li->lock);
		ast_free(li);
		return -1;
	}
	/* Held until the datastore is destroyed, so the module cannot be unloaded under a live call. */
	ast_module_ref(ast_module_info->self);
	datastore->data = li;

	ast_channel_lock(chan);
	if (ast_channel_datastore_find(chan, &lec_datastore, NULL)) {
		/* Attached concurrently by someone else: drop ours. */
		ast_channel_unlock(chan);
		ast_datastore_free(datastore);
		return 0;
	}
	ast_channel_datastore_add(chan, datastore);
	ast_channel_unlock(chan);

	if (ast_audiohook_attach(chan, &li->audiohook)) {
		ast_log(LOG_ERROR, "%s: could not attach audiohook on %s\n", cmd, ast_channel_name(chan));
		lec_detach(chan);
		return -1;
	}
	ast_verb(3, "LEC attached on %s (nlp %d)\n", li->chan_name, nlp);
	return 0;
}

static int lec_read(struct ast_channel *chan, const char *cmd, char *data, char *buf, size_t len)
{
	struct ast_datastore *datastore;
	struct lec_info *li;

	if (!chan) {
		return -1;
	}
	ast_channel_lock(chan);
	datastore = ast_channel_datastore_find(chan, &lec_datastore, NULL);
	if (!datastore) {
		ast_channel_unlock(chan);
		ast_copy_string(buf, "off", len);
		return 0;
	}
	li = datastore->data;
	ast_mutex_lock(&li->lock);
	lec_format_stats(li, buf, len);
	ast_mutex_unlock(&li->lock);
	ast_channel_unlock(chan);
	return 0;
}

static struct ast_custom_function lec_function = {
	.name = "LEC",
	.write = lec_write,
	.read = lec_read,
	.read_max = 512,
};

static int unload_module(void)
{
	return ast_custom_function_unregister(&lec_function);
}

static int load_module(void)
{
	/* Self-test: the canceller library must create and free an instance. */
	lec_t *t = lec_new(0, LEC_NLP_MODERATE);

	if (!t) {
		ast_log(LOG_ERROR, "LEC: WebRTC echo canceller could not be created, not loading\n");
		return AST_MODULE_LOAD_DECLINE;
	}
	lec_free(t);
	if (ast_custom_function_register(&lec_function)) {
		return AST_MODULE_LOAD_DECLINE;
	}
	return AST_MODULE_LOAD_SUCCESS;
}

AST_MODULE_INFO_STANDARD_EXTENDED(ASTERISK_GPL_KEY, "Line echo canceller for SIP trunk legs (WebRTC AEC)");
