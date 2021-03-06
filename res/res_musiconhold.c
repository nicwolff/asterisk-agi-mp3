/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2006, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com>
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*! \file
 *
 * \brief Routines implementing music on hold
 *
 * \arg See also \ref Config_moh
 * 
 * \author Mark Spencer <markster@digium.com>
 */

/*** MODULEINFO
	<conflict>win32</conflict>
	<use>dahdi</use>
 ***/

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision: 125280 $")

#include <ctype.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/signal.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sys/ioctl.h>
#ifdef SOLARIS
#include <thread.h>
#endif

#if defined(HAVE_DAHDI)
#include <dahdi/user.h>
#endif

#include "asterisk/lock.h"
#include "asterisk/file.h"
#include "asterisk/channel.h"
#include "asterisk/pbx.h"
#include "asterisk/app.h"
#include "asterisk/module.h"
#include "asterisk/translate.h"
#include "asterisk/say.h"
#include "asterisk/musiconhold.h"
#include "asterisk/config.h"
#include "asterisk/utils.h"
#include "asterisk/cli.h"
#include "asterisk/stringfields.h"
#include "asterisk/linkedlists.h"

#define INITIAL_NUM_FILES   8

static char *play_moh = "MusicOnHold";
static char *wait_moh = "WaitMusicOnHold";
static char *set_moh = "SetMusicOnHold";
static char *start_moh = "StartMusicOnHold";
static char *stop_moh = "StopMusicOnHold";

static char *play_moh_syn = "Play Music On Hold indefinitely";
static char *wait_moh_syn = "Wait, playing Music On Hold";
static char *set_moh_syn = "Set default Music On Hold class";
static char *start_moh_syn = "Play Music On Hold";
static char *stop_moh_syn = "Stop Playing Music On Hold";

static char *play_moh_desc = "  MusicOnHold(class[,duration]):\n"
"Plays hold music specified by class.  If omitted, the default\n"
"music source for the channel will be used. Change the default \n"
"class with Set(CHANNEL(musicclass)=...).\n"
"If duration is given, hold music will be played specified number\n"
"of seconds. If duration is ommited, music plays indefinitely.\n"
"Returns 0 when done, -1 on hangup.\n";

static char *wait_moh_desc = "  WaitMusicOnHold(delay):\n"
"\n"
"  !!! DEPRECATED. Use MusicOnHold instead !!!\n"
"\n"
"Plays hold music specified number of seconds.  Returns 0 when\n"
"done, or -1 on hangup.  If no hold music is available, the delay will\n"
"still occur with no sound.\n"
"\n"
"  !!! DEPRECATED. Use MusicOnHold instead !!!\n";

static char *set_moh_desc = "  SetMusicOnHold(class):\n"
"\n"
"  !!! DEPRECATED. USe Set(CHANNEL(musicclass)=...) instead !!!\n"
"\n"
"Sets the default class for music on hold for a given channel.  When\n"
"music on hold is activated, this class will be used to select which\n"
"music is played.\n"
"\n"
"  !!! DEPRECATED. USe Set(CHANNEL(musicclass)=...) instead !!!\n";

static char *start_moh_desc = "  StartMusicOnHold(class):\n"
"Starts playing music on hold, uses default music class for channel.\n"
"Starts playing music specified by class.  If omitted, the default\n"
"music source for the channel will be used.  Always returns 0.\n";

static char *stop_moh_desc = "  StopMusicOnHold(): "
"Stops playing music on hold.\n";

static int respawn_time = 20;

struct moh_files_state {
	struct mohclass *class;
	int origwfmt;
	int samples;
	int sample_queue;
	int pos;
	int save_pos;
	char *save_pos_filename;
};

#define MOH_QUIET		(1 << 0)
#define MOH_SINGLE		(1 << 1)
#define MOH_CUSTOM		(1 << 2)
#define MOH_RANDOMIZE		(1 << 3)
#define MOH_SORTALPHA		(1 << 4)

#define MOH_CACHERTCLASSES      (1 << 5)        /*!< Should we use a separate instance of MOH for each user or not */

static struct ast_flags global_flags[1] = {{0}};        /*!< global MOH_ flags */

struct mohclass {
	char name[MAX_MUSICCLASS];
	char dir[256];
	char args[256];
	char mode[80];
	char digit;
	/*! A dynamically sized array to hold the list of filenames in "files" mode */
	char **filearray;
	/*! The current size of the filearray */
	int allowed_files;
	/*! The current number of files loaded into the filearray */
	int total_files;
	unsigned int flags;
	/*! The format from the MOH source, not applicable to "files" mode */
	int format;
	/*! The pid of the external application delivering MOH */
	int pid;
	time_t start;
	pthread_t thread;
	/*! Source of audio */
	int srcfd;
	/*! FD for timing source */
	int pseudofd;
	/*! Number of users */
	int inuse;
	/*! Created on the fly, from RT engine */
	int realtime;
	unsigned int delete:1;
	AST_LIST_HEAD_NOLOCK(, mohdata) members;
	AST_LIST_ENTRY(mohclass) list;
};

struct mohdata {
	int pipe[2];
	int origwfmt;
	struct mohclass *parent;
	struct ast_frame f;
	AST_LIST_ENTRY(mohdata) list;
};

AST_RWLIST_HEAD_STATIC(mohclasses, mohclass);

#define LOCAL_MPG_123 "/usr/local/bin/mpg123"
#define MPG_123 "/usr/bin/mpg123"
#define MAX_MP3S 256

static int ast_moh_destroy_one(struct mohclass *moh);
static int reload(void);

static void ast_moh_free_class(struct mohclass **mohclass) 
{
	struct mohdata *member;
	struct mohclass *class = *mohclass;
	int i;
	
	while ((member = AST_LIST_REMOVE_HEAD(&class->members, list)))
		ast_free(member);
	
	if (class->thread) {
		pthread_cancel(class->thread);
		class->thread = 0;
	}

	if (class->filearray) {
		for (i = 0; i < class->total_files; i++)
			ast_free(class->filearray[i]);
		ast_free(class->filearray);
	}

	ast_free(class);
	*mohclass = NULL;
}


static void moh_files_release(struct ast_channel *chan, void *data)
{
	struct moh_files_state *state;

	if (chan) {
		if ((state = chan->music_state)) {
			if (chan->stream) {
	                        ast_closestream(chan->stream);
	                        chan->stream = NULL;
	                }
			ast_verb(3, "Stopped music on hold on %s\n", chan->name);
	
			if (state->origwfmt && ast_set_write_format(chan, state->origwfmt)) {
				ast_log(LOG_WARNING, "Unable to restore channel '%s' to format '%d'\n", chan->name, state->origwfmt);
			}
			state->save_pos = state->pos;

			if (ast_atomic_dec_and_test(&state->class->inuse) && state->class->delete)
				ast_moh_destroy_one(state->class);
		}
	}
}


