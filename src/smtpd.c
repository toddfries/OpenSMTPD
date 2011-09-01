/*	$OpenBSD: smtpd.c,v 1.129 2011/08/29 21:43:09 chl Exp $	*/

/*
 * Copyright (c) 2008 Gilles Chehade <gilles@openbsd.org>
 * Copyright (c) 2008 Pierre-Yves Ritschard <pyr@openbsd.org>
 * Copyright (c) 2009 Jacek Masiulaniec <jacekm@dobremiasto.net>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/* define _GNU_SOURCE for asprintf */
#define _GNU_SOURCE

#include <sys/file.h> /* Needed for flock */
#include <sys/types.h>
#include "sys-queue.h"
#include "sys-tree.h"
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/mman.h>

#include <err.h>
#include <errno.h>
#include <event.h>
#include <fcntl.h>
#include <grp.h> /* needed for setgroups */
#ifdef HAVE_LOGIN_CAP_H
#include <login_cap.h>
#endif
#include "imsg.h"
#include <paths.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "smtpd.h"
#include "log.h"

static void parent_imsg(struct imsgev *, struct imsg *);
static void usage(void);
static void parent_shutdown(void);
static void parent_send_config(int, short, void *);
static void parent_send_config_listeners(void);
static void parent_send_config_client_certs(void);
static void parent_send_config_ruleset(int);
static void parent_sig_handler(int, short, void *);
static void forkmda(struct imsgev *, u_int32_t, struct deliver *);
static int parent_enqueue_offline(char *);
static int parent_forward_open(char *);
static int path_starts_with(char *, char *);
static void fork_peers(void);
static struct child *child_lookup(pid_t);
static struct child *child_add(pid_t, int, int);
static void child_del(pid_t);

static int	queueing_add(char *);
static void	queueing_done(void);

struct queueing {
	TAILQ_ENTRY(queueing)	 entry;
	char			*path;
};

#define QUEUEING_MAX 5
static size_t			queueing_running = 0;
TAILQ_HEAD(, queueing)		queueing_q;

extern char	**environ;
void		(*imsg_callback)(struct imsgev *, struct imsg *);

struct smtpd	*env = NULL;

int __b64_pton(char const *, unsigned char *, size_t);

/* Saved arguments to main(). */
char **saved_argv;
int saved_argc;

static void
parent_imsg(struct imsgev *iev, struct imsg *imsg)
{
	struct forward_req	*fwreq;
	struct auth		*auth;
	struct auth_backend	*auth_backend;
	int			 fd;

	if (iev->proc == PROC_SMTP) {
		switch (imsg->hdr.type) {
		case IMSG_PARENT_SEND_CONFIG:
			log_debug("parent_imsg: PROC_SMTP->IMSG_PARENT_SEND_CONFIG");
			parent_send_config_listeners();
			return;

		case IMSG_PARENT_AUTHENTICATE:
			log_debug("parent_imsg: PROC_SMTP->IMSG_PARENT_AUTHENTICATE");
			auth_backend = auth_backend_lookup(AUTH_BSD);
			auth = imsg->data;
			auth->success = auth_backend->authenticate(auth->user,
			    auth->pass);
			imsg_compose_event(iev, IMSG_PARENT_AUTHENTICATE, 0, 0,
			    -1, auth, sizeof *auth);
			return;
		}
	}

	if (iev->proc == PROC_LKA) {
		switch (imsg->hdr.type) {
		case IMSG_PARENT_FORWARD_OPEN:
			log_debug("parent_imsg: PROC_LKA->IMSG_PARENT_FORWARD_OPEN");
			fwreq = imsg->data;
			fd = parent_forward_open(fwreq->as_user);
			fwreq->status = 0;
			if (fd == -2) {
				/* no ~/.forward, however it's optional. */
				fwreq->status = 1;
				fd = -1;
			} else if (fd != -1)
				fwreq->status = 1;
			imsg_compose_event(iev, IMSG_PARENT_FORWARD_OPEN, 0, 0,
			    fd, fwreq, sizeof *fwreq);
			return;
		}
	}

	if (iev->proc == PROC_QUEUE) {
		switch (imsg->hdr.type) {
		case IMSG_PARENT_ENQUEUE_OFFLINE:
			log_debug("parent_imsg: PROC_QUEUE->IMSG_PARENT_ENQUEUE_OFFLINE");
			if (! queueing_add(imsg->data))
				imsg_compose_event(iev,
				    IMSG_PARENT_ENQUEUE_OFFLINE, 0, 0, -1,
				    NULL, 0);
			return;
		}
	}

	if (iev->proc == PROC_MDA) {
		switch (imsg->hdr.type) {
		case IMSG_PARENT_FORK_MDA:
			log_debug("parent_imsg: PROC_MDA->IMSG_PARENT_FORK_MDA");
			forkmda(iev, imsg->hdr.peerid, imsg->data);
			return;
		}
	}

	if (iev->proc == PROC_CONTROL) {
		switch (imsg->hdr.type) {
		case IMSG_CTL_VERBOSE:
			log_verbose(*(int *)imsg->data);

			/* forward to other processes */
			imsg_compose_event(env->sc_ievs[PROC_LKA], IMSG_CTL_VERBOSE,
	    		    0, 0, -1, imsg->data, sizeof(int));
			imsg_compose_event(env->sc_ievs[PROC_MDA], IMSG_CTL_VERBOSE,
	    		    0, 0, -1, imsg->data, sizeof(int));
			imsg_compose_event(env->sc_ievs[PROC_MFA], IMSG_CTL_VERBOSE,
	    		    0, 0, -1, imsg->data, sizeof(int));
			imsg_compose_event(env->sc_ievs[PROC_MTA], IMSG_CTL_VERBOSE,
	    		    0, 0, -1, imsg->data, sizeof(int));
			imsg_compose_event(env->sc_ievs[PROC_QUEUE], IMSG_CTL_VERBOSE,
	    		    0, 0, -1, imsg->data, sizeof(int));
			imsg_compose_event(env->sc_ievs[PROC_SMTP], IMSG_CTL_VERBOSE,
	    		    0, 0, -1, imsg->data, sizeof(int));
			return;
		}
	}

	fatalx("parent_imsg: unexpected imsg");
}

