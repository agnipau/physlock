/* physlock: main.c
 * Copyright (c) 2013 Bert Muennich <be.muennich at gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <syslog.h>

#include "auth.h"
#include "config.h"
#include "options.h"
#include "util.h"
#include "vt.h"

static char buf[1024];
static int oldvt;
static vt_t vt;
static int oldsysrq;
static int oldprintk;

void cleanup() {
	if (oldsysrq > 0)
		write_int_to_file(SYSRQ_PATH, oldsysrq);
	if (oldprintk > 1)
		write_int_to_file(PRINTK_PATH, oldprintk);
	if (vt.fd >= 0)
		reset_vt(&vt);
	lock_vt_switch(0);
	release_vt(&vt, oldvt);
	vt_destroy();
	closelog();
	memset(buf, 0, sizeof(buf));
}

void sa_handler_exit(int signum) {
	exit(0);
}

void setup_signal(int signum, void (*handler)(int)) {
	struct sigaction sigact;

	sigact.sa_flags = 0;
	sigact.sa_handler = handler;
	sigemptyset(&sigact.sa_mask);
	
	if (sigaction(signum, &sigact, NULL) < 0)
		error(0, errno, "signal %d", signum);
}

void prompt(FILE *stream, const char *fmt, ...) {
	va_list args;
	unsigned int c, i = 0;

	va_start(args, fmt);
	vfprintf(stream, fmt, args);
	va_end(args);

	while ((c = fgetc(stream)) != EOF && c != '\n') {
		if (c != '\0' && i + 1 < sizeof(buf))
			buf[i++] = (char) c;
	}
	if (ferror(stream))
		error(EXIT_FAILURE, 0, "Error reading from console");
	buf[i] = '\0';
}

int main(int argc, char **argv) {
	int try = 0, unauth = 1, user_only = 1;
	userinfo_t root, user, *u = &user;

	oldvt = oldsysrq = oldprintk = vt.nr = vt.fd = -1;
	vt.ios = NULL;

	parse_options(argc, argv);

	if (geteuid() != 0)
		error(EXIT_FAILURE, 0, "Must be root!");

	setup_signal(SIGTERM, sa_handler_exit);
	setup_signal(SIGQUIT, sa_handler_exit);
	setup_signal(SIGHUP, SIG_IGN);
	setup_signal(SIGINT, SIG_IGN);
	setup_signal(SIGUSR1, SIG_IGN);
	setup_signal(SIGUSR2, SIG_IGN);

	close(0);
	close(1);

	openlog(progname, LOG_PID, LOG_AUTH);

	vt_init();
	get_current_vt(&oldvt);

	if (options->lock_switch != -1) {
		if (lock_vt_switch(options->lock_switch) == -1)
			exit(EXIT_FAILURE);
		vt_destroy();
		return 0;
	}

	get_user(&user, oldvt);
	if (authenticate(&user, "") == -1)
		error(EXIT_FAILURE, 0, "Error hashing password for user %s", user.name);
	get_root(&root);
	if (strcmp(user.name, root.name) != 0 && authenticate(&root, "") != -1)
		user_only = 0;

	if (options->disable_sysrq) {
		oldsysrq = read_int_from_file(SYSRQ_PATH, '\n');
		if (oldsysrq > 0)
			if (write_int_to_file(SYSRQ_PATH, 0) == -1)
				exit(EXIT_FAILURE);
	}

	if (options->mute_kernel_messages) {
		oldprintk = read_int_from_file(PRINTK_PATH, '\t');
		if (oldprintk > 1)
			if (write_int_to_file(PRINTK_PATH, 1) == -1)
				exit(EXIT_FAILURE);
	}

	acquire_new_vt(&vt);
	lock_vt_switch(1);

	atexit(cleanup);

	if (options->detach) {
		int chpid = fork();
		if (chpid < 0) {
			error(EXIT_FAILURE, errno, "fork");
		} else if (chpid > 0) {
			return 0;
		} else {
			setsid();
			sleep(1); /* w/o this, accessing the vt might fail */
			reopen_vt(&vt);
		}
	}
	secure_vt(&vt);

	while (unauth) {
		flush_vt(&vt);
		prompt(vt.ios, "%s's password: ", u->name);
		unauth = authenticate(u, buf);
		memset(buf, 0, sizeof(buf));
		if (unauth) {
			if (!user_only && (u == &root || ++try == 3)) {
				u = u == &root ? &user : &root;
				try = 0;
			}
			fprintf(vt.ios, "\nAuthentication failed\n\n");
			syslog(LOG_WARNING, "Authentication failure");
			sleep(AUTH_FAIL_TIMEOUT);
		}
	}

	return 0;
}
