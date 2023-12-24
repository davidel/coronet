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
#include <sys/ioctl.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdarg.h>
#include <limits.h>
#include <signal.h>
#include <dirent.h>
#include <sys/socket.h>
#include "coronet.h"
#include "coronet_lists.h"

/*
 * Epoll support is needed, so you need a Linux kernel 2.6.x or newer,
 * and a proper glibc.
 */
#include <sys/epoll.h>



/*
 * CONET_MAX_FDS is passed to epoll_create(), but recent versions of epoll
 * do not use that parameter anymore.
 */
#define CONET_MAX_FDS (1024 * 100)
#define CONET_MAX_EVENTS 128
#define CONET_TMOSLOTS (1 << 8)
#define CONET_TMOMASK (CONET_TMOSLOTS - 1)
#define CONET_TMOSTEP 1000


#define CONET_TMONEXT(c, a) (((c) + (a)) & CONET_TMOMASK)



static int conet_buf_refil(struct sk_conn *conn);
static int conet_yield(struct sk_conn *conn);
static int conet_read_ll(struct sk_conn *conn, char *buf, int nbyte);
static int conet_write_ll(struct sk_conn *conn, char const *buf, int nbyte);
static mstime_t conet_mstime(void);
static int conet_run_timers(mstime_t tcurr);




static int epfd = -1;
static int max_events, ready_events, next_event;
static struct epoll_event *evstore;
static struct ll_head fsklist, usklist;
static int tmobase;
static mstime_t tmotmbase, tmotmlast;
static struct ll_head tmolst[CONET_TMOSLOTS];
static struct ll_head tmoovlst;




int conet_init(void) {
	int i;

	if ((epfd = epoll_create(CONET_MAX_FDS)) == -1) {
		perror("epoll_create");
		return -1;
	}
	max_events = CONET_MAX_EVENTS;
	if ((evstore = (struct epoll_event *)
	     malloc(max_events * sizeof(struct epoll_event))) == NULL) {
		perror("evstore");
		close(epfd);
		return -1;
	}
	ready_events = next_event = 0;
	conet_llinit(&usklist);
	conet_llinit(&fsklist);
	tmobase = 0;
	tmotmbase = tmotmlast = conet_mstime();
	conet_llinit(&tmoovlst);
	for (i = 0; i < CONET_TMOSLOTS; i++)
		conet_llinit(&tmolst[i]);

	return 0;
}

void conet_cleanup(void) {
	struct ll_head *pos;
	struct sk_conn *conn;

	while ((pos = conet_llfirst(&fsklist)) != NULL) {
		conn = CONET_LLENT(pos, struct sk_conn, lnk);
		conet_lldel(pos);
		free(conn);
	}
	while ((pos = conet_llfirst(&usklist)) != NULL) {
		conn = CONET_LLENT(pos, struct sk_conn, lnk);
		conet_lldel(pos);
		close(conn->sfd);
		free(conn);
	}
	close(epfd);
	free(evstore);
}

static int conet_buf_refil(struct sk_conn *conn) {
	int n;

	conn->ridx = conn->bcnt = 0;
	if ((n = conet_read_ll(conn, conn->buf, CONET_BUFSIZE)) > 0)
		conn->bcnt = n;

	return n;
}

int conet_readsome(struct sk_conn *conn, void *buf, int n) {
	int cnt;

	if (conn->ridx < conn->bcnt) {
		if ((cnt = conn->bcnt - conn->ridx) > n)
			cnt = n;
		memcpy(buf, conn->buf + conn->ridx, cnt);
		conn->ridx += cnt;
	} else {
		cnt = conet_read_ll(conn, buf, n);
	}

	return cnt;
}

int conet_read(struct sk_conn *conn, void *buf, int n) {
	int cnt, acnt;

	for (cnt = 0; cnt < n;) {
		if ((acnt = conet_readsome(conn, buf, n - cnt)) <= 0)
			break;
		cnt += acnt;
		buf = (char *) buf + acnt;
	}

	return cnt;
}