static int ast_moh_files_next(struct ast_channel *chan) 
{
	struct moh_files_state *state = chan->music_state;
	int tries;

	/* Discontinue a stream if it is running already */
	if (chan->stream) {
		ast_closestream(chan->stream);
		chan->stream = NULL;
	}

	if (!state->class->total_files) {
		ast_log(LOG_WARNING, "No files available for class '%s'\n", state->class->name);
		return -1;
	}

	/* If a specific file has been saved confirm it still exists and that it is still valid */
	if (state->save_pos >= 0 && state->save_pos < state->class->total_files && state->class->filearray[state->save_pos] == state->save_pos_filename) {
		state->pos = state->save_pos;
		state->save_pos = -1;
	} else if (ast_test_flag(state->class, MOH_RANDOMIZE)) {
		/* Get a random file and ensure we can open it */
		for (tries = 0; tries < 20; tries++) {
			state->pos = ast_random() % state->class->total_files;
			if (ast_fileexists(state->class->filearray[state->pos], NULL, NULL) > 0)
				break;
		}
		state->save_pos = -1;
		state->samples = 0;
	} else {
		/* This is easy, just increment our position and make sure we don't exceed the total file count */
		state->pos++;
		state->pos %= state->class->total_files;
		state->save_pos = -1;
		state->samples = 0;
	}

	if (!ast_openstream_full(chan, state->class->filearray[state->pos], chan->language, 1)) {
		ast_log(LOG_WARNING, "Unable to open file '%s': %s\n", state->class->filearray[state->pos], strerror(errno));
		state->pos++;
		state->pos %= state->class->total_files;
		return -1;
	}

	/* Record the pointer to the filename for position resuming later */
	state->save_pos_filename = state->class->filearray[state->pos];

	ast_debug(1, "%s Opened file %d '%s'\n", chan->name, state->pos, state->class->filearray[state->pos]);

	if (state->samples)
		ast_seekstream(chan->stream, state->samples, SEEK_SET);

	return 0;
}


static struct ast_frame *moh_files_readframe(struct ast_channel *chan) 
{
	struct ast_frame *f = NULL;
	
	if (!(chan->stream && (f = ast_readframe(chan->stream)))) {
		if (!ast_moh_files_next(chan))
			f = ast_readframe(chan->stream);
	}

	return f;
}

static int moh_files_generator(struct ast_channel *chan, void *data, int len, int samples)
{
	struct moh_files_state *state = chan->music_state;
	struct ast_frame *f = NULL;
	int res = 0;

	state->sample_queue += samples;

	while (state->sample_queue > 0) {
		if ((f = moh_files_readframe(chan))) {
			state->samples += f->samples;
			state->sample_queue -= f->samples;
			res = ast_write(chan, f);
			ast_frfree(f);
			if (res < 0) {
				ast_log(LOG_WARNING, "Failed to write frame to '%s': %s\n", chan->name, strerror(errno));
				return -1;
			}
		} else
			return -1;	
	}
	return res;
}


static void *moh_files_alloc(struct ast_channel *chan, void *params)
{
	struct moh_files_state *state;
	struct mohclass *class = params;

	if (!chan->music_state && (state = ast_calloc(1, sizeof(*state)))) {
		chan->music_state = state;
		state->class = class;
		state->save_pos = -1;
	} else 
		state = chan->music_state;

	if (state) {
		if (state->class != class) {
			/* initialize */
			memset(state, 0, sizeof(*state));
			state->class = class;
			if (ast_test_flag(state->class, MOH_RANDOMIZE) && class->total_files)
				state->pos = ast_random() % class->total_files;
		}

		state->origwfmt = chan->writeformat;

		ast_verb(3, "Started music on hold, class '%s', on %s\n", class->name, chan->name);
	}
	
	return chan->music_state;
}

/*! \note This function should be called with the mohclasses list locked */
static struct mohclass *get_mohbydigit(char digit)
{
	struct mohclass *moh = NULL;

	AST_RWLIST_TRAVERSE(&mohclasses, moh, list) {
		if (digit == moh->digit)
			break;
	}

	return moh;
}

static void moh_handle_digit(struct ast_channel *chan, char digit)
{
	struct mohclass *moh;
	const char *classname = NULL;

	AST_RWLIST_RDLOCK(&mohclasses);
	if ((moh = get_mohbydigit(digit)))
		classname = ast_strdupa(moh->name);
	AST_RWLIST_UNLOCK(&mohclasses);

	if (!moh)
		return;

	ast_moh_stop(chan);
	ast_moh_start(chan, classname, NULL);
}

static struct ast_generator moh_file_stream = 
{
	alloc: moh_files_alloc,
	release: moh_files_release,
	generate: moh_files_generator,
	digit: moh_handle_digit,
};

static int spawn_mp3(struct mohclass *class)
{
	int fds[2];
	int files = 0;
	char fns[MAX_MP3S][80];
	char *argv[MAX_MP3S + 50];
	char xargs[256];
	char *argptr;
	int argc = 0;
	DIR *dir = NULL;
	struct dirent *de;
	sigset_t signal_set, old_set;

	
	if (!strcasecmp(class->dir, "nodir")) {
		files = 1;
	} else {
		dir = opendir(class->dir);
		if (!dir && !strstr(class->dir,"http://") && !strstr(class->dir,"HTTP://")) {
			ast_log(LOG_WARNING, "%s is not a valid directory\n", class->dir);
			return -1;
		}
	}

	if (!ast_test_flag(class, MOH_CUSTOM)) {
		argv[argc++] = "mpg123";
		argv[argc++] = "-q";
		argv[argc++] = "-s";
		argv[argc++] = "--mono";
		argv[argc++] = "-r";
		argv[argc++] = "8000";
		
		if (!ast_test_flag(class, MOH_SINGLE)) {
			argv[argc++] = "-b";
			argv[argc++] = "2048";
		}
		
		argv[argc++] = "-f";
		
		if (ast_test_flag(class, MOH_QUIET))
			argv[argc++] = "4096";
		else
			argv[argc++] = "8192";
		
		/* Look for extra arguments and add them to the list */
		ast_copy_string(xargs, class->args, sizeof(xargs));
		argptr = xargs;
		while (!ast_strlen_zero(argptr)) {
			argv[argc++] = argptr;
			strsep(&argptr, ",");
		}
	} else  {
		/* Format arguments for argv vector */
		ast_copy_string(xargs, class->args, sizeof(xargs));
		argptr = xargs;
		while (!ast_strlen_zero(argptr)) {
			argv[argc++] = argptr;
			strsep(&argptr, " ");
		}
	}


	if (strstr(class->dir,"http://") || strstr(class->dir,"HTTP://")) {
		ast_copy_string(fns[files], class->dir, sizeof(fns[files]));
		argv[argc++] = fns[files];
		files++;
	} else if (dir) {
		while ((de = readdir(dir)) && (files < MAX_MP3S)) {
			if ((strlen(de->d_name) > 3) && 
			    ((ast_test_flag(class, MOH_CUSTOM) && 
			      (!strcasecmp(de->d_name + strlen(de->d_name) - 4, ".raw") || 
			       !strcasecmp(de->d_name + strlen(de->d_name) - 4, ".sln"))) ||
			     !strcasecmp(de->d_name + strlen(de->d_name) - 4, ".mp3"))) {
				ast_copy_string(fns[files], de->d_name, sizeof(fns[files]));
				argv[argc++] = fns[files];
				files++;
			}
		}
	}
	argv[argc] = NULL;
	if (dir) {
		closedir(dir);
	}
	if (pipe(fds)) {	
		ast_log(LOG_WARNING, "Pipe failed\n");
		return -1;
	}
	if (!files) {
		ast_log(LOG_WARNING, "Found no files in '%s'\n", class->dir);
		close(fds[0]);
		close(fds[1]);
		return -1;
	}
	if (time(NULL) - class->start < respawn_time) {
		sleep(respawn_time - (time(NULL) - class->start));
	}

	/* Block signals during the fork() */
	sigfillset(&signal_set);
	pthread_sigmask(SIG_BLOCK, &signal_set, &old_set);

	time(&class->start);
	class->pid = fork();
	if (class->pid < 0) {
		close(fds[0]);
		close(fds[1]);
		ast_log(LOG_WARNING, "Fork failed: %s\n", strerror(errno));
		return -1;
	}
	if (!class->pid) {
		int x;

		if (ast_opt_high_priority)
			ast_set_priority(0);

		/* Reset ignored signals back to default */
		signal(SIGPIPE, SIG_DFL);
		pthread_sigmask(SIG_UNBLOCK, &signal_set, NULL);

		close(fds[0]);
		/* Stdout goes to pipe */
		dup2(fds[1], STDOUT_FILENO);
		/* Close unused file descriptors */
		for (x=3;x<8192;x++) {
			if (-1 != fcntl(x, F_GETFL)) {
				close(x);
			}
		}
		/* Child */
		chdir(class->dir);
		if (ast_test_flag(class, MOH_CUSTOM)) {
			execv(argv[0], argv);
		} else {
			/* Default install is /usr/local/bin */
			execv(LOCAL_MPG_123, argv);
			/* Many places have it in /usr/bin */
			execv(MPG_123, argv);
			/* Check PATH as a last-ditch effort */
			execvp("mpg123", argv);
		}
		ast_log(LOG_WARNING, "Exec failed: %s\n", strerror(errno));
		close(fds[1]);
		_exit(1);
	} else {
		/* Parent */
		pthread_sigmask(SIG_SETMASK, &old_set, NULL);
		close(fds[1]);
	}
	return fds[0];
}

