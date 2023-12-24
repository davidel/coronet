/*
 *  coronet by Davide Libenzi (coroutine/epoll network engine)
 *  Copyright (C) 2007  Davide Libenzi
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *  Davide Libenzi <davidel@xmailserver.org>
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
#include "coronet_lists.h"



#define CNHL_STKSIZE (1024 * 8)
#define CNHL_EVWAIT_TIMEO 500
#define CNHL_STATUPDATE_TMSTEP 1000
#define CNHL_TMSAMPLE 200

#define CNHL_KAVG 3
#define CNHL_AVG(c, a) (((c) + CNHL_KAVG * (a)) / (CNHL_KAVG + 1))



enum cnhl_errors {
	CNHL_ENETWORK = 0,
		CNHL_ECOROUTINE,
		CNHL_ECONNECT,
		CNHL_EREAD,
		CNHL_EWRITE,
		CNHL_EPROTO,
		CNHL_E200,
		CNHL_E300,
		CNHL_E400,
		CNHL_E500,

		CNHL_EMAX
};

struct cnhl_waiter {
	struct ll_head lnk;
	coroutine_t co;
};




static unsigned long long cnhl_mstime(void);
static void cnhl_usage(char const *prg);
static int cnhl_chunkread(struct sk_conn *conn, void *gbuf, int size);
static void *cnhl_session(void *data);
static int cnhl_new_conn(void);
static void cnhl_update_stats(void);




static int stopldr;
static char const *svr_host;
static int svr_port = 80;
static struct sockaddr_in saddr;
static long num_conns;
static long max_conns;
static long max_active;
static int num_reqs = 1;
static int num_urls;
static char **doc_urls;
static int url_next;
static int stksize = CNHL_STKSIZE;
static long live_coros;
static long open_conns;
static long total_conns;
static struct ll_head wlist;
static long errors[CNHL_EMAX];
static long htresps, last_htresps;
static unsigned long long rxbytes, last_rxbytes;
static double acrate, max_acrate, abrate, max_abrate;
static unsigned long long tlu, tl, tu = CNHL_STATUPDATE_TMSTEP, ts = CNHL_TMSAMPLE;
static char const * const errstrs[] = {
	"Network",
		"Coroutines",
		"Connect",
		"Read",
		"Write",
		"Protocol",
		"HTTP 2xx",
		"HTTP 3xx",
		"HTTP 4xx",
		"HTTP 5xx"
};



static unsigned long long cnhl_mstime(void) {
	struct timeval tv;

	if (gettimeofday(&tv, NULL) != 0)
		return 0;

	return 1000ULL * tv.tv_sec + tv.tv_usec / 1000;
}

static void cnhl_usage(char const *prg) {

	fprintf(stderr, "Use: %s -s HOST -n NCON [-p PORT (%d)] [-r NREQS (%d)]\n"
		"\t[-S STKSIZE (%d)] [-M MAXCONNS] [-t TMUPD (%d)] [-a NACTIVE]\n"
		"\t[-T TMSAMP (%llu)] [-h] URL ...\n",
		prg, svr_port, num_reqs, stksize, CNHL_STATUPDATE_TMSTEP, ts);
}

static int cnhl_chunkread(struct sk_conn *conn, void *gbuf, int size) {

	fprintf(stderr, "Chunked read not implemented yet\n");

	return -1;
}

static void *cnhl_session(void *data) {
	int i, n, hcode, size, clen, chunked, cclose;
	struct sk_conn *conn;
	char const *curl, *ptr, *ver, *code, *msg;
	char *ln, *aux;
	struct cnhl_waiter wnode;
	static char gbuf[8192];

	live_coros++;
	total_conns++;
	if ((conn = conet_create_conn(AF_INET, SOCK_STREAM, 0,
				      co_current())) == NULL) {
		errors[CNHL_ENETWORK]++;
		goto dexit;
	}
	if (conet_connect(conn, (struct sockaddr *) &saddr, sizeof(saddr)) < 0) {
		errors[CNHL_ECONNECT]++;
		goto erxit;
	}

	/*
	 * Wait after the connection for a signal from the main dispatcher. We may
	 * receive spurious co_calls, so we need to be sure we are called by the main
	 * dispatcher.
	 */
	wnode.co = co_current();
	conet_lladdt(&wnode.lnk, &wlist);
	do {
		co_resume();
	} while (!conet_llempty(&wnode.lnk));

	open_conns++;
	curl = doc_urls[url_next];
	url_next = (url_next + 1) % num_urls;
	for (i = 0; !stopldr && i < num_reqs; i++) {
		if (conet_printf(conn,
				 "GET %s HTTP/1.1\r\n"
				 "Host: %s\r\n"
				 "Connection: %s\r\n"
				 "Content-Length: 0\r\n"
				 "\r\n",
				 curl, svr_host, i + 1 < num_reqs ? "keep-alive": "close") < 0) {
			errors[CNHL_EWRITE]++;
			break;
		}
		if ((ln = conet_readln(conn, &size)) == NULL) {
			errors[CNHL_EREAD]++;
			break;
		}
		ver = strtok_r(ln, " ", &aux);
		code = strtok_r(NULL, " ", &aux);
		msg = strtok_r(NULL, " \r\n", &aux);
		hcode = code != NULL && isdigit(*code) ? atoi(code): -1;
		free(ln);
		if (hcode < 200 || hcode >= 600) {
			errors[CNHL_EPROTO]++;
			break;
		}
		htresps++;
		for (clen = cclose = -1, chunked = 0;;) {
			if ((ln = conet_readln(conn, &size)) == NULL) {
				errors[CNHL_EREAD]++;
				goto erxit;
			}
			if (strcmp(ln, "\r\n") == 0) {
				free(ln);
				break;
			}
			if (strncasecmp(ln, "Content-Length:", 15) == 0) {
				for (ptr = ln + 15; *ptr == ' ' || *ptr == '\t'; ptr++);
				clen = atoi(ptr);
			} else if (strncasecmp(ln, "Transfer-Encoding:", 18) == 0) {
				for (ptr = ln + 18; *ptr == ' ' || *ptr == '\t'; ptr++);
				chunked = strncasecmp(ptr, "chunked", 7) == 0;
			} else if (strncasecmp(ln, "Connection:", 11) == 0) {
				for (ptr = ln + 11; *ptr == ' ' || *ptr == '\t'; ptr++);
				cclose = strncasecmp(ptr, "close", 5) == 0;
			}
			free(ln);
		}
		if (clen >= 0) {
			for (n = 0; n < clen;) {
				size = (clen - n) > sizeof(gbuf) ? sizeof(gbuf): clen - n;
				if (conet_read(conn, gbuf, size) != size) {
					errors[CNHL_EREAD]++;
					goto erxit;
				}
				n += size;
			}
		} else if (chunked) {
			if ((n = cnhl_chunkread(conn, gbuf, sizeof(gbuf))) < 0) {
				errors[CNHL_EPROTO]++;
				goto erxit;
			}
		} else {
			if (cclose == 0) {
				errors[CNHL_EPROTO]++;
				goto erxit;
			}
			n = 0;
			do {
				if ((size = conet_read(conn, gbuf, sizeof(gbuf))) > 0)
					n += size;
			} while (size == sizeof(gbuf));
		}
		rxbytes += n;
		errors[CNHL_E200 + hcode / 100 - 2]++;
	}
	open_conns--;
	erxit:
	conet_close_conn(conn);
	dexit:
	live_coros--;

	return data;
}