char *conet_readln(struct sk_conn *conn, int *lnsize) {
	int lsize, nlsize, cnt;
	char *ln, *nln;
	char const *eol;

	for (lsize = 0, ln = NULL, eol = NULL; eol == NULL;) {
		if (conn->ridx == conn->bcnt &&
		    conet_buf_refil(conn) <= 0)
			break;
		if ((eol = (char *) memchr(conn->buf + conn->ridx, '\n',
					   conn->bcnt - conn->ridx)) != NULL)
			cnt = (int) (eol - (conn->buf + conn->ridx)) + 1;
		else
			cnt = conn->bcnt - conn->ridx;
		nlsize = lsize + cnt;
		if ((nln = (char *) realloc(ln, nlsize + 1)) == NULL) {
			perror("realloc");
			free(ln);
			return NULL;
		}
		conet_readsome(conn, nln + lsize, cnt);
		ln = nln;
		lsize = nlsize;
	}
	if (ln != NULL) {
		ln[lsize] = '\0';
		*lnsize = lsize;
	}

	return ln;
}

int conet_write(struct sk_conn *conn, void const *buf, int n) {
	int cnt, acnt;

	for (cnt = 0; cnt < n;) {
		if ((acnt = conet_write_ll(conn, buf, n - cnt)) < 0) {
			perror("write");
			return -1;
		}
		cnt += acnt;
		buf = (char const *) buf + acnt;
	}

	return cnt;
}

int conet_printf(struct sk_conn *conn, char const *fmt, ...) {
	int cnt;
	char *wstr = NULL;
	va_list args;

	va_start(args, fmt);
	cnt = vasprintf(&wstr, fmt, args);
	va_end(args);
	if (wstr != NULL) {
		cnt = conet_write(conn, wstr, cnt);
		free(wstr);
	}

	return cnt;
}

struct sk_conn *conet_new_conn(int sfd, coroutine_t co) {
	struct ll_head *pos;
	struct sk_conn *conn;
	struct epoll_event ev;

	if ((pos = conet_llfirst(&fsklist)) != NULL) {
		conet_lldel(pos);
		conn = CONET_LLENT(pos, struct sk_conn, lnk);
	} else {
		if ((conn = (struct sk_conn *) malloc(sizeof(struct sk_conn))) == NULL)
			return NULL;
	}
	conn->co = co;
	conn->sfd = sfd;
	conn->error = 0;
	conn->events = 0;
	conn->revents = 0;
	conn->timeo = -1;
	conn->ridx = conn->bcnt = 0;
	conet_llinit(&conn->tlnk);
	ev.events = 0;
	ev.data.ptr = conn;
	if (epoll_ctl(epfd, EPOLL_CTL_ADD, sfd, &ev) < 0) {
		fprintf(stderr, "epoll set insertion error (%s): fd=%d\n",
			strerror(errno), sfd);
		free(conn);
		return NULL;
	}
	conet_lladdt(&conn->lnk, &usklist);

	return conn;
}

void conet_close_conn(struct sk_conn *conn) {

	close(conn->sfd);
	conn->sfd = -1;
	conet_lldel(&conn->tlnk);
	conet_lldel(&conn->lnk);
	conet_lladdh(&conn->lnk, &fsklist);
}

/*
 * Timeouts in coronet are simply a way to be able to expire outstanding
 * requests, so do not expect high precision for them (since expiration
 * does not need to be so). The CONET_TMOSTEP parameter rules the expire
 * timeouts resolution.
 */
int conet_set_timeo(struct sk_conn *conn, int timeo) {

	conn->timeo = timeo * 1000;

	return 0;
}

static int conet_yield(struct sk_conn *conn) {
	int tidx;

	if (conn->timeo > 0) {
		conn->exptmo = conet_mstime() + conn->timeo;
		if (conn->exptmo - tmotmbase >= CONET_TMOSLOTS * CONET_TMOSTEP)
			conet_lladdt(&conn->tlnk, &tmoovlst);
		else {
			tidx = (conn->exptmo - tmotmbase) / CONET_TMOSTEP;
			tidx = CONET_TMONEXT(tmobase, tidx);
			conet_lladdt(&conn->tlnk, &tmolst[tidx]);
		}
	}
	co_resume();
	if (!conet_llempty(&conn->tlnk))
		conet_lldel_init(&conn->tlnk);

	return conn->error;
}

