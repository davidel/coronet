/*    Copyright 2023 Davide Libenzi
 * 
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 * 
 *        http://www.apache.org/licenses/LICENSE-2.0
 * 
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 * 
 */


#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/time.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <fcntl.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <stdarg.h>
#include <limits.h>
#include <signal.h>
#include <dirent.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <arpa/nameser.h>
#include <netdb.h>
#include "coronet.h"




#define CNHD_EVWAIT_TIMEO 1000
#define CNHD_STKSIZE (1024 * 8)



static int cnhd_set_cork(int fd, int v);
static int cnhd_send_mem(struct sk_conn *conn, long size, char const *ver,
			 char const *cclose);
static int cnhd_send_doc(struct sk_conn *conn, char const *doc, char const *ver,
			 char const *cclose);
static int cnhd_send_url(struct sk_conn *conn, char const *doc, char const *ver,
			 char const *cclose);
static void *cnhd_service(void *data);
static void *cnhd_acceptor(void *data);
static void cnhd_sigint(int sig);
static void cnhd_usage(char const *prg);




static int stopsvr;
static char const *rootfs = ".";
static int svr_port = 80;
static int lsnbklog = 1024;
static int stksize = CNHD_STKSIZE;
static unsigned long long conns, reqs, tbytes;




static int cnhd_set_cork(int fd, int v) {

	return setsockopt(fd, SOL_TCP, TCP_CORK, &v, sizeof(v));
}

static int cnhd_send_mem(struct sk_conn *conn, long size, char const *ver,
			 char const *cclose) {
	size_t csize, n;
	long msent;
	static char mbuf[1024 * 8];

	cnhd_set_cork(conn->sfd, 1);
	conet_printf(conn,
		     "%s 200 OK\r\n"
		     "Connection: %s\r\n"
		     "Content-Length: %ld\r\n"
		     "\r\n", ver, cclose, size);
	for (msent = 0; msent < size;) {
		csize = (size - msent) > sizeof(mbuf) ?
			sizeof(mbuf): (size_t) (size - msent);
		if ((n = conet_write(conn, mbuf, csize)) > 0)
			msent += n;
		if (n != csize)
			break;
	}
	cnhd_set_cork(conn->sfd, 0);

	tbytes += msent;

	return msent == size ? 0: -1;
}

static int cnhd_send_doc(struct sk_conn *conn, char const *doc, char const *ver,
			 char const *cclose) {

	conet_printf(conn,
		     "%s 404 OK\r\n"
		     "Connection: %s\r\n"
		     "Content-Length: 0\r\n"
		     "\r\n", ver, cclose);

	return 0;
}

static int cnhd_send_url(struct sk_conn *conn, char const *doc, char const *ver,
			 char const *cclose) {
	int error;

	if (strncmp(doc, "/mem-", 5) == 0)
		error = cnhd_send_mem(conn, atol(doc + 5), ver, cclose);
	else
		error = cnhd_send_doc(conn, doc, ver, cclose);

	return error;
}

static void *cnhd_service(void *data) {
	int cfd = (int) (long) data;
	int cclose = 0, chunked, lsize, clen;
	char *req, *meth, *doc, *ver, *ln, *auxptr;
	struct sk_conn *conn;

	if ((conn = conet_new_conn(cfd, co_current())) == NULL)
		return NULL;
	while (!stopsvr && !cclose) {
		if ((req = conet_readln(conn, &lsize)) == NULL)
			break;
		if ((meth = strtok_r(req, " ", &auxptr)) == NULL ||
		    (doc = strtok_r(NULL, " ", &auxptr)) == NULL ||
		    (ver = strtok_r(NULL, " \r\n", &auxptr)) == NULL ||
		    strcasecmp(meth, "GET") != 0) {
			bad_request:
			free(req);
			conet_printf(conn,
				     "HTTP/1.1 400 Bad request\r\n"
				     "Connection: close\r\n"
				     "Content-Length: 0\r\n"
				     "\r\n");
			break;
		}
		reqs++;
		cclose = strcasecmp(ver, "HTTP/1.1") != 0;
		for (clen = 0, chunked = 0;;) {
			if ((ln = conet_readln(conn, &lsize)) == NULL)
				break;
			if (strcmp(ln, "\r\n") == 0) {
				free(ln);
				break;
			}
			if (strncasecmp(ln, "Content-Length:", 15) == 0) {
				for (auxptr = ln + 15; *auxptr == ' '; auxptr++);
				clen = atoi(auxptr);
			} else if (strncasecmp(ln, "Connection:", 11) == 0) {
				for (auxptr = ln + 11; *auxptr == ' '; auxptr++);
				cclose = strncasecmp(auxptr, "close", 5) == 0;
			} else if (strncasecmp(ln, "Transfer-Encoding:", 18) == 0) {
				for (auxptr = ln + 18; *auxptr == ' '; auxptr++);
				chunked = strncasecmp(auxptr, "chunked", 7) == 0;
			}
			free(ln);
		}
		/*
		 * Sorry, really stupid HTTP server here. Neither GET payload nor
		 * chunked encoding allowed.
		 */
		if (clen || chunked)
			goto bad_request;
		cnhd_send_url(conn, doc, ver, cclose ? "close": "keep-alive");
		free(req);
	}
	conet_close_conn(conn);

	return data;
}