static void *monmp3thread(void *data)
{
#define	MOH_MS_INTERVAL		100

	struct mohclass *class = data;
	struct mohdata *moh;
	char buf[8192];
	short sbuf[8192];
	int res, res2;
	int len;
	struct timeval tv, tv_tmp;

	tv.tv_sec = 0;
	tv.tv_usec = 0;
	for(;/* ever */;) {
		pthread_testcancel();
		/* Spawn mp3 player if it's not there */
		if (class->srcfd < 0) {
			if ((class->srcfd = spawn_mp3(class)) < 0) {
				ast_log(LOG_WARNING, "Unable to spawn mp3player\n");
				/* Try again later */
				sleep(500);
				pthread_testcancel();
			}
		}
		if (class->pseudofd > -1) {
#ifdef SOLARIS
			thr_yield();
#endif
			/* Pause some amount of time */
			res = read(class->pseudofd, buf, sizeof(buf));
			pthread_testcancel();
		} else {
			long delta;
			/* Reliable sleep */
			tv_tmp = ast_tvnow();
			if (ast_tvzero(tv))
				tv = tv_tmp;
			delta = ast_tvdiff_ms(tv_tmp, tv);
			if (delta < MOH_MS_INTERVAL) {	/* too early */
				tv = ast_tvadd(tv, ast_samp2tv(MOH_MS_INTERVAL, 1000));	/* next deadline */
				usleep(1000 * (MOH_MS_INTERVAL - delta));
				pthread_testcancel();
			} else {
				ast_log(LOG_NOTICE, "Request to schedule in the past?!?!\n");
				tv = tv_tmp;
			}
			res = 8 * MOH_MS_INTERVAL;	/* 8 samples per millisecond */
		}
		if (AST_LIST_EMPTY(&class->members))
			continue;
		/* Read mp3 audio */
		len = ast_codec_get_len(class->format, res);
		
		if ((res2 = read(class->srcfd, sbuf, len)) != len) {
			if (!res2) {
				close(class->srcfd);
				class->srcfd = -1;
				pthread_testcancel();
				if (class->pid > 1) {
					kill(class->pid, SIGHUP);
					usleep(100000);
					kill(class->pid, SIGTERM);
					usleep(100000);
					kill(class->pid, SIGKILL);
					class->pid = 0;
				}
			} else {
				ast_debug(1, "Read %d bytes of audio while expecting %d\n", res2, len);
			}
			continue;
		}
		pthread_testcancel();
		AST_RWLIST_RDLOCK(&mohclasses);
		AST_RWLIST_TRAVERSE(&class->members, moh, list) {
			/* Write data */
			if ((res = write(moh->pipe[1], sbuf, res2)) != res2) {
				ast_debug(1, "Only wrote %d of %d bytes to pipe\n", res, res2);
			}
		}
		AST_RWLIST_UNLOCK(&mohclasses);
	}
	return NULL;
}

static int play_moh_exec(struct ast_channel *chan, void *data)
{
	char *parse;
	char *class;
	int timeout = -1;
	int res;
	AST_DECLARE_APP_ARGS(args,
		AST_APP_ARG(class);
		AST_APP_ARG(duration);
	);

	parse = ast_strdupa(data);

	AST_STANDARD_APP_ARGS(args, parse);

	if (!ast_strlen_zero(args.duration)) {
		if (sscanf(args.duration, "%d", &timeout) == 1) {
			timeout *= 1000;
		} else {
			ast_log(LOG_WARNING, "Invalid MusicOnHold duration '%s'. Will wait indefinitely.\n", args.duration);
		}
	}

	class = S_OR(args.class, NULL);
	if (ast_moh_start(chan, class, NULL)) {
		ast_log(LOG_WARNING, "Unable to start music on hold class '%s' on channel %s\n", class, chan->name);
		return 0;
	}

	if (timeout > 0)
		res = ast_safe_sleep(chan, timeout);
	else {
		while (!(res = ast_safe_sleep(chan, 10000)));
	}

	ast_moh_stop(chan);

	return res;
}

static int wait_moh_exec(struct ast_channel *chan, void *data)
{
	static int deprecation_warning = 0;
	int res;

	if (!deprecation_warning) {
		deprecation_warning = 1;
		ast_log(LOG_WARNING, "WaitMusicOnHold application is deprecated and will be removed. Use MusicOnHold with duration parameter instead\n");
	}

	if (!data || !atoi(data)) {
		ast_log(LOG_WARNING, "WaitMusicOnHold requires an argument (number of seconds to wait)\n");
		return -1;
	}
	if (ast_moh_start(chan, NULL, NULL)) {
		ast_log(LOG_WARNING, "Unable to start music on hold for %d seconds on channel %s\n", atoi(data), chan->name);
		return 0;
	}
	res = ast_safe_sleep(chan, atoi(data) * 1000);
	ast_moh_stop(chan);
	return res;
}