static void
usage(void)
{
	extern char	*__progname;

	fprintf(stderr, "usage: %s [-dnv] [-D macro=value] "
	    "[-f file]\n", __progname);
	exit(1);
}

static void
parent_shutdown(void)
{
	struct child	*child;
	pid_t		 pid;

	SPLAY_FOREACH(child, childtree, &env->children)
		if (child->type == CHILD_DAEMON)
			kill(child->pid, SIGTERM);

	do {
		pid = waitpid(WAIT_MYPGRP, NULL, 0);
	} while (pid != -1 || (pid == -1 && errno == EINTR));

#ifdef VALGRIND
	child_free();
	purge_config(PURGE_EVERYTHING);
	free_pipes();
	free_peers();
	clean_setproctitle();
#endif

	log_warnx("parent terminating");
	exit(0);
}

static void
parent_send_config(int fd, short event, void *p)
{
	parent_send_config_listeners();
	parent_send_config_client_certs();
	parent_send_config_ruleset(PROC_MFA);
	parent_send_config_ruleset(PROC_LKA);
}

static void
parent_send_config_listeners(void)
{
	struct listener		*l;
	struct ssl		*s;
	struct iovec		 iov[4];
	int			 opt;

	log_debug("parent_send_config: configuring smtp");
	imsg_compose_event(env->sc_ievs[PROC_SMTP], IMSG_CONF_START,
	    0, 0, -1, NULL, 0);

	SPLAY_FOREACH(s, ssltree, env->sc_ssl) {
		if (!(s->flags & F_SCERT))
			continue;

		iov[0].iov_base = s;
		iov[0].iov_len = sizeof(*s);
		iov[1].iov_base = s->ssl_cert;
		iov[1].iov_len = s->ssl_cert_len;
		iov[2].iov_base = s->ssl_key;
		iov[2].iov_len = s->ssl_key_len;
		iov[3].iov_base = s->ssl_dhparams;
		iov[3].iov_len = s->ssl_dhparams_len;

		imsg_composev(&env->sc_ievs[PROC_SMTP]->ibuf,
		    IMSG_CONF_SSL, 0, 0, -1, iov, nitems(iov));
		imsg_event_add(env->sc_ievs[PROC_SMTP]);
	}

	TAILQ_FOREACH(l, env->sc_listeners, entry) {
		struct sockaddr_storage lss = l->ss;

		if ((l->fd = socket(l->ss.ss_family, SOCK_STREAM, 0)) == -1)
			fatal("socket");
		opt = 1;
# ifdef SO_REUSEADDR
		if (setsockopt(l->fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
			fatal("setsockopt");
#else
		if (setsockopt(l->fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0)
			fatal("setsockopt");
#endif
		if (bind(l->fd, (struct sockaddr *)&l->ss, SS_LEN(lss)) == -1)
			fatal("bind");
		imsg_compose_event(env->sc_ievs[PROC_SMTP], IMSG_CONF_LISTENER,
		    0, 0, l->fd, l, sizeof(*l));
	}

	imsg_compose_event(env->sc_ievs[PROC_SMTP], IMSG_CONF_END,
	    0, 0, -1, NULL, 0);
}

static void
parent_send_config_client_certs(void)
{
	struct ssl		*s;
	struct iovec		 iov[4];

	log_debug("parent_send_config_client_certs: configuring smtp");
	imsg_compose_event(env->sc_ievs[PROC_MTA], IMSG_CONF_START,
	    0, 0, -1, NULL, 0);

	SPLAY_FOREACH(s, ssltree, env->sc_ssl) {
		if (!(s->flags & F_CCERT))
			continue;

		iov[0].iov_base = s;
		iov[0].iov_len = sizeof(*s);
		iov[1].iov_base = s->ssl_cert;
		iov[1].iov_len = s->ssl_cert_len;
		iov[2].iov_base = s->ssl_key;
		iov[2].iov_len = s->ssl_key_len;
		iov[3].iov_base = s->ssl_dhparams;
		iov[3].iov_len = s->ssl_dhparams_len;

		imsg_composev(&env->sc_ievs[PROC_MTA]->ibuf, IMSG_CONF_SSL,
		    0, 0, -1, iov, nitems(iov));
		imsg_event_add(env->sc_ievs[PROC_MTA]);
	}

	imsg_compose_event(env->sc_ievs[PROC_MTA], IMSG_CONF_END,
	    0, 0, -1, NULL, 0);
}

void
parent_send_config_ruleset(int proc)
{
	struct rule		*r;
	struct map		*m;
	struct mapel		*mapel;
	struct filter		*f;
	
	log_debug("parent_send_config_ruleset: reloading rules and maps");
	imsg_compose_event(env->sc_ievs[proc], IMSG_CONF_START,
	    0, 0, -1, NULL, 0);

	if (proc == PROC_MFA) {
		TAILQ_FOREACH(f, env->sc_filters, f_entry) {
			imsg_compose_event(env->sc_ievs[proc], IMSG_CONF_FILTER,
			    0, 0, -1, f, sizeof(*f));
		}
	}
	else {
		TAILQ_FOREACH(m, env->sc_maps, m_entry) {
			imsg_compose_event(env->sc_ievs[proc], IMSG_CONF_MAP,
			    0, 0, -1, m, sizeof(*m));
			TAILQ_FOREACH(mapel, &m->m_contents, me_entry) {
			imsg_compose_event(env->sc_ievs[proc], IMSG_CONF_MAP_CONTENT,
			    0, 0, -1, mapel, sizeof(*mapel));
			}
		}
	
		TAILQ_FOREACH(r, env->sc_rules, r_entry) {
			imsg_compose_event(env->sc_ievs[proc], IMSG_CONF_RULE,
			    0, 0, -1, r, sizeof(*r));
			imsg_compose_event(env->sc_ievs[proc], IMSG_CONF_RULE_SOURCE,
			    0, 0, -1, &r->r_sources->m_name, sizeof(r->r_sources->m_name));
		}
	}
	
	imsg_compose_event(env->sc_ievs[proc], IMSG_CONF_END,
	    0, 0, -1, NULL, 0);
	log_debug("parent_send_config_ruleset: reloading rules and maps: end");
}

static void
parent_sig_handler(int sig, short event, void *p)
{
	struct child	*child;
	int		 die = 0, status, fail;
	pid_t		 pid;
	char		*cause;

	switch (sig) {
	case SIGTERM:
	case SIGINT:
		die = 1;
		/* FALLTHROUGH */
	case SIGCHLD:
		do {
			pid = waitpid(-1, &status, WNOHANG);
			if (pid <= 0)
				continue;

			child = child_lookup(pid);
			if (child == NULL)
				fatalx("unexpected SIGCHLD");

			fail = 0;
			if (WIFSIGNALED(status)) {
				fail = 1;
				asprintf(&cause, "terminated; signal %d",
				    WTERMSIG(status));
			} else if (WIFEXITED(status)) {
				if (WEXITSTATUS(status) != 0) {
					fail = 1;
					asprintf(&cause, "exited abnormally");
				} else
					asprintf(&cause, "exited okay");
			} else
				fatalx("unexpected cause of SIGCHLD");

			switch (child->type) {
			case CHILD_DAEMON:
				die = 1;
				if (fail)
					log_warnx("lost child: %s %s",
					    env->sc_title[child->title], cause);
				break;

			case CHILD_MDA:
				if (WIFSIGNALED(status) &&
				    WTERMSIG(status) == SIGALRM) {
					free(cause);
					asprintf(&cause, "terminated; timeout");
				}
				imsg_compose_event(env->sc_ievs[PROC_MDA],
				    IMSG_MDA_DONE, child->mda_id, 0,
				    child->mda_out, cause, strlen(cause) + 1);
				break;

			case CHILD_ENQUEUE_OFFLINE:
				if (fail)
					log_warnx("couldn't enqueue offline "
					    "message; smtpctl %s", cause);
				else
					log_debug("offline message enqueued");
				imsg_compose_event(env->sc_ievs[PROC_QUEUE],
				    IMSG_PARENT_ENQUEUE_OFFLINE, 0, 0, -1,
				    NULL, 0);
				queueing_done();
				break;

			default:
				fatalx("unexpected child type");
			}

			child_del(child->pid);
			free(cause);
		} while (pid > 0 || (pid == -1 && errno == EINTR));

		if (die)
			parent_shutdown();
		break;
	default:
		fatalx("unexpected signal");
	}
}

int
main(int argc, char *argv[])
{
	int		 c, i;
	int		 debug, verbose;
	int		 opts;
	const char	*conffile = CONF_FILE;
	struct smtpd	 smtpd;
	struct event	 ev_sigint;
	struct event	 ev_sigterm;
	struct event	 ev_sigchld;
	struct event	 ev_sighup;
	struct timeval	 tv;
	struct peer	 peers[] = {
		{ PROC_CONTROL,	imsg_dispatch },
		{ PROC_LKA,	imsg_dispatch },
		{ PROC_MDA,	imsg_dispatch },
		{ PROC_MFA,	imsg_dispatch },
		{ PROC_MTA,	imsg_dispatch },
		{ PROC_SMTP,	imsg_dispatch },
		{ PROC_QUEUE,	imsg_dispatch }
	};

	init_rng();

	/* Save argv. Duplicate so setproctitle emulation doesn't clobber it */
	saved_argc = argc;
	saved_argv = xcalloc(argc + 1, sizeof(*saved_argv));
	for (i = 0; i < argc; i++)
		saved_argv[i] = xstrdup(argv[i]);
	saved_argv[i] = NULL;

#ifndef HAVE_SETPROCTITLE
	/* Prepare for later setproctitle emulation */
	compat_init_setproctitle(argc, argv);
	argv = saved_argv;
#endif

	env = &smtpd;

	opts = 0;
	debug = 0;
	verbose = 0;

	log_init(1);

	TAILQ_INIT(&queueing_q);

	while ((c = getopt(argc, argv, "dD:nf:v")) != -1) {
		switch (c) {
		case 'd':
			debug = 2;
			verbose = 1;
			break;
		case 'D':
			if (cmdline_symset(optarg) < 0)
				log_warnx("could not parse macro definition %s",
				    optarg);
			break;
		case 'n':
			debug = 2;
			opts |= SMTPD_OPT_NOACTION;
			break;
		case 'f':
			conffile = optarg;
			break;
		case 'v':
			verbose = 1;
			opts |= SMTPD_OPT_VERBOSE;
			break;
		default:
			usage();
		}
	}

	argv += optind;
	argc -= optind;

	if (parse_config(&smtpd, conffile, opts))
		exit(1);

	seed_rng();

	if (strlcpy(env->sc_conffile, conffile, MAXPATHLEN) >= MAXPATHLEN)
		errx(1, "config file exceeds MAXPATHLEN");


	if (env->sc_opts & SMTPD_OPT_NOACTION) {
		fprintf(stderr, "configuration OK\n");
		exit(0);
	}

	/* check for root privileges */
	if (geteuid())
		errx(1, "need root privileges");

	if ((env->sc_pw =  getpwnam(SMTPD_USER)) == NULL)
		errx(1, "unknown user %s", SMTPD_USER);

	env->sc_queue = queue_backend_lookup(QT_FS);
	if (env->sc_queue == NULL)
		errx(1, "could not find queue backend");

	if (!env->sc_queue->init())
		errx(1, "invalid directory permissions");

	log_init(debug);
	log_verbose(verbose);

	if (!debug)
		if (daemon(0, 0) == -1)
			err(1, "failed to daemonize");

	log_info("startup%s", (debug > 1)?" [debug mode]":"");

	if (env->sc_hostname[0] == '\0')
		errx(1, "machine does not have a hostname set");

	env->stats = mmap(NULL, sizeof(struct stats), PROT_WRITE|PROT_READ,
	    MAP_ANON|MAP_SHARED, -1, (off_t)0);
	if (env->stats == MAP_FAILED)
		fatal("mmap");
	bzero(env->stats, sizeof(struct stats));

	env->stats->parent.start = time(NULL);

	fork_peers();

	imsg_callback = parent_imsg;
	event_init();

	signal_set(&ev_sigint, SIGINT, parent_sig_handler, NULL);
	signal_set(&ev_sigterm, SIGTERM, parent_sig_handler, NULL);
	signal_set(&ev_sigchld, SIGCHLD, parent_sig_handler, NULL);
	signal_set(&ev_sighup, SIGHUP, parent_sig_handler, NULL);
	signal_add(&ev_sigint, NULL);
	signal_add(&ev_sigterm, NULL);
	signal_add(&ev_sigchld, NULL);
	signal_add(&ev_sighup, NULL);
	signal(SIGPIPE, SIG_IGN);

	config_pipes(peers, nitems(peers));
	config_peers(peers, nitems(peers));

	evtimer_set(&env->sc_ev, parent_send_config, NULL);
	bzero(&tv, sizeof(tv));
	evtimer_add(&env->sc_ev, &tv);

	log_debug("libevent %s (%s)", event_get_version(), event_get_method());
	log_debug("parent: event_dispatch");
	if (event_dispatch() < 0)
		fatal("event_dispatch");
	
	return (0);
}

static void
fork_peers(void)
{
	SPLAY_INIT(&env->children);

	/*
	 * Pick descriptor limit that will guarantee impossibility of fd
	 * starvation condition.  The logic:
	 *
	 * Treat hardlimit as 100%.
	 * Limit smtp to 50% (inbound connections)
	 * Limit mta to 50% (outbound connections)
	 * Limit mda to 50% (local deliveries)
	 * In all three above, compute max session limit by halving the fd
	 * limit (50% -> 25%), because each session costs two fds.
	 * Limit queue to 100% to cover the extreme case when tons of fds are
	 * opened for all four possible purposes (smtp, mta, mda, bounce)
	 */
	fdlimit(0.5);

	env->sc_instances[PROC_CONTROL] = 1;
	env->sc_instances[PROC_LKA] = 1;
	env->sc_instances[PROC_MDA] = 1;
	env->sc_instances[PROC_MFA] = 1;
	env->sc_instances[PROC_MTA] = 1;
	env->sc_instances[PROC_PARENT] = 1;
	env->sc_instances[PROC_QUEUE] = 1;
	env->sc_instances[PROC_RUNNER] = 1;
	env->sc_instances[PROC_SMTP] = 1;

	init_pipes();

	env->sc_title[PROC_CONTROL] = "control";
	env->sc_title[PROC_LKA] = "lookup agent";
	env->sc_title[PROC_MDA] = "mail delivery agent";
	env->sc_title[PROC_MFA] = "mail filter agent";
	env->sc_title[PROC_MTA] = "mail transfer agent";
	env->sc_title[PROC_QUEUE] = "queue";
	env->sc_title[PROC_RUNNER] = "runner";
	env->sc_title[PROC_SMTP] = "smtp server";

	child_add(control(), CHILD_DAEMON, PROC_CONTROL);
	child_add(lka(), CHILD_DAEMON, PROC_LKA);
	child_add(mda(), CHILD_DAEMON, PROC_MDA);
	child_add(mfa(), CHILD_DAEMON, PROC_MFA);
	child_add(mta(), CHILD_DAEMON, PROC_MTA);
	child_add(queue(), CHILD_DAEMON, PROC_QUEUE);
	child_add(runner(), CHILD_DAEMON, PROC_RUNNER);
	child_add(smtp(), CHILD_DAEMON, PROC_SMTP);

	setproctitle("[priv]");
}

struct child *
child_add(pid_t pid, int type, int title)
{
	struct child	*child;

	if ((child = calloc(1, sizeof(*child))) == NULL)
		fatal(NULL);

	child->pid = pid;
	child->type = type;
	child->title = title;

	if (SPLAY_INSERT(childtree, &env->children, child) != NULL)
		fatalx("child_add: double insert");

	return (child);
}

static void
child_del(pid_t pid)
{
	struct child	*p;

	p = child_lookup(pid);
	if (p == NULL)
		fatalx("child_del: unknown child");

	if (SPLAY_REMOVE(childtree, &env->children, p) == NULL)
		fatalx("child_del: tree remove failed");
	free(p);
}

static struct child *
child_lookup(pid_t pid)
{
	struct child	 key;

	key.pid = pid;
	return SPLAY_FIND(childtree, &env->children, &key);
}

void child_free(void)
{
	struct child	*child;

	while ((child = SPLAY_ROOT(&env->children)) != NULL)
		child_del(child->pid);
}

void
imsg_event_add(struct imsgev *iev)
{
	int err;

	if (iev->handler == NULL) {
		log_debug("imsg_event_add: iev->handler=NULL");
		imsg_flush(&iev->ibuf);
		return;
	}

	iev->events = EV_READ;
	if (iev->ibuf.w.queued)
		iev->events |= EV_WRITE;

	if ((err = event_del(&iev->ev)) != 0)
		fatal("imsg_event_add: event_del");
	event_set(&iev->ev, iev->ibuf.fd, iev->events, iev->handler, iev->data);
	if ((err = event_add(&iev->ev, NULL)) != 0)
		fatal("imsg_event_add: event_add");
}

void
imsg_compose_event(struct imsgev *iev, u_int16_t type, u_int32_t peerid,
    pid_t pid, int fd, void *data, u_int16_t datalen)
{
	int	ret;

	if ((ret = imsg_compose(&iev->ibuf, type, peerid,
				pid, fd, data, datalen)) != -1) {
		imsg_event_add(iev);
	} else {
		log_debug("imsg_compose_event ret=%d", ret);
		
		fatal("imsg_compose_event");
	}
}

static void
forkmda(struct imsgev *iev, u_int32_t id,
    struct deliver *deliver)
{
	char		 ebuf[128], sfn[32];
	struct user_backend *ub;
	struct user u;
	struct child	*child;
	pid_t		 pid;
	int		 n, allout, pipefd[2];

	log_debug("forkmda: to %s as %s", deliver->to, deliver->user);

	bzero(&u, sizeof (u));
	ub = user_backend_lookup(USER_GETPWNAM);
	errno = 0;
	if (! ub->getbyname(&u, deliver->user)) {
		n = snprintf(ebuf, sizeof ebuf, "getpwnam: %s",
		    errno ? strerror(errno) : "no such user");
		imsg_compose_event(iev, IMSG_MDA_DONE, id, 0, -1, ebuf, n + 1);
		return;
	}

	/* lower privs early to allow fork fail due to ulimit */
	if (seteuid(u.uid) < 0)
		fatal("cannot lower privileges");

	if (pipe(pipefd) < 0) {
		n = snprintf(ebuf, sizeof ebuf, "pipe: %s", strerror(errno));
		if (seteuid(0) < 0)
			fatal("forkmda: cannot restore privileges");
		imsg_compose_event(iev, IMSG_MDA_DONE, id, 0, -1, ebuf, n + 1);
		return;
	}

	/* prepare file which captures stdout and stderr */
	strlcpy(sfn, "/tmp/smtpd.out.XXXXXXXXXXX", sizeof(sfn));
	allout = mkstemp(sfn);
	if (allout < 0) {
		n = snprintf(ebuf, sizeof ebuf, "mkstemp: %s", strerror(errno));
		if (seteuid(0) < 0)
			fatal("forkmda: cannot restore privileges");
		imsg_compose_event(iev, IMSG_MDA_DONE, id, 0, -1, ebuf, n + 1);
		close(pipefd[0]);
		close(pipefd[1]);
		return;
	}
	unlink(sfn);

	pid = fork();
	if (pid < 0) {
		n = snprintf(ebuf, sizeof ebuf, "fork: %s", strerror(errno));
		if (seteuid(0) < 0)
			fatal("forkmda: cannot restore privileges");
		imsg_compose_event(iev, IMSG_MDA_DONE, id, 0, -1, ebuf, n + 1);
		close(pipefd[0]);
		close(pipefd[1]);
		close(allout);
		return;
	}

	/* parent passes the child fd over to mda */
	if (pid > 0) {
		if (seteuid(0) < 0)
			fatal("forkmda: cannot restore privileges");
		child = child_add(pid, CHILD_MDA, -1);
		child->mda_out = allout;
		child->mda_id = id;
		close(pipefd[0]);
		imsg_compose_event(iev, IMSG_PARENT_FORK_MDA, id, 0, pipefd[1],
		    NULL, 0);
		return;
	}

#define error(m) { perror(m); _exit(1); }
	if (seteuid(0) < 0)
		error("forkmda: cannot restore privileges");
	if (chdir(u.directory) < 0 && chdir("/") < 0)
		error("chdir");
	if (dup2(pipefd[0], STDIN_FILENO) < 0 ||
	    dup2(allout, STDOUT_FILENO) < 0 ||
	    dup2(allout, STDERR_FILENO) < 0)
		error("forkmda: dup2");
	if (closefrom(STDERR_FILENO + 1) < 0)
		error("closefrom");
	if (setgroups(1, &u.gid) ||
	    setresgid(u.gid, u.gid, u.gid) ||
	    setresuid(u.uid, u.uid, u.uid))
		error("forkmda: cannot drop privileges");
	if (setsid() < 0)
		error("setsid");
	if (signal(SIGPIPE, SIG_DFL) == SIG_ERR ||
	    signal(SIGINT, SIG_DFL) == SIG_ERR ||
	    signal(SIGTERM, SIG_DFL) == SIG_ERR ||
	    signal(SIGCHLD, SIG_DFL) == SIG_ERR ||
	    signal(SIGHUP, SIG_DFL) == SIG_ERR)
		error("signal");

	/* avoid hangs by setting 5m timeout */
	alarm(300);

	if (deliver->mode == A_EXT) {
		char	*environ_new[2];

		environ_new[0] = "PATH=" _PATH_DEFPATH;
		environ_new[1] = (char *)NULL;
		environ = environ_new;
		execle("/bin/sh", "/bin/sh", "-c", deliver->to, (char *)NULL,
		    environ_new);
		error("execle");
	}

	if (deliver->mode == A_MAILDIR) {
		char	 tmp[PATH_MAX], new[PATH_MAX];
		int	 ch, fd;
		FILE	*fp;

#define error2(m) { n = errno; unlink(tmp); errno = n; error(m); }
		setproctitle("maildir delivery");
		if (mkdir(deliver->to, 0700) < 0 && errno != EEXIST)
			error("cannot mkdir maildir");
		if (chdir(deliver->to) < 0)
			error("cannot cd to maildir");
		if (mkdir("cur", 0700) < 0 && errno != EEXIST)
			error("mkdir cur failed");
		if (mkdir("tmp", 0700) < 0 && errno != EEXIST)
			error("mkdir tmp failed");
		if (mkdir("new", 0700) < 0 && errno != EEXIST)
			error("mkdir new failed");
		snprintf(tmp, sizeof tmp, "tmp/%lld.%d.%s",
		    (long long int) time(NULL),
		    getpid(), env->sc_hostname);
		fd = open(tmp, O_CREAT | O_EXCL | O_WRONLY, 0600);
		if (fd < 0)
			error("cannot open tmp file");
		fp = fdopen(fd, "w");
		if (fp == NULL)
			error2("fdopen");
		while ((ch = getc(stdin)) != EOF)
			if (putc(ch, fp) == EOF)
				break;
		if (ferror(stdin))
			error2("read error");
		if (fflush(fp) == EOF || ferror(fp))
			error2("write error");
		if (fsync(fd) < 0)
			error2("fsync");
		if (fclose(fp) == EOF)
			error2("fclose");
		snprintf(new, sizeof new, "new/%s", tmp + 4);
		if (rename(tmp, new) < 0)
			error2("cannot rename tmp->new");
		_exit(0);
	}
#undef error2

	if (deliver->mode == A_FILENAME) {
		struct stat 	 sb;
		time_t		 now;
		size_t		 len;
		int		 fd;
		FILE		*fp;
		char		*ln;

#define error2(m) { n = errno; ftruncate(fd, sb.st_size); errno = n; error(m); }
		setproctitle("file delivery");
		fd = open(deliver->to, O_CREAT | O_APPEND | O_WRONLY, 0600);
		if (fd < 0)
			error("open");
		if (fstat(fd, &sb) < 0)
			error("fstat");
#ifndef HAVE_STAT_ST_FLAGS
		if (S_ISREG(sb.st_mode) && flock(fd, LOCK_EX) < 0)
#else
		if (S_ISREG(sb.st_flags) && flock(fd, LOCK_EX) < 0)
#endif
			error("flock");
		fp = fdopen(fd, "a");
		if (fp == NULL)
			error("fdopen");
		time(&now);
		fprintf(fp, "From %s@%s %s", SMTPD_USER, env->sc_hostname,
		    ctime(&now));
		while ((ln = fgetln(stdin, &len)) != NULL) {
			if (ln[len - 1] == '\n')
				len--;
			if (len >= 5 && memcmp(ln, "From ", 5) == 0)
				putc('>', fp);
			fprintf(fp, "%.*s\n", (int)len, ln);
			if (ferror(fp))
				break;
		}
		if (ferror(stdin))
			error2("read error");
		putc('\n', fp);
		if (fflush(fp) == EOF || ferror(fp))
			error2("write error");
		if (fsync(fd) < 0)
			error2("fsync");
		if (fclose(fp) == EOF)
			error2("fclose");
		_exit(0);
	}

	error("forkmda: unknown mode");
}
#undef error
#undef error2

static int
parent_enqueue_offline(char *runner_path)
{
	char		 path[MAXPATHLEN];
	struct user_backend *ub;
	struct user	 u;
	struct stat	 sb;
	pid_t		 pid;

	log_debug("parent_enqueue_offline: path %s", runner_path);

	if (! bsnprintf(path, sizeof(path), "%s%s", PATH_SPOOL, runner_path))
		fatalx("parent_enqueue_offline: filename too long");

	if (! path_starts_with(path, PATH_SPOOL PATH_OFFLINE))
		fatalx("parent_enqueue_offline: path outside offline dir");

	if (lstat(path, &sb) == -1) {
		if (errno == ENOENT) {
			log_warn("parent_enqueue_offline: %s", path);
			return (0);
		}
		fatal("parent_enqueue_offline: lstat");
	}

#ifdef HAVE_CHFLAGS
	if (chflags(path, 0) == -1) {
		if (errno == ENOENT) {
			log_warn("parent_enqueue_offline: %s", path);
			return (0);
		}
		fatal("parent_enqueue_offline: chflags");
	}
#endif

	ub = user_backend_lookup(USER_GETPWNAM);
	bzero(&u, sizeof (u));
	errno = 0;
	if (! ub->getbyuid(&u, sb.st_uid)) {
		log_warn("parent_enqueue_offline: getpwuid for uid %d failed",
		    sb.st_uid);
		unlink(path);
		return (0);
	}

	if (! S_ISREG(sb.st_mode)) {
		log_warnx("file %s (uid %d) not regular, removing", path, sb.st_uid);
		if (S_ISDIR(sb.st_mode))
			rmdir(path);
		else
			unlink(path);
		return (0);
	}

	if ((pid = fork()) == -1)
		fatal("parent_enqueue_offline: fork");

	if (pid == 0) {
		char	*envp[2], *p, *tmp;
		FILE	*fp;
		size_t	 len;
		arglist	 args;

		bzero(&args, sizeof(args));

		if (setgroups(1, &u.gid) ||
		    setresgid(u.gid, u.gid, u.gid) ||
		    setresuid(u.uid, u.uid, u.uid) ||
		    closefrom(STDERR_FILENO + 1) == -1) {
			unlink(path);
			_exit(1);
		}

		if ((fp = fopen(path, "r")) == NULL) {
			unlink(path);
			_exit(1);
		}
		unlink(path);

		if (chdir(u.directory) == -1 && chdir("/") == -1)
			_exit(1);

		if (setsid() == -1 ||
		    signal(SIGPIPE, SIG_DFL) == SIG_ERR ||
		    dup2(fileno(fp), STDIN_FILENO) == -1)
			_exit(1);

		if ((p = fgetln(fp, &len)) == NULL)
			_exit(1);

		if (p[len - 1] != '\n')
			_exit(1);
		p[len - 1] = '\0';

		addargs(&args, "%s", "sendmail");

		while ((tmp = strsep(&p, "|")) != NULL)
			addargs(&args, "%s", tmp);

		if (lseek(fileno(fp), len, SEEK_SET) == -1)
			_exit(1);

		envp[0] = "PATH=" _PATH_DEFPATH;
		envp[1] = (char *)NULL;
		environ = envp;

		execvp(PATH_SMTPCTL, args.list);
		_exit(1);
	}

	queueing_running++;
	child_add(pid, CHILD_ENQUEUE_OFFLINE, -1);

	return (1);
}

static int
queueing_add(char *path)
{
	struct queueing	*q;

	if (queueing_running < QUEUEING_MAX)
		/* skip queue */
		return parent_enqueue_offline(path);

	q = malloc(sizeof(*q) + strlen(path) + 1);
	if (q == NULL)
		return (-1);
	q->path = (char *)q + sizeof(*q);
	memmove(q->path, path, strlen(path) + 1);
	TAILQ_INSERT_TAIL(&queueing_q, q, entry);

	return (1);
}

static void
queueing_done(void)
{
	struct queueing	*q;

	queueing_running--;

	while(queueing_running < QUEUEING_MAX) {
		if ((q = TAILQ_FIRST(&queueing_q)) == NULL)
			break; /* all done */
		TAILQ_REMOVE(&queueing_q, q, entry);
		parent_enqueue_offline(q->path);
		free(q);
	}
}

static int
parent_forward_open(char *username)
{
	struct user_backend *ub;
	struct user u;
	char pathname[MAXPATHLEN];
	int fd;

	bzero(&u, sizeof (u));
	ub = user_backend_lookup(USER_GETPWNAM);
	if (! ub->getbyname(&u, username))
		return -1;

	if (! bsnprintf(pathname, sizeof (pathname), "%s/.forward", u.directory))
		fatal("snprintf");

	fd = open(pathname, O_RDONLY);
	if (fd == -1) {
		if (errno == ENOENT)
			return -2;
		log_warn("parent_forward_open: %s", pathname);
		return -1;
	}

	if (! secure_file(fd, pathname, u.directory, u.uid, 1)) {
		log_warnx("%s: unsecure file", pathname);
		close(fd);
		return -1;
	}

	return fd;
}

int
path_starts_with(char *file, char *prefix)
{
	char	 rprefix[MAXPATHLEN];
	char	 rfile[MAXPATHLEN];

	if (realpath(file, rfile) == NULL || realpath(prefix, rprefix) == NULL)
		return (-1);

	return (strncmp(rfile, rprefix, strlen(rprefix)) == 0);
}

int
child_cmp(struct child *c1, struct child *c2)
{
	if (c1->pid < c2->pid)
		return (-1);

	if (c1->pid > c2->pid)
		return (1);

	return (0);
}

void
imsg_dispatch(int fd, short event, void *p)
{
	struct imsgev		*iev = p;
	struct imsg		 imsg;
	ssize_t			 n;

	if (event & EV_READ) {
		if ((n = imsg_read(&iev->ibuf)) == -1)
			fatal("imsg_read");
		if (n == 0) {
			/* this pipe is dead, so remove the event handler */
			event_del(&iev->ev);
			event_loopexit(NULL);
			return;
		}
	}
	if (event & EV_WRITE) {
		if (msgbuf_write(&iev->ibuf.w) != 0)
			fatal("msgbuf_write");
	}

	for (;;) {
		if ((n = imsg_get(&iev->ibuf, &imsg)) == -1)
			fatal("imsg_get");
		if (n == 0)
			break;
		imsg_callback(iev, &imsg);
		imsg_free(&imsg);
	}
	imsg_event_add(iev);
}

SPLAY_GENERATE(childtree, child, entry, child_cmp);

#ifdef VALGRIND
void clean_setproctitle(void)
{
	int i;

	log_debug("clean_setproctitle");

	for (i = 0; i < saved_argc; i++)
		free(saved_argv[i]);

	free(saved_argv);

#if defined(SPT_TYPE) && SPT_TYPE == SPT_REUSEARGV
	for (i = 0; environ[i] != NULL; i++)
		free(environ[i]);
	free(environ);
#endif
}
#endif /* VALGRIND */