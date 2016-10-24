/*
 * This file is part of the KNOT Project
 *
 * Copyright (c) 2015, CESAR. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *    * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *    * Neither the name of the CESAR nor the
 *      names of its contributors may be used to endorse or promote products
 *      derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL CESAR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <sys/inotify.h>
#include <json-c/json.h>

#include "log.h"
#include "settings.h"
#include "manager.h"

#define BUF_LEN (sizeof(struct inotify_event))

static GMainLoop *main_loop;

static struct settings settings;

static const char *opt_cfg;
/*
 * Settings provided by command line have higher
 * priority than values read from config file.
 */
static unsigned int opt_port = 0;
static const char *opt_host = NULL;
/* Default is websockets */
static const char *opt_proto = "http";
static const char *opt_tty = NULL;

static void sig_term(int sig)
{
	g_main_loop_quit(main_loop);
}

static GOptionEntry options[] = {
	{ "config", 'f', 0, G_OPTION_ARG_STRING, &opt_cfg,
					"configuration file path", NULL },
	{ "host", 'h', 0, G_OPTION_ARG_STRING, &opt_host,
					"host", "Cloud server URL" },
	{ "port", 'p', 0, G_OPTION_ARG_INT, &opt_port,
					"port", "Cloud server port" },
	{ "proto", 'P', 0, G_OPTION_ARG_STRING, &opt_proto,
					"protocol", "eg: http or ws" },
	{ "tty", 't', 0, G_OPTION_ARG_STRING, &opt_tty,
					"TTY", "eg: /dev/ttyUSB0" },
	{ NULL },
};

static char *load_config(const char *file)
{
	int length;
	char *buffer;
	FILE *fl;

	fl = fopen(file, "r");
	if (fl == NULL) {
		LOG_ERROR("Failed to open file: %s", file);
		return NULL;
	}

	fseek(fl, 0, SEEK_END);
	length = ftell(fl);
	fseek(fl, 0, SEEK_SET);

	buffer = (char *) malloc((length + 1) * sizeof(char));
	if (buffer == NULL) {
		fclose(fl);
		return NULL;
	}

	if (fread(buffer, length, 1, fl) != 1) {
		free(buffer);
		fclose(fl);
		return NULL;
	}

	buffer[length] = '\0';

	fclose(fl);

	return buffer;
}

static int parse_config(const char *config, struct settings *settings)
{
	const char *uuid, *token, *tmp;
	json_object *jobj, *obj_cloud, *obj_tmp;
	int err = -EINVAL;

	jobj = json_tokener_parse(config);
	if (jobj == NULL)
		return -EINVAL;

	if (!json_object_object_get_ex(jobj, "cloud", &obj_cloud))
		goto done;

	if (!json_object_object_get_ex(obj_cloud, "uuid", &obj_tmp))
		goto done;

	uuid = json_object_get_string(obj_tmp);

	if (!json_object_object_get_ex(obj_cloud, "token", &obj_tmp))
		goto done;

	token = json_object_get_string(obj_tmp);

	if (settings->host == NULL) {
		if (!json_object_object_get_ex(obj_cloud, "serverName",
								 &obj_tmp))
			goto done;

		tmp = json_object_get_string(obj_tmp);
		settings->host = g_strdup(tmp);
	}

	if (settings->port == 0) {
		if (!json_object_object_get_ex(obj_cloud, "port", &obj_tmp))
			goto done;

		settings->port = json_object_get_int(obj_tmp);
	}

	settings->uuid = g_strdup(uuid);
	settings->token = g_strdup(token);

	err = 0; /* Success */

done:
	/* Free mem used in json parse: */
	json_object_put(jobj);
	return err;
}

static gboolean inotify_cb(GIOChannel *gio, GIOCondition condition,
								gpointer data)
{
	int inotifyFD = g_io_channel_unix_get_fd(gio);
	char buf[BUF_LEN];
	ssize_t numRead;
	const struct inotify_event *event;

	numRead = read(inotifyFD, buf, BUF_LEN);
	if (numRead == -1) {
		LOG_ERROR("Error read from inotify fd\n");
		return FALSE;
	}

	LOG_INFO("Read %ld bytes from inotify fd\n", (long) numRead);

	/* Process the events in buffer returned by read() */

	event = (struct inotify_event *) buf;
	if (event->mask & IN_MODIFY)
		g_main_loop_quit(main_loop);

	return TRUE;
}

int main(int argc, char *argv[])
{
	GOptionContext *context;
	GError *gerr = NULL;
	int err;
	char *json_str;
	GIOChannel *inotify_io;
	int inotifyFD, wd;
	guint watch_id;
	LOG_INFO("KNOT Gateway\n");

	context = g_option_context_new(NULL);
	g_option_context_add_main_entries(context, options, NULL);

	if (!g_option_context_parse(context, &argc, &argv, &gerr)) {
		LOG_ERROR("Invalid arguments: %s\n", gerr->message);
		g_error_free(gerr);
		g_option_context_free(context);
		return EXIT_FAILURE;
	}

	g_option_context_free(context);

	if (!opt_cfg) {
		LOG_ERROR("Missing KNOT configuration file!\n");
		return EXIT_FAILURE;
	}

	json_str = load_config(opt_cfg);
	if (json_str == NULL)
		return EXIT_FAILURE;

	memset(&settings, 0, sizeof(settings));
	settings.proto = "http";/* only supported protocol at moment */
	settings.tty = opt_tty;
	/*
	 * Command line options (host and port) have higher priority
	 * than values read from config file. UUID and Token should
	 * not be read from command line due security reason.
	 */

	if (opt_host)
		settings.host = g_strdup(opt_host);

	settings.port = opt_port;

	err = parse_config(json_str, &settings);
	free(json_str);
	if (err < 0)
		goto failure;

	err = manager_start(&settings);
	if (err < 0) {
		LOG_ERROR("start(): %s (%d)\n", strerror(-err), -err);
		goto failure;
	}
	/*
	 * TODO: implement a robust & clean way to reload settings
	 * instead of force quitting when configuration file changes.
	 */

	/* Starting inotify */
	inotifyFD = inotify_init();

	wd = inotify_add_watch(inotifyFD, opt_cfg, IN_MODIFY);
	if (wd == -1) {
		manager_stop();
		close(inotifyFD);
		LOG_ERROR("inotify_add_watch(): %s\n", opt_cfg);
		goto failure;
	}
	/* Setting gio channel to watch inotify fd */
	inotify_io = g_io_channel_unix_new(inotifyFD);
	watch_id = g_io_add_watch(inotify_io, G_IO_IN, inotify_cb, NULL);
	g_io_channel_set_close_on_unref(inotify_io, TRUE);
	g_io_channel_unref(inotify_io);

	/* Set user id to nobody */
	err = setuid(65534);
	LOG_INFO("Set user to nobody: %d\n", err);

	signal(SIGTERM, sig_term);
	signal(SIGINT, sig_term);
	signal(SIGPIPE, SIG_IGN);

	main_loop = g_main_loop_new(NULL, FALSE);

	g_main_loop_run(main_loop);

	g_main_loop_unref(main_loop);

	/* inotify cleanup */
	g_source_remove(watch_id);
	inotify_rm_watch(inotifyFD, wd);

	manager_stop();
	g_free(settings.host);
	g_free(settings.uuid);
	g_free(settings.token);

	LOG_INFO("Exiting\n");

	return EXIT_SUCCESS;

failure:
	g_free(settings.host);
	g_free(settings.uuid);
	g_free(settings.token);

	return EXIT_FAILURE;
}