int conet_mod_conn(struct sk_conn *conn, unsigned int events) {
	struct epoll_event ev;

	ev.events = events | EPOLLET;
	ev.data.ptr = conn;
	if (epoll_ctl(epfd, EPOLL_CTL_MOD, conn->sfd, &ev) < 0) {
		fprintf(stderr, "epoll set modify error (%s): fd=%d\n",
			strerror(errno), conn->sfd);
		return -1;
	}

	return 0;
}

int conet_socket(int domain, int type, int protocol) {
	int sfd, flags = 1;
	struct linger ling = { 0, 0 };

	if ((sfd = socket(domain, type, protocol)) == -1) {
		perror("socket");
		return -1;
	}
	setsockopt(sfd, SOL_SOCKET, SO_LINGER, &ling, sizeof(ling));
	if (ioctl(sfd, FIONBIO, &flags) &&
	    ((flags = fcntl(sfd, F_GETFL, 0)) < 0 ||
	     fcntl(sfd, F_SETFL, flags | O_NONBLOCK) < 0)) {
		perror("setting O_NONBLOCK");
		close(sfd);
		return -1;
	}

	return sfd;
}

int conet_connect(struct sk_conn *conn, const struct sockaddr *serv_addr,
		  socklen_t addrlen) {

	if (connect(conn->sfd, serv_addr, addrlen) == -1) {
		if (errno != EWOULDBLOCK && errno != EINPROGRESS)
			return -1;
		if (!(conn->events & EPOLLOUT)) {
			conn->events = EPOLLOUT | EPOLLERR | EPOLLHUP;
			if (conet_mod_conn(conn, conn->events) < 0)
				return -1;
		}
		if (conet_yield(conn) < 0 ||
		    (conn->revents & (EPOLLERR | EPOLLHUP)))
			return -1;
	}

	return 0;
}

static int conet_read_ll(struct sk_conn *conn, char *buf, int nbyte) {
	int n;

	while ((n = read(conn->sfd, buf, nbyte)) < 0) {
		if (errno == EINTR)
			continue;
		if (errno != EAGAIN && errno != EWOULDBLOCK)
			return -1;
		if (!(conn->events & EPOLLIN)) {
			conn->events = EPOLLIN | EPOLLERR | EPOLLHUP;
			if (conet_mod_conn(conn, conn->events) < 0)
				return -1;
		}
		if (conet_yield(conn) < 0)
			return -1;
	}

	return n;
}

static int conet_write_ll(struct sk_conn *conn, char const *buf, int nbyte) {
	int n;

	while ((n = write(conn->sfd, buf, nbyte)) < 0) {
		if (errno == EINTR)
			continue;
		if (errno != EAGAIN && errno != EWOULDBLOCK)
			return -1;
		if (!(conn->events & EPOLLOUT)) {
			conn->events = EPOLLOUT | EPOLLERR | EPOLLHUP;
			if (conet_mod_conn(conn, conn->events) < 0)
				return -1;
		}
		if (conet_yield(conn) < 0)
			return -1;
	}

	return n;
}

int conet_accept(struct sk_conn *conn, struct sockaddr *addr, int *addrlen) {
	int cfd, flags = 1;
	struct linger ling = { 0, 0 };

	while ((cfd = accept(conn->sfd, addr, (socklen_t *) addrlen)) < 0) {
		if (errno == EINTR)
			continue;
		if (errno != EAGAIN && errno != EWOULDBLOCK)
			return -1;
		if (!(conn->events & EPOLLIN)) {
			conn->events = EPOLLIN | EPOLLERR | EPOLLHUP;
			if (conet_mod_conn(conn, conn->events) < 0)
				return -1;
		}
		if (conet_yield(conn) < 0)
			return -1;
	}
	setsockopt(cfd, SOL_SOCKET, SO_LINGER, &ling, sizeof(ling));
	if (ioctl(cfd, FIONBIO, &flags) &&
	    ((flags = fcntl(cfd, F_GETFL, 0)) < 0 ||
	     fcntl(cfd, F_SETFL, flags | O_NONBLOCK) < 0)) {
		perror("setting O_NONBLOCK");
		close(cfd);
		return -1;
	}

	return cfd;
}