static int set_moh_exec(struct ast_channel *chan, void *data)
{
	static int deprecation_warning = 0;

	if (!deprecation_warning) {
		deprecation_warning = 1;
		ast_log(LOG_WARNING, "SetMusicOnHold application is deprecated and will be removed. Use Set(CHANNEL(musicclass)=...) instead\n");
	}

	if (ast_strlen_zero(data)) {
		ast_log(LOG_WARNING, "SetMusicOnHold requires an argument (class)\n");
		return -1;
	}
	ast_string_field_set(chan, musicclass, data);
	return 0;
}

static int start_moh_exec(struct ast_channel *chan, void *data)
{
	char *parse;
	char *class;
	AST_DECLARE_APP_ARGS(args,
		AST_APP_ARG(class);
	);

	parse = ast_strdupa(data);

	AST_STANDARD_APP_ARGS(args, parse);

	class = S_OR(args.class, NULL);
	if (ast_moh_start(chan, class, NULL)) 
		ast_log(LOG_WARNING, "Unable to start music on hold class '%s' on channel %s\n", class, chan->name);

	return 0;
}

static int stop_moh_exec(struct ast_channel *chan, void *data)
{
	ast_moh_stop(chan);

	return 0;
}

/*! \note This function should be called with the mohclasses list locked */
static struct mohclass *get_mohbyname(const char *name, int warn)
{
	struct mohclass *moh = NULL;

	AST_RWLIST_TRAVERSE(&mohclasses, moh, list) {
		if (!strcasecmp(name, moh->name))
			break;
	}

	if (!moh && warn)
		ast_log(LOG_DEBUG, "Music on Hold class '%s' not found in memory\n", name);

	return moh;
}

static struct mohdata *mohalloc(struct mohclass *cl)
{
	struct mohdata *moh;
	long flags;	
	
	if (!(moh = ast_calloc(1, sizeof(*moh))))
		return NULL;
	
	if (pipe(moh->pipe)) {
		ast_log(LOG_WARNING, "Failed to create pipe: %s\n", strerror(errno));
		ast_free(moh);
		return NULL;
	}

	/* Make entirely non-blocking */
	flags = fcntl(moh->pipe[0], F_GETFL);
	fcntl(moh->pipe[0], F_SETFL, flags | O_NONBLOCK);
	flags = fcntl(moh->pipe[1], F_GETFL);
	fcntl(moh->pipe[1], F_SETFL, flags | O_NONBLOCK);

	moh->f.frametype = AST_FRAME_VOICE;
	moh->f.subclass = cl->format;
	moh->f.offset = AST_FRIENDLY_OFFSET;

	moh->parent = cl;

	AST_RWLIST_WRLOCK(&mohclasses);
	AST_LIST_INSERT_HEAD(&cl->members, moh, list);
	AST_RWLIST_UNLOCK(&mohclasses);
	
	return moh;
}

static void moh_release(struct ast_channel *chan, void *data)
{
	struct mohdata *moh = data;
	int oldwfmt;
	struct moh_files_state *state;

	AST_RWLIST_WRLOCK(&mohclasses);
	AST_RWLIST_REMOVE(&moh->parent->members, moh, list);	
	AST_RWLIST_UNLOCK(&mohclasses);
	
	close(moh->pipe[0]);
	close(moh->pipe[1]);
	oldwfmt = moh->origwfmt;
	state = chan->music_state;
	if (moh->parent->delete && ast_atomic_dec_and_test(&moh->parent->inuse))
		ast_moh_destroy_one(moh->parent);
	if (ast_atomic_dec_and_test(&state->class->inuse) && state->class->delete)
		ast_moh_destroy_one(state->class);

	ast_free(moh);
	if (chan) {
		if (oldwfmt && ast_set_write_format(chan, oldwfmt)) 
			ast_log(LOG_WARNING, "Unable to restore channel '%s' to format %s\n", chan->name, ast_getformatname(oldwfmt));
		ast_verb(3, "Stopped music on hold on %s\n", chan->name);
	}
}

static void *moh_alloc(struct ast_channel *chan, void *params)
{
	struct mohdata *res;
	struct mohclass *class = params;
	struct moh_files_state *state;

	/* Initiating music_state for current channel. Channel should know name of moh class */
	if (!chan->music_state && (state = ast_calloc(1, sizeof(*state)))) {
		chan->music_state = state;
		state->class = class;
	} else
		state = chan->music_state;
	if (state && state->class != class) {
		memset(state, 0, sizeof(*state));
		state->class = class;
	}

	if ((res = mohalloc(class))) {
		res->origwfmt = chan->writeformat;
		if (ast_set_write_format(chan, class->format)) {
			ast_log(LOG_WARNING, "Unable to set channel '%s' to format '%s'\n", chan->name, ast_codec2str(class->format));
			moh_release(NULL, res);
			res = NULL;
		}
		ast_verb(3, "Started music on hold, class '%s', on channel '%s'\n", class->name, chan->name);
	}
	return res;
}

static int moh_generate(struct ast_channel *chan, void *data, int len, int samples)
{
	struct mohdata *moh = data;
	short buf[1280 + AST_FRIENDLY_OFFSET / 2];
	int res;

	if (!moh->parent->pid)
		return -1;

	len = ast_codec_get_len(moh->parent->format, samples);

	if (len > sizeof(buf) - AST_FRIENDLY_OFFSET) {
		ast_log(LOG_WARNING, "Only doing %d of %d requested bytes on %s\n", (int)sizeof(buf), len, chan->name);
		len = sizeof(buf) - AST_FRIENDLY_OFFSET;
	}
	res = read(moh->pipe[0], buf + AST_FRIENDLY_OFFSET/2, len);
	if (res <= 0)
		return 0;

	moh->f.datalen = res;
	moh->f.data = buf + AST_FRIENDLY_OFFSET / 2;
	moh->f.samples = ast_codec_get_samples(&moh->f);

	if (ast_write(chan, &moh->f) < 0) {
		ast_log(LOG_WARNING, "Failed to write frame to '%s': %s\n", chan->name, strerror(errno));
		return -1;
	}

	return 0;
}

static struct ast_generator mohgen = 
{
	alloc: moh_alloc,
	release: moh_release,
	generate: moh_generate,
	digit: moh_handle_digit
};

static int moh_add_file(struct mohclass *class, const char *filepath)
{
	if (!class->allowed_files) {
		if (!(class->filearray = ast_calloc(1, INITIAL_NUM_FILES * sizeof(*class->filearray))))
			return -1;
		class->allowed_files = INITIAL_NUM_FILES;
	} else if (class->total_files == class->allowed_files) {
		if (!(class->filearray = ast_realloc(class->filearray, class->allowed_files * sizeof(*class->filearray) * 2))) {
			class->allowed_files = 0;
			class->total_files = 0;
			return -1;
		}
		class->allowed_files *= 2;
	}

	if (!(class->filearray[class->total_files] = ast_strdup(filepath)))
		return -1;

	class->total_files++;

	return 0;
}