static int cnhl_new_conn(void) {
	coroutine_t co;

	if ((co = co_create((void *) cnhl_session, NULL, NULL,
			    stksize)) == NULL) {
		fprintf(stderr, "Unable to create coroutine\n");
		errors[CNHL_ECOROUTINE]++;
		return -1;
	}
	co_call(co);

	return 0;
}

static void cnhl_update_stats(void) {
	unsigned long long tc;
	double crate, brate;

	if ((tc = cnhl_mstime()) > tl + ts) {
		crate = 1000.0 * (htresps - last_htresps) / (double) (tc - tl);
		brate = 1000.0 * (rxbytes - last_rxbytes) / (double) (tc - tl);
		acrate = CNHL_AVG(crate, acrate);
		abrate = CNHL_AVG(brate, abrate);
		if (acrate > max_acrate)
			max_acrate = acrate;
		if (abrate > max_abrate)
			max_abrate = abrate;
		if (tc > tlu + tu) {
			fprintf(stdout, "%9ld  %9ld  %9ld  %12llu  %9.1f  %12.1f\n",
				live_coros, open_conns, htresps, rxbytes, acrate, abrate);
			tlu = tc;
		}
		last_htresps = htresps;
		last_rxbytes = rxbytes;
		tl = tc;
	}
}

