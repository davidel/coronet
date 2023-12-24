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


#if !defined(_CORONET_H)
#define _CORONET_H


#include <sys/types.h>
#include <stdio.h>


/*
 * You need the Portable Coroutine Library (PCL) to build this source.
 * You can find a copy of PCL source code at :
 *
 *             http://www.xmailserver.org/libpcl.html
 *
 * Or, many distributions has a package for it (Debian names it "libpcl1-dev").
 *
 */
#include <pcl.h>


#ifdef __cplusplus
#define CNAPI extern "C"
#else
#define CNAPI
#endif


#define CONET_BUFSIZE (1024 * 2)

typedef unsigned long long mstime_t;

struct ll_head {
	struct ll_head *prev, *next;
};

struct sk_conn {
	struct ll_head lnk;
	coroutine_t co;
	int sfd;
	int error;
	unsigned int events, revents;
	int timeo;
	struct ll_head tlnk;
	mstime_t exptmo;
	int ridx, bcnt;
	char buf[CONET_BUFSIZE];
};



CNAPI int conet_init(void);
CNAPI void conet_cleanup(void);
CNAPI int conet_readsome(struct sk_conn *conn, void *buf, int n);
CNAPI int conet_read(struct sk_conn *conn, void *buf, int n);
CNAPI char *conet_readln(struct sk_conn *conn, int *lnsize);
CNAPI int conet_write(struct sk_conn *conn, void const *buf, int n);
CNAPI int conet_printf(struct sk_conn *conn, char const *fmt, ...);
CNAPI struct sk_conn *conet_new_conn(int sfd, coroutine_t co);
CNAPI void conet_close_conn(struct sk_conn *conn);
CNAPI int conet_set_timeo(struct sk_conn *conn, int timeo);
CNAPI int conet_mod_conn(struct sk_conn *conn, unsigned int events);
CNAPI int conet_socket(int domain, int type, int protocol);
CNAPI int conet_connect(struct sk_conn *conn, const struct sockaddr *serv_addr,
			socklen_t addrlen);
CNAPI int conet_accept(struct sk_conn *conn, struct sockaddr *addr, int *addrlen);
CNAPI struct sk_conn *conet_create_conn(int domain, int type, int protocol,
					coroutine_t co);
CNAPI int conet_events_wait(int timeo);
CNAPI int conet_events_dispatch(int evdmax);


#endif