static int moh_sort_compare(const void *i1, const void *i2)
{
	char *s1, *s2;

	s1 = ((char **)i1)[0];
	s2 = ((char **)i2)[0];

	return strcasecmp(s1, s2);
}

static int moh_scan_files(struct mohclass *class) {

	DIR *files_DIR;
	struct dirent *files_dirent;
	char path[PATH_MAX];
	char filepath[PATH_MAX];
	char *ext;
	struct stat statbuf;
	int dirnamelen;
	int i;

	files_DIR = opendir(class->dir);
	if (!files_DIR) {
		ast_log(LOG_WARNING, "Cannot open dir %s or dir does not exist\n", class->dir);
		return -1;
	}

	for (i = 0; i < class->total_files; i++)
		ast_free(class->filearray[i]);

	class->total_files = 0;
	dirnamelen = strlen(class->dir) + 2;
	getcwd(path, sizeof(path));
	chdir(class->dir);
	while ((files_dirent = readdir(files_DIR))) {
		/* The file name must be at least long enough to have the file type extension */
		if ((strlen(files_dirent->d_name) < 4))
			continue;

		/* Skip files that starts with a dot */
		if (files_dirent->d_name[0] == '.')
			continue;

		/* Skip files without extensions... they are not audio */
		if (!strchr(files_dirent->d_name, '.'))
			continue;

		snprintf(filepath, sizeof(filepath), "%s/%s", class->dir, files_dirent->d_name);

		if (stat(filepath, &statbuf))
			continue;

		if (!S_ISREG(statbuf.st_mode))
			continue;

		if ((ext = strrchr(filepath, '.')))
			*ext = '\0';

		/* if the file is present in multiple formats, ensure we only put it into the list once */
		for (i = 0; i < class->total_files; i++)
			if (!strcmp(filepath, class->filearray[i]))
				break;

		if (i == class->total_files) {
			if (moh_add_file(class, filepath))
				break;
		}
	}

	closedir(files_DIR);
	chdir(path);
	if (ast_test_flag(class, MOH_SORTALPHA))
		qsort(&class->filearray[0], class->total_files, sizeof(char *), moh_sort_compare);
	return class->total_files;
}

static int moh_diff(struct mohclass *old, struct mohclass *new)
{
	if (!old || !new)
		return -1;

	if (strcmp(old->dir, new->dir))
		return -1;
	else if (strcmp(old->mode, new->mode))
		return -1;
	else if (strcmp(old->args, new->args))
		return -1;
	else if (old->flags != new->flags)
		return -1;

	return 0;
}

static int moh_register(struct mohclass *moh, int reload)
{
#ifdef HAVE_DAHDI
	int x;
#endif
	struct mohclass *mohclass = NULL;
	int res = 0;

	AST_RWLIST_WRLOCK(&mohclasses);
	if ((mohclass = get_mohbyname(moh->name, 0)) && !moh_diff(mohclass, moh)) {
		if (!mohclass->delete) {
			ast_log(LOG_WARNING, "Music on Hold class '%s' already exists\n", moh->name);
			ast_free(moh);
			AST_RWLIST_UNLOCK(&mohclasses);
			return -1;
		}
	}
	AST_RWLIST_UNLOCK(&mohclasses);

	time(&moh->start);
	moh->start -= respawn_time;
	
	if (!strcasecmp(moh->mode, "files")) {
		res = moh_scan_files(moh);
		if (res <= 0) {
			if (res == 0) {
				if (option_verbose > 2)
					ast_verbose(VERBOSE_PREFIX_3 "Files not found in %s for moh class:%s\n", moh->dir, moh->name);
			}
			ast_moh_free_class(&moh);
			return -1;
		}
		if (strchr(moh->args, 'r'))
			ast_set_flag(moh, MOH_RANDOMIZE);
	} else if (!strcasecmp(moh->mode, "mp3") || !strcasecmp(moh->mode, "mp3nb") || !strcasecmp(moh->mode, "quietmp3") || !strcasecmp(moh->mode, "quietmp3nb") || !strcasecmp(moh->mode, "httpmp3") || !strcasecmp(moh->mode, "custom")) {

		if (!strcasecmp(moh->mode, "custom"))
			ast_set_flag(moh, MOH_CUSTOM);
		else if (!strcasecmp(moh->mode, "mp3nb"))
			ast_set_flag(moh, MOH_SINGLE);
		else if (!strcasecmp(moh->mode, "quietmp3nb"))
			ast_set_flag(moh, MOH_SINGLE | MOH_QUIET);
		else if (!strcasecmp(moh->mode, "quietmp3"))
			ast_set_flag(moh, MOH_QUIET);
		
		moh->srcfd = -1;
#ifdef HAVE_DAHDI
		/* Open /dev/dahdi/pseudo for timing...  Is
		   there a better, yet reliable way to do this? */
		moh->pseudofd = open("/dev/dahdi/pseudo", O_RDONLY);
		if (moh->pseudofd < 0) {
			ast_log(LOG_WARNING, "Unable to open pseudo channel for timing...  Sound may be choppy.\n");
		} else {
			x = 320;
			ioctl(moh->pseudofd, DAHDI_SET_BLOCKSIZE, &x);
		}
#else
		moh->pseudofd = -1;
#endif
		if (ast_pthread_create_background(&moh->thread, NULL, monmp3thread, moh)) {
			ast_log(LOG_WARNING, "Unable to create moh...\n");
			if (moh->pseudofd > -1)
				close(moh->pseudofd);
			ast_moh_free_class(&moh);
			return -1;
		}
	} else {
		ast_log(LOG_WARNING, "Don't know how to do a mode '%s' music on hold\n", moh->mode);
		ast_moh_free_class(&moh);
		return -1;
	}

	AST_RWLIST_WRLOCK(&mohclasses);
	AST_RWLIST_INSERT_HEAD(&mohclasses, moh, list);
	AST_RWLIST_UNLOCK(&mohclasses);
	
	return 0;
}

static void local_ast_moh_cleanup(struct ast_channel *chan)
{
	struct moh_files_state *state = chan->music_state;

	if (state) {
		if (state->class->realtime) {
			if (ast_test_flag(global_flags, MOH_CACHERTCLASSES)) {
				/* We are cleaning out cached RT class, we should remove it from list, if no one else using it */
				if (!(state->class->inuse)) {
					/* Remove this class from list */
					AST_RWLIST_WRLOCK(&mohclasses);
					AST_RWLIST_REMOVE(&mohclasses, state->class, list);
					AST_RWLIST_UNLOCK(&mohclasses);
	
					/* Free some memory */
					ast_moh_destroy_one(state->class);
				}
			} else {
				ast_moh_destroy_one(state->class);
			}
		}
		ast_free(chan->music_state);
		chan->music_state = NULL;
	}
}

static struct mohclass *moh_class_malloc(void)
{
	struct mohclass *class;

	if ((class = ast_calloc(1, sizeof(*class)))) {
		class->format = AST_FORMAT_SLINEAR;
		class->realtime = 0;
	}

	return class;
}

