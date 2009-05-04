/* nhrp_task.h - File descriptor polling and task scheduling
 *
 * Copyright (C) 2007 Timo Teräs <timo.teras@iki.fi>
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 or later as
 * published by the Free Software Foundation.
 *
 * See http://www.gnu.org/ for details.
 */

#ifndef NHRP_COMMON_H
#define NHRP_COMMON_H

#include <stdint.h>
#include <stdlib.h>
#include <poll.h>
#include <sys/time.h>
#include <sys/queue.h>
#include <linux/if_ether.h>

struct nhrp_packet;
struct nhrp_interface;
struct nhrp_address;
struct nhrp_peer;

extern const char *nhrp_config_file, *nhrp_script_file;
extern int nhrp_running, nhrp_verbose;

/* Mainloop and timed tasks */
struct nhrp_task;

struct nhrp_task_ops {
	void (*callback)(struct nhrp_task *task);
	char* (*describe)(struct nhrp_task *task, size_t buflen, char *buffer);
};
#define NHRP_TASK(x) \
	static void x##_callback(struct nhrp_task *); \
	static char* x##_describe(struct nhrp_task *, size_t, char *); \
	static struct nhrp_task_ops x = { x##_callback, x##_describe }

struct nhrp_task {
	const struct nhrp_task_ops *ops;
	LIST_ENTRY(nhrp_task) task_list;
	struct timeval execute_time;
};

LIST_HEAD(nhrp_task_list, nhrp_task);
extern struct nhrp_task_list nhrp_all_tasks;

void nhrp_time_monotonic(struct timeval *tv);
int nhrp_task_poll_fd(int fd, short events,
		      int (*callback)(void *ctx, int fd, short events),
		      void *ctx);
void nhrp_task_unpoll_fd(int fd);
void nhrp_task_run(void);
void nhrp_task_stop(void);
void nhrp_task_schedule(struct nhrp_task *task, int timeout,
			const struct nhrp_task_ops *ops);
void nhrp_task_schedule_relative(struct nhrp_task *task, struct timeval *tv,
				 int rel_ms, const struct nhrp_task_ops *ops);
void nhrp_task_cancel(struct nhrp_task *task);

#define nhrp_task_schedule_at(task, tv, ops) \
	nhrp_task_schedule_relative(task, tv, 0, ops)

/* Logging */
void nhrp_debug(const char *format, ...);
void nhrp_info(const char *format, ...);
void nhrp_error(const char *format, ...);
void nhrp_perror(const char *message);
void nhrp_hex_dump(const char *name, const uint8_t *buf, int bytes);

#define NHRP_BUG_ON(cond) if (cond) { \
	nhrp_error("BUG: failure at %s:%d/%s(): %s!", \
		__FILE__, __LINE__, __func__, #cond); \
	abort(); \
}

/* Initializers for system dependant stuff */
int forward_init(void);
int forward_local_addresses_changed(void);

int kernel_init(void);
int kernel_route(struct nhrp_interface *out_iface,
		 struct nhrp_address *dest,
		 struct nhrp_address *default_source,
		 struct nhrp_address *next_hop,
		 u_int16_t *mtu);
int kernel_send(uint8_t *packet, size_t bytes, struct nhrp_interface *out,
		struct nhrp_address *to);
int kernel_inject_neighbor(struct nhrp_address *neighbor,
			   struct nhrp_address *hwaddr,
			   struct nhrp_interface *dev);

int log_init(void);
int signal_init(void);
int admin_init(const char *socket);

#endif
