/*
 * CDDL HEADER START
 *
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 *
 * CDDL HEADER END
 *
 * Copyright 2017 Saso Kiselkov. All rights reserved.
 */

#include <stddef.h>
#include <string.h>

#ifndef	TEST_STANDALONE_BUILD
#include <XPLMProcessing.h>
#else	/* TEST_STANDALONE_BUILD */
#include "thread.h"
#include "time.h"
#endif	/* TEST_STANDALONE_BUILD */

#include "assert.h"
#include "list.h"
#include "wav.h"

#include "snd_sys.h"

typedef struct msg {
	const char *file;
	wav_t *wav;
} msg_info_t;

static msg_info_t voice_msgs[RA_NUM_MSGS] = {
	{ .file = "clb.wav",		.wav = NULL },
	{ .file = "clb_cross.wav",	.wav = NULL },
	{ .file = "clb_more.wav",	.wav = NULL },
	{ .file = "clb_now.wav",	.wav = NULL },
	{ .file = "clear.wav",		.wav = NULL },
	{ .file = "des.wav",		.wav = NULL },
	{ .file = "des_cross.wav",	.wav = NULL },
	{ .file = "des_more.wav",	.wav = NULL },
	{ .file = "des_now.wav",	.wav = NULL },
	{ .file = "monitor_vs.wav",	.wav = NULL },
	{ .file = "maint_vs.wav",	.wav = NULL },
	{ .file = "maint_vs_cross.wav",	.wav = NULL },
	{ .file = "level_off.wav",	.wav = NULL },
	{ .file = "tfc.wav",		.wav = NULL }
};

static bool_t		inited = B_FALSE;
static bool_t		sound_is_on = B_TRUE;
static tcas_msg_t	cur_msg = -1;
static sound_on_t	sound_on = NULL;

#ifdef	TEST_STANDALONE_BUILD
static bool_t		worker_shutdown = B_FALSE;
static mutex_t		worker_lock;
static condvar_t	worker_cv;
static thread_t		worker_thr;
#endif	/* TEST_STANDALONE_BUILD */

static void
set_sound_on(bool_t flag)
{
	for (int i = 0; i < RA_NUM_MSGS; i++)
		xtcas_wav_set_gain(voice_msgs[i].wav, flag ? 1.0 : 0.0);
}

void
xtcas_play_msg(tcas_msg_t msg)
{
	ASSERT3U(msg, <, RA_NUM_MSGS);
#ifndef	TEST_STANDALONE_BUILD
	cur_msg = msg;
#else	/* !TEST_STANDALONE_BUILD */
	(void) xtcas_wav_play(voice_msgs[msg].wav);
#endif	/* !TEST_STANDALONE_BUILD */
}

static float
snd_sched_cb(float elapsed_since_last_call, float elapsed_since_last_floop,
    int counter, void *refcon)
{
	tcas_msg_t msg = cur_msg;

	ASSERT(inited);
	UNUSED(elapsed_since_last_call);
	UNUSED(elapsed_since_last_floop);
	UNUSED(counter);
	UNUSED(refcon);

	if ((int)msg != -1)
		return (-1.0);
	cur_msg = -1;

	/*
	 * Make sure our messages are only audible when we're inside
	 * the cockpit and AC power is on.
	 */
	if (sound_is_on && !sound_on()) {
		set_sound_on(B_FALSE);
		sound_is_on = B_FALSE;
	} else if (!sound_is_on && sound_on()) {
		set_sound_on(B_TRUE);
		sound_is_on = B_TRUE;
	}

#ifndef	TEST_STANDALONE_BUILD
	(void) xtcas_wav_play(voice_msgs[msg].wav);
#endif

	return (-1.0);
}

#ifdef	TEST_STANDALONE_BUILD

static void
snd_sched_loop(void)
{
	mutex_enter(&worker_lock);
	while (!worker_shutdown) {
		snd_sched_cb(0, 0, 0, NULL);
		cv_timedwait(&worker_cv, &worker_lock, microclock() + 50000);
	}
	mutex_exit(&worker_lock);
}

void
shutdown_worker(void)
{
	mutex_enter(&worker_lock);
	worker_shutdown = B_TRUE;
	cv_broadcast(&worker_cv);
	mutex_exit(&worker_lock);
	thread_join(&worker_thr);
}

#endif	/* TEST_STANDALONE_BUILD */

bool_t
xtcas_snd_sys_init(const char *snd_dir, sound_on_t snd_op)
{
	dbg_log(snd, 1, "snd_sys_init");

	ASSERT(!inited);

	/* no WAV/OpenAL calls before this */
	if (!xtcas_openal_init())
		return (B_FALSE);

	for (tcas_msg_t msg = 0; msg < RA_NUM_MSGS; msg++) {
		char *pathname;

		ASSERT3P(voice_msgs[msg].wav, ==, NULL);
		pathname = mkpathname(snd_dir, voice_msgs[msg].file, NULL);
		voice_msgs[msg].wav = xtcas_wav_load(pathname,
		    voice_msgs[msg].file);
		if (voice_msgs[msg].wav == NULL) {
			free(pathname);
			goto errout;
		}
		xtcas_wav_set_gain(voice_msgs[msg].wav, 1.0);
		free(pathname);
	}

	sound_on = snd_op;
#ifndef	TEST_STANDALONE_BUILD
	XPLMRegisterFlightLoopCallback(snd_sched_cb, -1.0, NULL);
#else	/* !TEST_STANDALONE_BUILD */
	mutex_init(&worker_lock);
	cv_init(&worker_cv);
	VERIFY(thread_create(&worker_thr, snd_sched_loop, NULL));
#endif	/* !TEST_STANDALONE_BUILD */

	inited = B_TRUE;

	return (B_TRUE);

errout:
	for (tcas_msg_t msg = 0; msg < RA_NUM_MSGS; msg++) {
		if (voice_msgs[msg].wav != NULL) {
			xtcas_wav_free(voice_msgs[msg].wav);
			voice_msgs[msg].wav = NULL;
		}
	}
	xtcas_openal_fini();

	return (B_FALSE);
}

void
xtcas_snd_sys_fini(void)
{
	dbg_log(snd, 1, "snd_sys_fini");

	if (!inited)
		return;

#ifndef	TEST_STANDALONE_BUILD
	XPLMUnregisterFlightLoopCallback(snd_sched_cb, NULL);
#else	/* TEST_STANDALONE_BUILD */
	shutdown_worker();
#endif	/* TEST_STANDALONE_BUILD */

	for (tcas_msg_t msg = 0; msg < RA_NUM_MSGS; msg++) {
		if (voice_msgs[msg].wav != NULL) {
			xtcas_wav_free(voice_msgs[msg].wav);
			voice_msgs[msg].wav = NULL;
		}
	}

	/* no more OpenAL/WAV calls after this */
	xtcas_openal_fini();

	inited = B_FALSE;
}