static int local_ast_moh_start(struct ast_channel *chan, const char *mclass, const char *interpclass)
{
	struct mohclass *mohclass = NULL;
	struct ast_variable *var = NULL;
	struct ast_variable *tmp = NULL;
	struct moh_files_state *state = chan->music_state;
#ifdef HAVE_DAHDI
	int x;
#endif

	/* The following is the order of preference for which class to use:
	 * 1) The channels explicitly set musicclass, which should *only* be
	 *    set by a call to Set(CHANNEL(musicclass)=whatever) in the dialplan.
	 * 2) The mclass argument. If a channel is calling ast_moh_start() as the
	 *    result of receiving a HOLD control frame, this should be the
	 *    payload that came with the frame.
	 * 3) The interpclass argument. This would be from the mohinterpret
	 *    option from channel drivers. This is the same as the old musicclass
	 *    option.
	 * 4) The default class.
	 */
	
	/* First, let's check in memory for static and cached RT classes */
	AST_RWLIST_RDLOCK(&mohclasses);
	if (!ast_strlen_zero(chan->musicclass))
		mohclass = get_mohbyname(chan->musicclass, 1);
	if (!mohclass && !ast_strlen_zero(mclass))
		mohclass = get_mohbyname(mclass, 1);
	if (!mohclass && !ast_strlen_zero(interpclass))
		mohclass = get_mohbyname(interpclass, 1);
	AST_RWLIST_UNLOCK(&mohclasses);

	/* If no moh class found in memory, then check RT */
	if (!mohclass && ast_check_realtime("musiconhold")) {
		if (!ast_strlen_zero(chan->musicclass)) {
			var = ast_load_realtime("musiconhold", "name", chan->musicclass, NULL);
		}
		if (!var && !ast_strlen_zero(mclass))
			var = ast_load_realtime("musiconhold", "name", mclass, NULL);
		if (!var && !ast_strlen_zero(interpclass))
			var = ast_load_realtime("musiconhold", "name", interpclass, NULL);
		if (!var)
			var = ast_load_realtime("musiconhold", "name", "default", NULL);
		if (var && (mohclass = moh_class_malloc())) {
			mohclass->realtime = 1;
			for (tmp = var; tmp; tmp = tmp->next) {
				if (!strcasecmp(tmp->name, "name"))
					ast_copy_string(mohclass->name, tmp->value, sizeof(mohclass->name));
				else if (!strcasecmp(tmp->name, "mode"))
					ast_copy_string(mohclass->mode, tmp->value, sizeof(mohclass->mode)); 
				else if (!strcasecmp(tmp->name, "directory"))
					ast_copy_string(mohclass->dir, tmp->value, sizeof(mohclass->dir));
				else if (!strcasecmp(tmp->name, "application"))
					ast_copy_string(mohclass->args, tmp->value, sizeof(mohclass->args));
				else if (!strcasecmp(tmp->name, "digit") && (isdigit(*tmp->value) || strchr("*#", *tmp->value)))
					mohclass->digit = *tmp->value;
				else if (!strcasecmp(tmp->name, "random"))
					ast_set2_flag(mohclass, ast_true(tmp->value), MOH_RANDOMIZE);
				else if (!strcasecmp(tmp->name, "sort") && !strcasecmp(tmp->value, "random"))
					ast_set_flag(mohclass, MOH_RANDOMIZE);
				else if (!strcasecmp(tmp->name, "sort") && !strcasecmp(tmp->value, "alpha")) 
					ast_set_flag(mohclass, MOH_SORTALPHA);
				else if (!strcasecmp(tmp->name, "format")) {
					mohclass->format = ast_getformatbyname(tmp->value);
					if (!mohclass->format) {
						ast_log(LOG_WARNING, "Unknown format '%s' -- defaulting to SLIN\n", tmp->value);
						mohclass->format = AST_FORMAT_SLINEAR;
					}
				}
			}
			ast_variables_destroy(var);
			if (ast_strlen_zero(mohclass->dir)) {
				if (!strcasecmp(mohclass->mode, "custom")) {
					strcpy(mohclass->dir, "nodir");
				} else {
					ast_log(LOG_WARNING, "A directory must be specified for class '%s'!\n", mohclass->name);
					ast_free(mohclass);
					return -1;
				}
			}
			if (ast_strlen_zero(mohclass->mode)) {
				ast_log(LOG_WARNING, "A mode must be specified for class '%s'!\n", mohclass->name);
				ast_free(mohclass);
				return -1;
			}
			if (ast_strlen_zero(mohclass->args) && !strcasecmp(mohclass->mode, "custom")) {
				ast_log(LOG_WARNING, "An application must be specified for class '%s'!\n", mohclass->name);
				ast_free(mohclass);
				return -1;
			}

			if (ast_test_flag(global_flags, MOH_CACHERTCLASSES)) {
				/* CACHERTCLASSES enabled, let's add this class to default tree */
				if (state && state->class) {
					/* Class already exist for this channel */
					ast_log(LOG_NOTICE, "This channel already has a MOH class attached (%s)!\n", state->class->name);
					if (state->class->realtime && !ast_test_flag(global_flags, MOH_CACHERTCLASSES) && !strcasecmp(mohclass->name, state->class->name)) {
						/* we found RT class with the same name, seems like we should continue playing existing one */
						ast_moh_free_class(&mohclass);
						mohclass = state->class;
					}
				}
				moh_register(mohclass, 0);
			} else {

				/* We don't register RT moh class, so let's init it manualy */

				time(&mohclass->start);
				mohclass->start -= respawn_time;
	
				if (!strcasecmp(mohclass->mode, "files")) {
					if (!moh_scan_files(mohclass)) {
						ast_moh_free_class(&mohclass);
						return -1;
					}
					if (strchr(mohclass->args, 'r'))
						ast_set_flag(mohclass, MOH_RANDOMIZE);
				} else if (!strcasecmp(mohclass->mode, "mp3") || !strcasecmp(mohclass->mode, "mp3nb") || !strcasecmp(mohclass->mode, "quietmp3") || !strcasecmp(mohclass->mode, "quietmp3nb") || !strcasecmp(mohclass->mode, "httpmp3") || !strcasecmp(mohclass->mode, "custom")) {

					if (!strcasecmp(mohclass->mode, "custom"))
						ast_set_flag(mohclass, MOH_CUSTOM);
					else if (!strcasecmp(mohclass->mode, "mp3nb"))
						ast_set_flag(mohclass, MOH_SINGLE);
					else if (!strcasecmp(mohclass->mode, "quietmp3nb"))
						ast_set_flag(mohclass, MOH_SINGLE | MOH_QUIET);
					else if (!strcasecmp(mohclass->mode, "quietmp3"))
						ast_set_flag(mohclass, MOH_QUIET);
			
					mohclass->srcfd = -1;
#ifdef HAVE_DAHDI
					/* Open /dev/dahdi/pseudo for timing...  Is
					   there a better, yet reliable way to do this? */
					mohclass->pseudofd = open("/dev/dahdi/pseudo", O_RDONLY);
					if (mohclass->pseudofd < 0) {
						ast_log(LOG_WARNING, "Unable to open pseudo channel for timing...  Sound may be choppy.\n");
					} else {
						x = 320;
						ioctl(mohclass->pseudofd, DAHDI_SET_BLOCKSIZE, &x);
					}
#else
					mohclass->pseudofd = -1;
#endif
					/* Let's check if this channel already had a moh class before */
					if (state && state->class) {
						/* Class already exist for this channel */
						ast_log(LOG_NOTICE, "This channel already has a MOH class attached (%s)!\n", state->class->name);
						if (state->class->realtime && !ast_test_flag(global_flags, MOH_CACHERTCLASSES) && !strcasecmp(mohclass->name, state->class->name)) {
							/* we found RT class with the same name, seems like we should continue playing existing one */
							ast_moh_free_class(&mohclass);
							mohclass = state->class;
	
						}
					} else {
						if (ast_pthread_create_background(&mohclass->thread, NULL, monmp3thread, mohclass)) {
							ast_log(LOG_WARNING, "Unable to create moh...\n");
							if (mohclass->pseudofd > -1)
								close(mohclass->pseudofd);
							ast_moh_free_class(&mohclass);
							return -1;
						}
					}
				} else {
					ast_log(LOG_WARNING, "Don't know how to do a mode '%s' music on hold\n", mohclass->mode);
					ast_moh_free_class(&mohclass);
					return -1;
				}

			}

		} else if (var)
			ast_variables_destroy(var);
	}

	

	/* Requested MOH class not found, check for 'default' class in musiconhold.conf  */
	if (!mohclass) {
		AST_RWLIST_RDLOCK(&mohclasses);
		mohclass = get_mohbyname("default", 1);
		if (mohclass)
			ast_atomic_fetchadd_int(&mohclass->inuse, +1);
		AST_RWLIST_UNLOCK(&mohclasses);
	} else {
		AST_RWLIST_RDLOCK(&mohclasses);
		ast_atomic_fetchadd_int(&mohclass->inuse, +1);
		AST_RWLIST_UNLOCK(&mohclasses);
	}

	if (!mohclass)
		return -1;

	ast_set_flag(chan, AST_FLAG_MOH);
	if (mohclass->total_files) {
		return ast_activate_generator(chan, &moh_file_stream, mohclass);
	} else
		return ast_activate_generator(chan, &mohgen, mohclass);
}