struct sk_conn *conet_create_conn(int domain, int type, int protocol,
				  coroutine_t co) {
	int sfd;
	struct sk_conn *conn;

	if ((sfd = conet_socket(domain, type, protocol)) == -1)
		return NULL;
	if ((conn = conet_new_conn(sfd, co)) == NULL) {
		close(sfd);
		return NULL;
	}

	return conn;
}

static mstime_t conet_mstime(void) {
	struct timeval tv;

	if (gettimeofday(&tv, NULL) != 0)
		return 0;

	return 1000ULL * tv.tv_sec + tv.tv_usec / 1000;
}

static int conet_run_timers(mstime_t tcurr) {
	int i, base, tcount, xcount;
	struct ll_head *pos, *head;
	struct sk_conn *conn;

	i = base = tmobase;
	do {
		tcount = xcount = 0;
		head = &tmolst[i];
		for (pos = conet_llfirst(head); pos != NULL;) {
			conn = CONET_LLENT(pos, struct sk_conn, tlnk);
			pos = conet_llnext(pos, head);
			if (conn->exptmo >= tcurr) {
				xcount++;
				conet_lldel_init(&conn->tlnk);
				conn->error = -ETIMEDOUT;
				errno = ETIMEDOUT;
				co_call(conn->co);
			} else
				tcount++;
		}
		if (tcount == 0) {
			if (tmotmbase + CONET_TMOSTEP > tcurr)
				break;
			tmobase = CONET_TMONEXT(i, 1);
			tmotmbase += CONET_TMOSTEP;
		} else if (xcount == 0) {
			break;
		}
		i = CONET_TMONEXT(i, 1);
	} while (i != base);
	for (pos = conet_llfirst(&tmoovlst); pos != NULL;) {
		conn = CONET_LLENT(pos, struct sk_conn, tlnk);
		pos = conet_llnext(pos, &tmoovlst);
		if (conn->exptmo >= tcurr) {
			conet_lldel_init(&conn->tlnk);
			conn->error = -ETIMEDOUT;
			errno = ETIMEDOUT;
			co_call(conn->co);
		} else if (conn->exptmo - tmotmbase < CONET_TMOSLOTS * CONET_TMOSTEP) {
			i = (conn->exptmo - tmotmbase) / CONET_TMOSTEP;
			i = CONET_TMONEXT(tmobase, i);
			conet_lldel(&conn->tlnk);
			conet_lladdt(&conn->tlnk, &tmolst[i]);
		}
	}

	return 0;
}

int conet_events_wait(int timeo) {
	int cnt = 0;

	if (next_event == ready_events)
		ready_events = next_event = 0;
	if (timeo > CONET_TMOSTEP || timeo < 0)
		timeo = CONET_TMOSTEP;
	if (ready_events < max_events)
		cnt = epoll_wait(epfd, evstore + ready_events,
				 max_events - ready_events, timeo);
	if (cnt > 0)
		ready_events += cnt;

	return ready_events - next_event;
}

int conet_events_dispatch(int evdmax) {
	int i;
	mstime_t tcurr;
	struct sk_conn *conn;
	struct epoll_event *cevent;

	if (evdmax <= 0)
		evdmax = max_events;
	for (i = 0, cevent = evstore + next_event;
	     i < evdmax && next_event < ready_events;
	     next_event++, cevent++, i++) {
		conn = cevent->data.ptr;
		if (conn->sfd != -1) {
			conn->error = 0;
			conn->revents = cevent->events;
			if (conn->revents & conn->events)
				co_call(conn->co);
		}
	}
	tcurr = conet_mstime();
	if (tcurr > tmotmlast + CONET_TMOSTEP) {
		conet_run_timers(tcurr);
		tmotmlast = tcurr;
	}

	return i;
}