static void cnhl_sigint(int sig) {

	stopldr++;
}

int main(int ac, char **av) {
	int i;
	unsigned long long ti;
	struct ll_head *pos;
	struct cnhl_waiter *wnode;
	struct hostent *he;
	struct in_addr inadr;

	for (i = 1; i < ac; i++) {
		if (strcmp(av[i], "-s") == 0) {
			if (++i < ac)
				svr_host = av[i];
		} else if (strcmp(av[i], "-p") == 0) {
			if (++i < ac)
				svr_port = atoi(av[i]);
		} else if (strcmp(av[i], "-n") == 0) {
			if (++i < ac)
				num_conns = atol(av[i]);
		} else if (strcmp(av[i], "-a") == 0) {
			if (++i < ac)
				max_active = atol(av[i]);
		} else if (strcmp(av[i], "-r") == 0) {
			if (++i < ac)
				num_reqs = atoi(av[i]);
		} else if (strcmp(av[i], "-S") == 0) {
			if (++i < ac)
				stksize = atoi(av[i]);
		} else if (strcmp(av[i], "-M") == 0) {
			if (++i < ac)
				max_conns = atol(av[i]);
		} else if (strcmp(av[i], "-t") == 0) {
			if (++i < ac)
				tu = atol(av[i]);
		} else if (strcmp(av[i], "-T") == 0) {
			if (++i < ac)
				ts = atol(av[i]);
		} else if (strcmp(av[i], "-h") == 0) {
			cnhl_usage(av[0]);
			return 1;
		} else
			break;
	}
	if (i == ac || svr_host == NULL || num_conns == 0) {
		cnhl_usage(av[0]);
		return 1;
	}
	signal(SIGINT, cnhl_sigint);
	signal(SIGPIPE, SIG_IGN);
	siginterrupt(SIGINT, 1);
	if (max_active == 0)
		max_active = num_conns;
	num_urls = ac - i;
	doc_urls = &av[i];
	conet_llinit(&wlist);
	if (inet_aton(svr_host, &inadr) == 0) {
		if ((he = gethostbyname(svr_host)) == NULL) {
			fprintf(stderr, "Unable to resolve: %s\n", svr_host);
			return 2;
		}
		memcpy(&inadr.s_addr, he->h_addr_list[0], he->h_length);
	}
	memset(&saddr, 0, sizeof(saddr));
	saddr.sin_family = AF_INET;
	saddr.sin_port = htons(svr_port);
	memcpy(&saddr.sin_addr, &inadr.s_addr, 4);
	if (conet_init() < 0)
		return 2;

	fprintf(stdout, "%9s  %9s  %9s  %12s  %9s  %12s\n",
		"CONNS", "ACTIVE", "TRESP", "TBYTES", "RESPSEC", "BYTESEC");

	ti = tlu = tl = cnhl_mstime();
	while (!stopldr && (max_conns == 0 || total_conns < max_conns)) {
		while (live_coros < num_conns &&
		       (max_conns == 0 || total_conns < max_conns)) {
			if (cnhl_new_conn() < 0)
				goto erxit;
		}
		while (open_conns < max_active &&
		       (pos = conet_llfirst(&wlist)) != NULL) {
			wnode = CONET_LLENT(pos, struct cnhl_waiter, lnk);
			conet_lldel_init(pos);
			co_call(wnode->co);
		}

		conet_events_wait(CNHL_EVWAIT_TIMEO);
		conet_events_dispatch(0);

		cnhl_update_stats();
	}

	erxit:

	while (stopldr < 2 && live_coros > 0) {
		while (open_conns < max_active &&
		       (pos = conet_llfirst(&wlist)) != NULL) {
			wnode = CONET_LLENT(pos, struct cnhl_waiter, lnk);
			conet_lldel_init(pos);
			co_call(wnode->co);
		}

		conet_events_wait(CNHL_EVWAIT_TIMEO);
		conet_events_dispatch(0);

		cnhl_update_stats();
	}
	cnhl_update_stats();

	fprintf(stdout,
		"\n"
		"Peak Connection Rate ....: %11.1f conn/sec\n"
		"Peak Transfer Rate ......: %11.1f bytes/sec\n",
		max_acrate, max_abrate);

	fprintf(stderr, "\nError list:\n");
	for (i = 0 ; i < CNHL_EMAX; i++)
		fprintf(stderr, "\t%-12s  %ld\n", errstrs[i], errors[i]);
	conet_cleanup();

	return 0;
}