static void local_ast_moh_stop(struct ast_channel *chan)
{
	struct moh_files_state *state = chan->music_state;
	ast_clear_flag(chan, AST_FLAG_MOH);
	ast_deactivate_generator(chan);

	if (state) {
		if (chan->stream) {
			ast_closestream(chan->stream);
			chan->stream = NULL;
		}
	}
}

static int load_moh_classes(int reload)
{
	struct ast_config *cfg;
	struct ast_variable *var;
	struct mohclass *class;	
	char *cat;
	int numclasses = 0;
	struct ast_flags config_flags = { reload ? CONFIG_FLAG_FILEUNCHANGED : 0 };

	cfg = ast_config_load("musiconhold.conf", config_flags);

	if (cfg == NULL || cfg == CONFIG_STATUS_FILEUNCHANGED)
		return 0;

	if (reload) {
		AST_RWLIST_WRLOCK(&mohclasses);
		AST_RWLIST_TRAVERSE(&mohclasses, class, list) {
			class->delete = 1;
		}
		AST_RWLIST_UNLOCK(&mohclasses);
	}
	
	ast_clear_flag(global_flags, AST_FLAGS_ALL);

	cat = ast_category_browse(cfg, NULL);
	for (; cat; cat = ast_category_browse(cfg, cat)) {
		/* Setup common options from [general] section */
		if (!strcasecmp(cat, "general")) {
			var = ast_variable_browse(cfg, cat);
			while (var) {
				if (!strcasecmp(var->name, "cachertclasses")) {
					ast_set2_flag(global_flags, ast_true(var->value), MOH_CACHERTCLASSES);
				} else {
					ast_log(LOG_WARNING, "Unknown option '%s' in [general] section of musiconhold.conf\n", var->name);
				}
				var = var->next;
			}
		}
		/* These names were deprecated in 1.4 and should not be used until after the next major release. */
		if (strcasecmp(cat, "classes") && strcasecmp(cat, "moh_files") && strcasecmp(cat, "general")) {
			if (!(class = moh_class_malloc()))
				break;

			ast_copy_string(class->name, cat, sizeof(class->name));	
			var = ast_variable_browse(cfg, cat);
			while (var) {
				if (!strcasecmp(var->name, "mode"))
					ast_copy_string(class->mode, var->value, sizeof(class->mode)); 
				else if (!strcasecmp(var->name, "directory"))
					ast_copy_string(class->dir, var->value, sizeof(class->dir));
				else if (!strcasecmp(var->name, "application"))
					ast_copy_string(class->args, var->value, sizeof(class->args));
				else if (!strcasecmp(var->name, "digit") && (isdigit(*var->value) || strchr("*#", *var->value)))
					class->digit = *var->value;
				else if (!strcasecmp(var->name, "random"))
					ast_set2_flag(class, ast_true(var->value), MOH_RANDOMIZE);
				else if (!strcasecmp(var->name, "sort") && !strcasecmp(var->value, "random"))
					ast_set_flag(class, MOH_RANDOMIZE);
				else if (!strcasecmp(var->name, "sort") && !strcasecmp(var->value, "alpha")) 
					ast_set_flag(class, MOH_SORTALPHA);
				else if (!strcasecmp(var->name, "format")) {
					class->format = ast_getformatbyname(var->value);
					if (!class->format) {
						ast_log(LOG_WARNING, "Unknown format '%s' -- defaulting to SLIN\n", var->value);
						class->format = AST_FORMAT_SLINEAR;
					}
				}
				var = var->next;
			}

			if (ast_strlen_zero(class->dir)) {
				if (!strcasecmp(class->mode, "custom")) {
					strcpy(class->dir, "nodir");
				} else {
					ast_log(LOG_WARNING, "A directory must be specified for class '%s'!\n", class->name);
					ast_free(class);
					continue;
				}
			}
			if (ast_strlen_zero(class->mode)) {
				ast_log(LOG_WARNING, "A mode must be specified for class '%s'!\n", class->name);
				ast_free(class);
				continue;
			}
			if (ast_strlen_zero(class->args) && !strcasecmp(class->mode, "custom")) {
				ast_log(LOG_WARNING, "An application must be specified for class '%s'!\n", class->name);
				ast_free(class);
				continue;
			}

			/* Don't leak a class when it's already registered */
			moh_register(class, reload);

			numclasses++;
		}
	}

	ast_config_destroy(cfg);

	return numclasses;
}