static void *cnhd_acceptor(void *data) {
	int sfd = (int) (long) data;
	int cfd, addrlen = sizeof(struct sockaddr_in);
	coroutine_t co;
	struct sk_conn *conn;
	struct sockaddr_in addr;

	if ((conn = conet_new_conn(sfd, co_current())) == NULL)
		return NULL;
	while (!stopsvr &&
	       (cfd = conet_accept(conn, (struct sockaddr *) &addr,
				   &addrlen)) != -1) {
		conns++;
		if ((co = co_create((void *) cnhd_service, (void *) (long) cfd, NULL,
				    stksize)) == NULL) {
			fprintf(stderr, "Unable to create coroutine\n");
			close(cfd);
		} else
			co_call(co);
	}
	conet_close_conn(conn);

	return data;
}

static void cnhd_sigint(int sig) {

	stopsvr++;
}

static void cnhd_usage(char const *prg) {

	fprintf(stderr, "Use: %s [-p PORT (%d)] [-r ROOTFS ('%s')] [-L LSNBKLOG (%d)]\n"
		"\t[-S STKSIZE (%d)] [-h]\n", prg, svr_port, rootfs, lsnbklog, stksize);
}

int main(int ac, char **av) {
	int i, sfd, one = 1;
	coroutine_t co;
	struct linger ling = { 0, 0 };
	struct sockaddr_in addr;

	for (i = 1; i < ac; i++) {
		if (strcmp(av[i], "-r") == 0) {
			if (++i < ac)
				rootfs = av[i];
		} else if (strcmp(av[i], "-p") == 0) {
			if (++i < ac)
				svr_port = atoi(av[i]);
		} else if (strcmp(av[i], "-L") == 0) {
			if (++i < ac)
				lsnbklog = atol(av[i]);
		} else if (strcmp(av[i], "-S") == 0) {
			if (++i < ac)
				stksize = atoi(av[i]);
		} else {
			cnhd_usage(av[0]);
			return 1;
		}
	}
	signal(SIGINT, cnhd_sigint);
	signal(SIGPIPE, SIG_IGN);
	siginterrupt(SIGINT, 1);
	if (conet_init() < 0)
		return 1;
	if ((sfd = conet_socket(AF_INET, SOCK_STREAM, 0)) == -1) {
		conet_cleanup();
		return 2;
	}
	setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	setsockopt(sfd, SOL_SOCKET, SO_LINGER, &ling, sizeof(ling));

	addr.sin_family = AF_INET;
	addr.sin_port = htons(svr_port);
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(sfd, (struct sockaddr *) &addr, sizeof(addr)) == -1) {
		perror("bind");
		close(sfd);
		conet_cleanup();
		return 3;
	}
	listen(sfd, lsnbklog);
	if ((co = co_create((void *) cnhd_acceptor, (void *) (long) sfd, NULL,
			    stksize)) == NULL) {
		fprintf(stderr, "Unable to create coroutine\n");
		close(sfd);
		conet_cleanup();
		return 4;
	}
	co_call(co);

	while (!stopsvr) {
		conet_events_wait(CNHD_EVWAIT_TIMEO);
		conet_events_dispatch(0);
	}

	close(sfd);
	conet_cleanup();

	fprintf(stdout,
		"Connections .....: %llu\n"
		"Requests ........: %llu\n"
		"Total Bytes .....: %llu\n", conns, reqs, tbytes);

	return 0;
}

