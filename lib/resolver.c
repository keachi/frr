/* C-Ares integration to Quagga mainloop
 * Copyright (c) 2014-2015 Timo Teräs
 *
 * This file is free software: you may copy, redistribute and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <ares.h>
#include <ares_version.h>

#include "typesafe.h"
#include "jhash.h"
#include "thread.h"
#include "lib_errors.h"
#include "resolver.h"
#include "command.h"
#include "xref.h"

XREF_SETUP();

struct resolver_state {
	ares_channel channel;
	struct thread_master *master;
	struct thread *timeout;
};

static struct resolver_state state;
static bool resolver_debug;

/* a FD doesn't necessarily map 1:1 to a request;  we could be talking to
 * multiple caches simultaneously, to see which responds fastest.
 * Theoretically we could also be using the same fd for multiple lookups,
 * but the c-ares API guarantees an n:1 mapping for fd => channel.
 *
 * Either way c-ares makes that decision and we just need to deal with
 * whatever FDs it gives us.
 */

DEFINE_MTYPE_STATIC(LIB, ARES_FD, "c-ares (DNS) file descriptor information");
PREDECL_HASH(resolver_fds);

struct resolver_fd {
	struct resolver_fds_item itm;

	int fd;
	struct resolver_state *state;
	struct thread *t_read, *t_write;
};

static int resolver_fd_cmp(const struct resolver_fd *a,
			   const struct resolver_fd *b)
{
	return numcmp(a->fd, b->fd);
}

static uint32_t resolver_fd_hash(const struct resolver_fd *item)
{
	return jhash_1word(item->fd, 0xacd04c9e);
}

DECLARE_HASH(resolver_fds, struct resolver_fd, itm, resolver_fd_cmp,
	     resolver_fd_hash);

static struct resolver_fds_head resfds[1] = {INIT_HASH(resfds[0])};

static struct resolver_fd *resolver_fd_get(int fd,
					   struct resolver_state *newstate)
{
	struct resolver_fd ref = {.fd = fd}, *res;

	res = resolver_fds_find(resfds, &ref);
	if (!res && newstate) {
		res = XCALLOC(MTYPE_ARES_FD, sizeof(*res));
		res->fd = fd;
		res->state = newstate;
		resolver_fds_add(resfds, res);

		if (resolver_debug)
			zlog_debug("c-ares registered FD %d", fd);
	}
	return res;
}

static void resolver_fd_drop_maybe(struct resolver_fd *resfd)
{
	if (resfd->t_read || resfd->t_write)
		return;

	if (resolver_debug)
		zlog_debug("c-ares unregistered FD %d", resfd->fd);

	resolver_fds_del(resfds, resfd);
	XFREE(MTYPE_ARES_FD, resfd);
}

/* end of FD housekeeping */

static void resolver_update_timeouts(struct resolver_state *r);

static int resolver_cb_timeout(struct thread *t)
{
	struct resolver_state *r = THREAD_ARG(t);

	ares_process(r->channel, NULL, NULL);
	resolver_update_timeouts(r);

	return 0;
}

static int resolver_cb_socket_readable(struct thread *t)
{
	struct resolver_fd *resfd = THREAD_ARG(t);
	struct resolver_state *r = resfd->state;

	thread_add_read(r->master, resolver_cb_socket_readable, resfd,
			resfd->fd, &resfd->t_read);
	/* ^ ordering important:
	 * ares_process_fd may transitively call THREAD_OFF(resfd->t_read)
	 * combined with resolver_fd_drop_maybe, so resfd may be free'd after!
	 */
	ares_process_fd(r->channel, resfd->fd, ARES_SOCKET_BAD);
	resolver_update_timeouts(r);

	return 0;
}

static int resolver_cb_socket_writable(struct thread *t)
{
	struct resolver_fd *resfd = THREAD_ARG(t);
	struct resolver_state *r = resfd->state;

	thread_add_write(r->master, resolver_cb_socket_writable, resfd,
			 resfd->fd, &resfd->t_write);
	/* ^ ordering important:
	 * ares_process_fd may transitively call THREAD_OFF(resfd->t_write)
	 * combined with resolver_fd_drop_maybe, so resfd may be free'd after!
	 */
	ares_process_fd(r->channel, ARES_SOCKET_BAD, resfd->fd);
	resolver_update_timeouts(r);

	return 0;
}

static void resolver_update_timeouts(struct resolver_state *r)
{
	struct timeval *tv, tvbuf;

	THREAD_OFF(r->timeout);
	tv = ares_timeout(r->channel, NULL, &tvbuf);
	if (tv) {
		unsigned int timeoutms = tv->tv_sec * 1000 + tv->tv_usec / 1000;

		thread_add_timer_msec(r->master, resolver_cb_timeout, r,
				      timeoutms, &r->timeout);
	}
}