static int ast_moh_destroy_one(struct mohclass *moh)
{
	char buff[8192];
	int bytes, tbytes = 0, stime = 0, pid = 0;

	if (moh) {
		if (moh->pid > 1) {
			ast_debug(1, "killing %d!\n", moh->pid);
			stime = time(NULL) + 2;
			pid = moh->pid;
			moh->pid = 0;
			/* Back when this was just mpg123, SIGKILL was fine.  Now we need
			 * to give the process a reason and time enough to kill off its
			 * children. */
			kill(pid, SIGHUP);
			usleep(100000);
			kill(pid, SIGTERM);
			usleep(100000);
			kill(pid, SIGKILL);
			while ((ast_wait_for_input(moh->srcfd, 100) > 0) && (bytes = read(moh->srcfd, buff, 8192)) && time(NULL) < stime)
				tbytes = tbytes + bytes;
			ast_debug(1, "mpg123 pid %d and child died after %d bytes read\n", pid, tbytes);
			close(moh->srcfd);
		}
		ast_moh_free_class(&moh);
	}

	return 0;
}

static void ast_moh_destroy(void)
{
	struct mohclass *moh;

	ast_verb(2, "Destroying musiconhold processes\n");

	AST_RWLIST_WRLOCK(&mohclasses);
	while ((moh = AST_RWLIST_REMOVE_HEAD(&mohclasses, list))) {
		ast_moh_destroy_one(moh);
	}
	AST_RWLIST_UNLOCK(&mohclasses);
}

static char *handle_cli_moh_reload(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "moh reload";
		e->usage =
			"Usage: moh reload\n"
			"       Reloads the MusicOnHold module.\n"
			"       Alias for 'module reload res_musiconhold.so'\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc != e->args)
		return CLI_SHOWUSAGE;

	reload();

	return CLI_SUCCESS;
}

static char *handle_cli_moh_show_files(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	int i;
	struct mohclass *class;

	switch (cmd) {
	case CLI_INIT:
		e->command = "moh show files";
		e->usage =
			"Usage: moh show files\n"
			"       Lists all loaded file-based MusicOnHold classes and their\n"
			"       files.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc != e->args)
		return CLI_SHOWUSAGE;

	AST_RWLIST_RDLOCK(&mohclasses);
	AST_RWLIST_TRAVERSE(&mohclasses, class, list) {
		if (!class->total_files)
			continue;

		ast_cli(a->fd, "Class: %s\n", class->name);
		for (i = 0; i < class->total_files; i++)
			ast_cli(a->fd, "\tFile: %s\n", class->filearray[i]);
	}
	AST_RWLIST_UNLOCK(&mohclasses);

	return CLI_SUCCESS;
}

static char *handle_cli_moh_show_classes(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	struct mohclass *class;

	switch (cmd) {
	case CLI_INIT:
		e->command = "moh show classes";
		e->usage =
			"Usage: moh show classes\n"
			"       Lists all MusicOnHold classes.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc != e->args)
		return CLI_SHOWUSAGE;

	AST_RWLIST_RDLOCK(&mohclasses);
	AST_RWLIST_TRAVERSE(&mohclasses, class, list) {
		ast_cli(a->fd, "Class: %s\n", class->name);
		ast_cli(a->fd, "\tMode: %s\n", S_OR(class->mode, "<none>"));
		ast_cli(a->fd, "\tDirectory: %s\n", S_OR(class->dir, "<none>"));
		ast_cli(a->fd, "\tUse Count: %d\n", class->inuse);
		if (class->digit)
			ast_cli(a->fd, "\tDigit: %c\n", class->digit);
		if (ast_test_flag(class, MOH_CUSTOM))
			ast_cli(a->fd, "\tApplication: %s\n", S_OR(class->args, "<none>"));
		if (strcasecmp(class->mode, "files"))
			ast_cli(a->fd, "\tFormat: %s\n", ast_getformatname(class->format));
	}
	AST_RWLIST_UNLOCK(&mohclasses);

	return CLI_SUCCESS;
}

static struct ast_cli_entry cli_moh[] = {
	AST_CLI_DEFINE(handle_cli_moh_reload,       "Reload MusicOnHold"),
	AST_CLI_DEFINE(handle_cli_moh_show_classes, "List MusicOnHold classes"),
	AST_CLI_DEFINE(handle_cli_moh_show_files,   "List MusicOnHold file-based classes")
};

static int init_classes(int reload) 
{
	struct mohclass *moh;
    
	if (!load_moh_classes(reload)) 		/* Load classes from config */
		return 0;			/* Return if nothing is found */

	AST_RWLIST_WRLOCK(&mohclasses);
	AST_RWLIST_TRAVERSE_SAFE_BEGIN(&mohclasses, moh, list) {
		if (reload && moh->delete) {
			AST_RWLIST_REMOVE_CURRENT(list);
			if (!moh->inuse)
				ast_moh_destroy_one(moh);
		}
	}
	AST_RWLIST_TRAVERSE_SAFE_END
	AST_RWLIST_UNLOCK(&mohclasses);

	return 1;
}

static int load_module(void)
{
	int res;

	res = ast_register_application(play_moh, play_moh_exec, play_moh_syn, play_moh_desc);
	ast_register_atexit(ast_moh_destroy);
	ast_cli_register_multiple(cli_moh, sizeof(cli_moh) / sizeof(struct ast_cli_entry));
	if (!res)
		res = ast_register_application(wait_moh, wait_moh_exec, wait_moh_syn, wait_moh_desc);
	if (!res)
		res = ast_register_application(set_moh, set_moh_exec, set_moh_syn, set_moh_desc);
	if (!res)
		res = ast_register_application(start_moh, start_moh_exec, start_moh_syn, start_moh_desc);
	if (!res)
		res = ast_register_application(stop_moh, stop_moh_exec, stop_moh_syn, stop_moh_desc);

	if (!init_classes(0)) { 	/* No music classes configured, so skip it */
		ast_log(LOG_WARNING, "No music on hold classes configured, disabling music on hold.\n");
	} else {
		ast_install_music_functions(local_ast_moh_start, local_ast_moh_stop, local_ast_moh_cleanup);
	}

	return AST_MODULE_LOAD_SUCCESS;
}

static int reload(void)
{
	if (init_classes(1))
		ast_install_music_functions(local_ast_moh_start, local_ast_moh_stop, local_ast_moh_cleanup);

	return 0;
}

static int unload_module(void)
{
	int res = 0;
	struct mohclass *class = NULL;

	AST_RWLIST_WRLOCK(&mohclasses);
	AST_LIST_TRAVERSE(&mohclasses, class, list) {
		if (class->inuse > 0) {
			res = -1;
			break;
		}
	}
	AST_RWLIST_UNLOCK(&mohclasses);
	if (res < 0) {
		ast_log(LOG_WARNING, "Unable to unload res_musiconhold due to active MOH channels\n");
		return res;
	}

	ast_uninstall_music_functions();
	ast_moh_destroy();
	res = ast_unregister_application(play_moh);
	res |= ast_unregister_application(wait_moh);
	res |= ast_unregister_application(set_moh);
	res |= ast_unregister_application(start_moh);
	res |= ast_unregister_application(stop_moh);
	ast_cli_unregister_multiple(cli_moh, sizeof(cli_moh) / sizeof(struct ast_cli_entry));
	return res;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_DEFAULT, "Music On Hold Resource",
		.load = load_module,
		.unload = unload_module,
		.reload = reload,
	       );
