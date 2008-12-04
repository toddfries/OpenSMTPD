/*
 * Copyright (c) 2008 Gilles Chehade <gilles@openbsd.org>
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

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/tree.h>
#include <sys/queue.h>
#include <sys/param.h>

#include <sys/socket.h>

#include <ctype.h>
#include <db.h>
#include <err.h>
#include <errno.h>
#include <event.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>
#include <util.h>

#include "smtpd.h"

extern char *__progname;

static int usage(void);
int parse_aliases(const char *);
int parse_entry(char *, size_t, size_t);

int alias_parse(struct alias *, char *, size_t);

DB *db;
static int dflag;

int
main(int argc, char *argv[])
{
	int ch;
	char pathname[MAXPATHLEN];
	char dbname[MAXPATHLEN];

	while ((ch = getopt(argc, argv, "dh")) != -1) {
		switch (ch) {
		case 'd':
			dflag = 1;
			break;
		case 'h':
		default:
			return usage();
		}
	}
	argc -= optind;
	argv += optind;

	if (argc != 0)
		return usage();

	bzero(pathname, MAXPATHLEN);
	snprintf(pathname, MAXPATHLEN, "/etc/mail/aliases.XXXXX");
	if (mkdtemp(pathname) == NULL)
		errx(1, "failed to create temporary directory");

	bzero(dbname, MAXPATHLEN);
	snprintf(dbname, MAXPATHLEN, "%s/aliases.db", pathname);
	db = dbopen(dbname, O_CREAT|O_EXLOCK|O_RDWR|O_SYNC, 0644, DB_HASH,
		NULL);
	if (db == NULL) {
		warn("dbopen");
		goto bad;
	}

	if (! parse_aliases(PATH_ALIASES)) {
		warnx("syntax error in aliases file");
		goto bad;
	}

	db->close(db);

	if (rename(dbname, PATH_ALIASESDB) == -1) {
		warn("rename");
		goto bad;
	}

	if (chmod(PATH_ALIASESDB, 0644) == -1)
		err(1, "chmod");

	if (rmdir(pathname) == -1)
		err(1, "rmdir");

	return EX_OK;
bad:
	if (dbname[0] != '\0')
		if (unlink(dbname) == -1)
			err(1, "unlink: %s", dbname);
	if (rmdir(pathname) == -1)
		err(1, "rmdir: %s", pathname);
	return 1;
}

static int
usage(void)
{
	fprintf(stderr, "usage: %s filename\n", __progname);
	return EX_USAGE;
}

int
parse_aliases(const char *filename)
{
	FILE *fp;
	char *line;
	size_t len;
	size_t lineno = 0;
	char delim[] = { '\\', '\\', '#' };

	fp = fopen(filename, "r");
	if (fp == NULL)
		errx(1, "failed to open aliases file");


	while ((line = fparseln(fp, &len, &lineno, delim, 0)) != NULL) {
		if (len == 0)
			continue;
		parse_entry(line, len, lineno);
		free(line);
	}

	fclose(fp);
	return 1;
}

int
parse_entry(char *line, size_t len, size_t lineno)
{
	char *name;
	char *delim;
	char *rcpt;
	char *subrcpt;
	struct alias alias;
	int ret;
	DBT key;
	DBT val;

	name = line;
	while (*name && isspace(*name))
		++name;

	rcpt = delim = strchr(name, ':');
	if (name == rcpt)
		goto bad;
	*delim-- = 0;
	rcpt++;
	while (isspace(*delim))
		*delim-- = '\0';
	rcpt++;
	while (*rcpt && isspace(*rcpt))
		++rcpt;
	if (*rcpt == '\0')
		goto bad;

	/* At this point, name points to nul-terminate name */
	for (; (subrcpt = strsep(&rcpt, ",")) != NULL;) {
		while (*subrcpt && isspace(*subrcpt))
			++subrcpt;
		if (*subrcpt == '\0')
			continue;
		delim = subrcpt + strlen(subrcpt);
		delim--;
		while (isspace(*delim))
			*delim-- = '\0';

		key.data = name;
		key.size = strlen(name) + 1;

		if ((ret = db->get(db, &key, &val, 0)) == -1)
			errx(1, "db->get()");

		if (ret == 1) {
			val.data = NULL;
			val.size = 0;
		}

		if (! alias_parse(&alias, subrcpt, strlen(subrcpt)))
			goto bad;

		if (val.size == 0) {
			val.size = sizeof(struct alias);
			val.data = &alias;

			if ((ret = db->put(db, &key, &val, 0)) == -1)
				errx(1, "db->get()");
		}
		else {
			void *p;

			p = calloc(val.size + sizeof(alias), 1);
			if (p == NULL)
				errx(1, "calloc: memory exhausted");
			memcpy(p, val.data, val.size);
			memcpy((u_int8_t *)p + val.size, &alias, sizeof(alias));

			val.data = p;
			val.size += sizeof(alias);

			if ((ret = db->put(db, &key, &val, 0)) == -1)
				errx(1, "db->get()");

			free(p);
		}

		db->sync(db, 0);
	}

	return 1;

bad:
	warnx("line %zd: invalid entry: %s", lineno, line);
	return 0;
}