static void ares_socket_cb(void *data, ares_socket_t fd, int readable,
			   int writable)
{
	struct resolver_state *r = (struct resolver_state *)data;
	struct resolver_fd *resfd;

	resfd = resolver_fd_get(fd, (readable || writable) ? r : NULL);
	if (!resfd)
		return;

	assert(resfd->state == r);

	if (!readable)
		THREAD_OFF(resfd->t_read);
	else if (!resfd->t_read)
		thread_add_read(r->master, resolver_cb_socket_readable, resfd,
				fd, &resfd->t_read);

	if (!writable)
		THREAD_OFF(resfd->t_write);
	else if (!resfd->t_write)
		thread_add_write(r->master, resolver_cb_socket_writable, resfd,
				 fd, &resfd->t_write);

	resolver_fd_drop_maybe(resfd);
}


static void ares_address_cb(void *arg, int status, int timeouts,
			    struct hostent *he)
{
	struct resolver_query *query = (struct resolver_query *)arg;
	union sockunion addr[16];
	void (*callback)(struct resolver_query *, const char *, int,
			 union sockunion *);
	size_t i;

	callback = query->callback;
	query->callback = NULL;

	if (status != ARES_SUCCESS) {
		if (resolver_debug)
			zlog_debug("[%p] Resolving failed (%s)",
				   query, ares_strerror(status));

		callback(query, ares_strerror(status), -1, NULL);
		return;
	}

	for (i = 0; i < array_size(addr) && he->h_addr_list[i] != NULL; i++) {
		memset(&addr[i], 0, sizeof(addr[i]));
		addr[i].sa.sa_family = he->h_addrtype;
		switch (he->h_addrtype) {
		case AF_INET:
			memcpy(&addr[i].sin.sin_addr,
			       (uint8_t *)he->h_addr_list[i], he->h_length);
			break;
		case AF_INET6:
			memcpy(&addr[i].sin6.sin6_addr,
			       (uint8_t *)he->h_addr_list[i], he->h_length);
			break;
		}
	}

	if (resolver_debug)
		zlog_debug("[%p] Resolved with %d results", query, (int)i);

	callback(query, NULL, i, &addr[0]);
}

static int resolver_cb_literal(struct thread *t)
{
	struct resolver_query *query = THREAD_ARG(t);
	void (*callback)(struct resolver_query *, const char *, int,
			 union sockunion *);

	callback = query->callback;
	query->callback = NULL;

	callback(query, ARES_SUCCESS, 1, &query->literal_addr);
	return 0;
}

void resolver_resolve(struct resolver_query *query, int af,
		      const char *hostname,
		      void (*callback)(struct resolver_query *, const char *,
				       int, union sockunion *))
{
	int ret;

	if (query->callback != NULL) {
		flog_err(
			EC_LIB_RESOLVER,
			"Trying to resolve '%s', but previous query was not finished yet",
			hostname);
		return;
	}

	query->callback = callback;
	query->literal_cb = NULL;

	ret = str2sockunion(hostname, &query->literal_addr);
	if (ret == 0) {
		if (resolver_debug)
			zlog_debug("[%p] Resolving '%s' (IP literal)",
				   query, hostname);

		/* for consistency with proper name lookup, don't call the
		 * callback immediately; defer to thread loop
		 */
		thread_add_timer_msec(state.master, resolver_cb_literal,
				      query, 0, &query->literal_cb);
		return;
	}

	if (resolver_debug)
		zlog_debug("[%p] Resolving '%s'", query, hostname);

	ares_gethostbyname(state.channel, hostname, af, ares_address_cb, query);
	resolver_update_timeouts(&state);
}

DEFUN(debug_resolver,
      debug_resolver_cmd,
      "[no] debug resolver",
      NO_STR
      DEBUG_STR
      "Debug DNS resolver actions\n")
{
	resolver_debug = (argc == 2);
	return CMD_SUCCESS;
}

static int resolver_config_write_debug(struct vty *vty);
static struct cmd_node resolver_debug_node = {
	.name = "resolver debug",
	.node = RESOLVER_DEBUG_NODE,
	.prompt = "",
	.config_write = resolver_config_write_debug,
};

static int resolver_config_write_debug(struct vty *vty)
{
	if (resolver_debug)
		vty_out(vty, "debug resolver\n");
	return 1;
}


void resolver_init(struct thread_master *tm)
{
	struct ares_options ares_opts;

	state.master = tm;

	ares_opts = (struct ares_options){
		.sock_state_cb = &ares_socket_cb,
		.sock_state_cb_data = &state,
		.timeout = 2,
		.tries = 3,
	};

	ares_init_options(&state.channel, &ares_opts,
			  ARES_OPT_SOCK_STATE_CB | ARES_OPT_TIMEOUT
				  | ARES_OPT_TRIES);

	install_node(&resolver_debug_node);
	install_element(CONFIG_NODE, &debug_resolver_cmd);
	install_element(ENABLE_NODE, &debug_resolver_cmd);
}
