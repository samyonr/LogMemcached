/*
 * backup_rdma.c
 *
 */

/*
 * Copyright (c) 2005 Topspin Communications.  All rights reserved.
 * Copyright (c) 2006 Cisco Systems.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <netdb.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <signal.h>
#include <pthread.h>
#include <assert.h>
#include <sys/time.h>
#include <time.h>
#include <sys/param.h>
#include <infiniband/verbs.h>

#include "backup_rdma.h"
#include "memcached.h"

#define BACKLOG 10 // how many pending connections queue will hold

struct ibv_device      	**g_dev_list;
struct ibv_device		*g_ib_dev;
struct pingpong_context *g_ctx;
struct pingpong_dest     g_my_dest;
struct timeval           g_start, g_end;
char                    *g_ib_devname = NULL;
char                    *g_servername = NULL;
char                     *g_port = "18515";
int                      g_ib_port = 1;
int                      g_size = 4096;
enum ibv_mtu		 	 g_mtu = IBV_MTU_1024;
int                      g_rx_depth = 500;
int                      g_iters = 1000;
int                      g_routs;
int                      g_rcnt, g_scnt;
int                      g_num_cq_events = 0;
int                      g_sl = 0;
int			 			 g_gidx = -1;
char			 		 g_gid[33];

enum {
	PINGPONG_RECV_WRID = 1,
	PINGPONG_SEND_WRID = 2,
};

static int page_size;

struct pingpong_context {
	struct ibv_context		*context;
	struct ibv_comp_channel *channel;
	struct ibv_pd			*pd;
	struct ibv_mr			*mr;
	struct ibv_cq			*cq;
	struct ibv_qp			*qp;
	void					*buf;
	int			 			size;
	int			 			send_flags;
	int			 			rx_depth;
	int			 			pending;
	struct ibv_port_attr    portinfo;
};

struct pingpong_dest {
	unsigned int lid;
	unsigned int qpn;
	unsigned int psn;
	union ibv_gid gid;
};

static pthread_t g_server_thread;
static pthread_t g_client_thread;
int backup_client(struct addr addr_data);
int backup_server(struct addr addr_data);
void *backup_server_thread(void *arg);
void *backup_client_thread(void *arg);
void *backup_server_connection_handler(void *socket_desc);

/* Handels SIGCHLD Signal */
void sigchld_handler(int s);
/* get sockaddr, IPv4 or IPv6 */
void *get_in_addr(struct sockaddr *sa);

int pp_connect_ctx(struct pingpong_context *ctx, int port, int my_psn,
			  enum ibv_mtu mtu, int sl,
			  struct pingpong_dest *dest, int sgid_idx);
struct pingpong_context *pp_init_ctx(struct ibv_device *ib_dev, int size,
					    int rx_depth, int port, int is_server);
int pp_post_recv(struct pingpong_context *ctx, int n);
int pp_post_send(struct pingpong_context *ctx);
int pp_get_port_info(struct ibv_context *context, int port,
		     struct ibv_port_attr *attr);
int pp_close_ctx(struct pingpong_context *ctx);
void wire_gid_to_gid(const char *wgid, union ibv_gid *gid);
void gid_to_wire_gid(const union ibv_gid *gid, char wgid[]);

int rdma_init(int is_client, char *servername)
{
	srand48(getpid() * time(NULL));

	g_servername = servername;

	page_size = sysconf(_SC_PAGESIZE);

	g_dev_list = ibv_get_device_list(NULL);
	if (!g_dev_list) {
		perror("Failed to get IB devices list");
		return false;
	}

	if (!g_ib_devname) {
		g_ib_dev = *g_dev_list;
		if (!g_ib_dev) {
			fprintf(stderr, "No IB devices found\n");
			return false;
		}
	} else {
		int i;
		for (i = 0; g_dev_list[i]; ++i)
			if (!strcmp(ibv_get_device_name(g_dev_list[i]), g_ib_devname))
				break;
		g_ib_dev = g_dev_list[i];
		if (!g_ib_dev) {
			fprintf(stderr, "IB device %s not found\n", g_ib_devname);
			return false;
		}
	}

	g_ctx = pp_init_ctx(g_ib_dev, g_size, g_rx_depth, g_ib_port, !g_servername);
	if (!g_ctx)
		return false;

	g_routs = pp_post_recv(g_ctx, g_ctx->rx_depth);
	if (g_routs < g_ctx->rx_depth) {
		fprintf(stderr, "Couldn't post receive (%d)\n", g_routs);
		return false;
	}


	if (pp_get_port_info(g_ctx->context, g_ib_port, &g_ctx->portinfo)) {
		fprintf(stderr, "Couldn't get port info\n");
		return false;
	}

	g_my_dest.lid = g_ctx->portinfo.lid;
	if (g_ctx->portinfo.link_layer == IBV_LINK_LAYER_INFINIBAND && !g_my_dest.lid) {
		fprintf(stderr, "Couldn't get local LID or LID is zero\n");
		return false;
	}

	if (g_gidx >= 0) {
		if (ibv_query_gid(g_ctx->context, g_ib_port, g_gidx, &g_my_dest.gid)) {
			fprintf(stderr, "Could not get local gid for gid index %d\n", g_gidx);
			return false;
		}
	} else
		memset(&g_my_dest.gid, 0, sizeof g_my_dest.gid);

	g_my_dest.qpn = g_ctx->qp->qp_num;
	g_my_dest.psn = lrand48() & 0xffffff;
	inet_ntop(AF_INET6, &g_my_dest.gid, g_gid, sizeof g_gid);
	printf("  local address:  LID 0x%04x, QPN 0x%06x, PSN 0x%06x, GID %s\n",
			g_my_dest.lid, g_my_dest.qpn, g_my_dest.psn, g_gid);


	struct addr addr_data;
	addr_data.ip = g_servername;
	addr_data.port = g_port;
	if (is_client)
	{
		backup_client(addr_data);
	}
	else
	{
		backup_server(addr_data);
	}

	return true;
}

int backup_client(struct addr addr_data)
{
	int rv;
	struct addr	*addr = (struct addr*)malloc(sizeof(struct addr));
	addr->ip = addr_data.ip;
	addr->port = addr_data.port;

    //Create backup server thread
    rv = pthread_create(&g_client_thread, NULL, backup_client_thread, (void *)addr);
    if(rv < 0)
    {
    	printf("Error creating backup client thread\n");
    	return false;
    }

    return true;
}

void *backup_client_thread(void *arg)
{
	struct addr *addr = (struct addr *)arg;
	struct addrinfo hints, *servinfo, *p;
	int rv;
	char s[INET6_ADDRSTRLEN];
	int sockfd = -1;

	char msg[sizeof "0000:000000:000000:00000000000000000000000000000000"];
	struct pingpong_dest *rem_dest = NULL;
	char gid[33];

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	rv = getaddrinfo(addr->ip, addr->port, &hints, &servinfo);
	if (rv != 0)
	{
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
		return NULL;
	}

	// loop through all the results and connect to the first we can
	for (p = servinfo; p != NULL; p = p->ai_next)
	{
		sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
		if (sockfd == -1)
		{
			perror("client: socket\n");
			continue;
		}
		rv = connect(sockfd, p->ai_addr, p->ai_addrlen);
		if (rv == -1)
		{
			close(sockfd);
			perror("client: connect\n");
			continue;
		}

		break;
	}

	if (p == NULL)
	{
		fprintf(stderr, "client: failed to connect\n");
		return NULL;
	}

	inet_ntop(p->ai_family, get_in_addr((struct sockaddr *)p->ai_addr), s, sizeof s);
	printf("client: connecting to %s\n", s);

	freeaddrinfo(servinfo); // all done with this structure

	gid_to_wire_gid(&g_my_dest.gid, gid);
	sprintf(msg, "%04x:%06x:%06x:%s", g_my_dest.lid, g_my_dest.qpn, g_my_dest.psn, gid);

	if (write(sockfd, msg, sizeof msg) != sizeof msg) {
		fprintf(stderr, "Couldn't send local address\n");
		close(sockfd);
		return NULL;
	}

	if (read(sockfd, msg, sizeof msg) != sizeof msg) {
		perror("client read");
		fprintf(stderr, "Couldn't read remote address\n");
		close(sockfd);
		return NULL;
	}

	write(sockfd, "done", sizeof "done");

	rem_dest = malloc(sizeof *rem_dest);
	if (!rem_dest) {
		close(sockfd);
		return NULL;
	}

	sscanf(msg, "%x:%x:%x:%s", &rem_dest->lid, &rem_dest->qpn, &rem_dest->psn, gid);
	wire_gid_to_gid(gid, &rem_dest->gid);

	close(sockfd);

	inet_ntop(AF_INET6, &rem_dest->gid, gid, sizeof gid);
	printf("  remote address: LID 0x%04x, QPN 0x%06x, PSN 0x%06x, GID %s\n",
	       rem_dest->lid, rem_dest->qpn, rem_dest->psn, gid);

	if (pp_connect_ctx(g_ctx, g_ib_port, g_my_dest.psn, g_mtu, g_sl, rem_dest, g_gidx)) {
		return NULL;
	}

	g_ctx->pending = PINGPONG_RECV_WRID;

	if (pp_post_send(g_ctx)) {
		fprintf(stderr, "Couldn't post send\n");
		return NULL;
	}
	g_ctx->pending |= PINGPONG_SEND_WRID;

	if (gettimeofday(&g_start, NULL)) {
		perror("gettimeofday");
		return NULL;
	}

	g_rcnt = g_scnt = 0;
	while (g_rcnt < g_iters || g_scnt < g_iters) {
		struct ibv_wc wc[2];
		int ne, i;

		do {
			ne = ibv_poll_cq(g_ctx->cq, 2, wc);
			if (ne < 0) {
				fprintf(stderr, "poll CQ failed %d\n", ne);
				return NULL;
			}

		} while (ne < 1);

		for (i = 0; i < ne; ++i) {
			if (wc[i].status != IBV_WC_SUCCESS) {
				fprintf(stderr, "Failed status %s (%d) for wr_id %d\n",
					ibv_wc_status_str(wc[i].status),
					wc[i].status, (int) wc[i].wr_id);
				return NULL;
			}

			switch ((int) wc[i].wr_id) {
			case PINGPONG_SEND_WRID:
				++g_scnt;
				break;

			case PINGPONG_RECV_WRID:
				if (--g_routs <= 1) {
					g_routs += pp_post_recv(g_ctx, g_ctx->rx_depth - g_routs);
					if (g_routs < g_ctx->rx_depth) {
						fprintf(stderr,"Couldn't post receive (%d)\n",g_routs);
						return NULL;
					}
				}

				++g_rcnt;
				break;

			default:
				fprintf(stderr, "Completion for unknown wr_id %d\n",
					(int) wc[i].wr_id);
				return NULL;
			}

			g_ctx->pending &= ~(int) wc[i].wr_id;
			if (g_scnt < g_iters && !g_ctx->pending) {
				if (pp_post_send(g_ctx)) {
					fprintf(stderr, "Couldn't post send\n");
					return NULL;
				}
				g_ctx->pending = PINGPONG_RECV_WRID |
						   PINGPONG_SEND_WRID;
			}
		}
	}

	if (gettimeofday(&g_end, NULL)) {
		perror("gettimeofday");
		return NULL;
	}

	{
		float usec = (g_end.tv_sec - g_start.tv_sec) * 1000000 +
			(g_end.tv_usec - g_start.tv_usec);
		long long bytes = (long long) g_size * g_iters * 2;

		printf("%lld bytes in %.2f seconds = %.2f Mbit/sec\n",
		       bytes, usec / 1000000., bytes * 8. / usec);
		printf("%d iters in %.2f seconds = %.2f usec/iter\n",
		       g_iters, usec / 1000000., usec / g_iters);
	}

	ibv_ack_cq_events(g_ctx->cq, g_num_cq_events);

	if (pp_close_ctx(g_ctx))
		return NULL;

	ibv_free_device_list(g_dev_list);
	free(rem_dest);

	return NULL;
}

int backup_server(struct addr addr_data)
{
	int rv;
	struct addr	*addr = (struct addr*)malloc(sizeof(struct addr));
	addr->ip = addr_data.ip;
	addr->port = addr_data.port;

    // create backup server thread
    rv = pthread_create(&g_server_thread, NULL, backup_server_thread, (void *)addr);
    if(rv < 0)
    {
    	printf("Error creating backup server thread\n");
    	return false;
    }

    return true;
}

void *backup_server_thread(void *arg)
{
    int sockfd, new_fd;  // listen on sock_fd, new connection on new_fd
    struct addrinfo hints, *servinfo, *p;
    struct sockaddr_storage their_addr; // connector's address information
    socklen_t sin_size;
    struct sigaction sa;
    int yes = 1;
    char s[INET6_ADDRSTRLEN];
    int rv;
    struct addr *addr = (struct addr *)arg;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE; // use my IP

    rv = getaddrinfo(NULL, addr->port, &hints, &servinfo);
    if (rv != 0)
    {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
        exit(1);
    }

    // loop through all the results and bind to the first we can
    for (p = servinfo; p != NULL; p = p->ai_next)
    {
    	sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sockfd == -1)
        {
            perror("server: socket\n");
            continue;
        }

    	rv = setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));
        if (rv == -1)
        {
            perror("setsockopt\n");
            exit(1);
        }

        rv = bind(sockfd, p->ai_addr, p->ai_addrlen);
        if (rv == -1) {
            close(sockfd);
            perror("server: bind\n");
            continue;
        }

        break;
    }

    freeaddrinfo(servinfo); // all done with this structure

    if (p == NULL)
    {
        fprintf(stderr, "server: failed to bind\n");
        exit(1);
    }

    rv = listen(sockfd, BACKLOG);
    if (rv == -1)
    {
        perror("listen\n");
        exit(1);
    }

    sa.sa_handler = sigchld_handler; // reap all dead processes
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;

    rv = sigaction(SIGCHLD, &sa, NULL);
    if (rv == -1)
    {
        perror("sigaction\n");
        exit(1);
    }

    printf("server: waiting for connections...\n");

    while(1) // main accept() loop
    {
        sin_size = sizeof their_addr;
        new_fd = accept(sockfd, (struct sockaddr *)&their_addr, &sin_size);
        if (new_fd == -1)
        {
            perror("accept\n");
            continue;
        }

        inet_ntop(their_addr.ss_family, get_in_addr((struct sockaddr *)&their_addr), s, sizeof s);
        printf("server: got connection from %s\n", s);

        //Create receive thread
        pthread_t thread;
        rv = pthread_create(&thread, NULL , backup_server_connection_handler, (void*) &new_fd);
        if(rv < 0)
        {
        	printf("Error creating receive thread\n");
        }
    }

    exit(0);
}

/*
 * Exchange RDMA destination
 */
void *backup_server_connection_handler(void *socket_desc)
{
	char msg[sizeof "0000:000000:000000:00000000000000000000000000000000"];
	int read_cnt;
	int conn_fd = (int)(*(char *)socket_desc);
	struct pingpong_dest *rem_dest = NULL;
	char gid[33];

	read_cnt = read(conn_fd, msg, sizeof msg);
	if (read_cnt != sizeof msg) {
		perror("server read");
		fprintf(stderr, "%d/%d: Couldn't read remote address\n", read_cnt, (int) sizeof msg);
		close(conn_fd);
		return rem_dest;
	}

	rem_dest = malloc(sizeof *rem_dest);
	if (!rem_dest) {
		close(conn_fd);
		return NULL;
	}

	sscanf(msg, "%x:%x:%x:%s", &rem_dest->lid, &rem_dest->qpn, &rem_dest->psn, gid);
	wire_gid_to_gid(gid, &rem_dest->gid);

	if (pp_connect_ctx(g_ctx, g_ib_port, g_my_dest.psn, g_mtu, g_sl, rem_dest, g_gidx)) {
		fprintf(stderr, "Couldn't connect to remote QP\n");
		free(rem_dest);
		rem_dest = NULL;
		close(conn_fd);
		return NULL;
	}


	gid_to_wire_gid(&g_my_dest.gid, gid);
	sprintf(msg, "%04x:%06x:%06x:%s", g_my_dest.lid, g_my_dest.qpn, g_my_dest.psn, gid);
	if (write(conn_fd, msg, sizeof msg) != sizeof msg) {
		fprintf(stderr, "Couldn't send local address\n");
		free(rem_dest);
		rem_dest = NULL;
		close(conn_fd);
		return NULL;
	}

	read(conn_fd, msg, sizeof msg);

	close(conn_fd);

	if (!rem_dest)
		return NULL;

	inet_ntop(AF_INET6, &rem_dest->gid, gid, sizeof gid);
	printf("  remote address: LID 0x%04x, QPN 0x%06x, PSN 0x%06x, GID %s\n",
		   rem_dest->lid, rem_dest->qpn, rem_dest->psn, gid);

	g_ctx->pending = PINGPONG_RECV_WRID;

	if (gettimeofday(&g_start, NULL)) {
		perror("gettimeofday");
		return NULL;
	}

	g_rcnt = g_scnt = 0;

	while (g_rcnt < g_iters || g_scnt < g_iters) {
		struct ibv_wc wc[2];
		int ne, i;

		do {
			ne = ibv_poll_cq(g_ctx->cq, 2, wc);
			if (ne < 0) {
				fprintf(stderr, "poll CQ failed %d\n", ne);
				return NULL;
			}

		} while (ne < 1);

		for (i = 0; i < ne; ++i) {
			if (wc[i].status != IBV_WC_SUCCESS) {
				fprintf(stderr, "Failed status %s (%d) for wr_id %d\n",
					ibv_wc_status_str(wc[i].status),
					wc[i].status, (int) wc[i].wr_id);
				return NULL;
			}

			switch ((int) wc[i].wr_id) {
			case PINGPONG_SEND_WRID:
				++g_scnt;
				break;

			case PINGPONG_RECV_WRID:
				if (--g_routs <= 1) {
					g_routs += pp_post_recv(g_ctx, g_ctx->rx_depth - g_routs);
					if (g_routs < g_ctx->rx_depth) {
						fprintf(stderr,"Couldn't post receive (%d)\n", g_routs);
						return NULL;
					}
				}

				++g_rcnt;
				break;

			default:
				fprintf(stderr, "Completion for unknown wr_id %d\n",
					(int) wc[i].wr_id);
				return NULL;
			}

			g_ctx->pending &= ~(int) wc[i].wr_id;
			if (g_scnt < g_iters && !g_ctx->pending) {
				if (pp_post_send(g_ctx)) {
					fprintf(stderr, "Couldn't post send\n");
					return NULL;
				}
				g_ctx->pending = PINGPONG_RECV_WRID |
						   PINGPONG_SEND_WRID;
			}
		}

	}

	if (gettimeofday(&g_end, NULL)) {
		perror("gettimeofday");
		return NULL;
	}

	{
		float usec = (g_end.tv_sec - g_start.tv_sec) * 1000000 +
			(g_end.tv_usec - g_start.tv_usec);
		long long bytes = (long long) g_size * g_iters * 2;

		printf("%lld bytes in %.2f seconds = %.2f Mbit/sec\n",
			   bytes, usec / 1000000., bytes * 8. / usec);
		printf("%d iters in %.2f seconds = %.2f usec/iter\n",
			   g_iters, usec / 1000000., usec / g_iters);
	}

	ibv_ack_cq_events(g_ctx->cq, g_num_cq_events);

	if (pp_close_ctx(g_ctx))
		return NULL;

	ibv_free_device_list(g_dev_list);
	free(rem_dest);

	return NULL;
}

struct pingpong_context *pp_init_ctx(struct ibv_device *ib_dev, int size,
					    int rx_depth, int port, int is_server)
{
	struct pingpong_context *ctx;

	ctx = calloc(1, sizeof *ctx);
	if (!ctx)
		return NULL;

	ctx->size     = size;
	ctx->rx_depth = rx_depth;

	ctx->buf = malloc(roundup(size, page_size));
	if (!ctx->buf) {
		fprintf(stderr, "Couldn't allocate work buf.\n");
		return NULL;
	}

	memset(ctx->buf, 0x7b + is_server, size);

	ctx->context = ibv_open_device(ib_dev);
	if (!ctx->context) {
		fprintf(stderr, "Couldn't get context for %s\n",
			ibv_get_device_name(ib_dev));
		return NULL;
	}

	ctx->channel = NULL;

	ctx->pd = ibv_alloc_pd(ctx->context);
	if (!ctx->pd) {
		fprintf(stderr, "Couldn't allocate PD\n");
		return NULL;
	}

	ctx->mr = ibv_reg_mr(ctx->pd, ctx->buf, size, IBV_ACCESS_LOCAL_WRITE);
	if (!ctx->mr) {
		fprintf(stderr, "Couldn't register MR\n");
		return NULL;
	}

	ctx->cq = ibv_create_cq(ctx->context, rx_depth + 1, NULL,
				ctx->channel, 0);
	if (!ctx->cq) {
		fprintf(stderr, "Couldn't create CQ\n");
		return NULL;
	}

	{
		struct ibv_qp_init_attr attr = {
			.send_cq = ctx->cq,
			.recv_cq = ctx->cq,
			.cap     = {
				.max_send_wr  = 1,
				.max_recv_wr  = rx_depth,
				.max_send_sge = 1,
				.max_recv_sge = 1
			},
			.qp_type = IBV_QPT_RC
		};

		ctx->qp = ibv_create_qp(ctx->pd, &attr);
		if (!ctx->qp)  {
			fprintf(stderr, "Couldn't create QP\n");
			return NULL;
		}
	}

	{
		struct ibv_qp_attr attr = {
			.qp_state        = IBV_QPS_INIT,
			.pkey_index      = 0,
			.port_num        = port,
			.qp_access_flags = 0
		};

		if (ibv_modify_qp(ctx->qp, &attr,
				  IBV_QP_STATE              |
				  IBV_QP_PKEY_INDEX         |
				  IBV_QP_PORT               |
				  IBV_QP_ACCESS_FLAGS)) {
			fprintf(stderr, "Failed to modify QP to INIT\n");
			return NULL;
		}
	}

	return ctx;
}

int pp_connect_ctx(struct pingpong_context *ctx, int port, int my_psn,
			  enum ibv_mtu mtu, int sl,
			  struct pingpong_dest *dest, int sgid_idx)
{
	struct ibv_qp_attr attr = {
		.qp_state		= IBV_QPS_RTR,
		.path_mtu		= mtu,
		.dest_qp_num		= dest->qpn,
		.rq_psn			= dest->psn,
		.max_dest_rd_atomic	= 1,
		.min_rnr_timer		= 12,
		.ah_attr		= {
			.is_global	= 0,
			.dlid		= dest->lid,
			.sl		= sl,
			.src_path_bits	= 0,
			.port_num	= port
		}
	};

	if (dest->gid.global.interface_id) {
		attr.ah_attr.is_global = 1;
		attr.ah_attr.grh.hop_limit = 1;
		attr.ah_attr.grh.dgid = dest->gid;
		attr.ah_attr.grh.sgid_index = sgid_idx;
	}
	if (ibv_modify_qp(ctx->qp, &attr,
			  IBV_QP_STATE              |
			  IBV_QP_AV                 |
			  IBV_QP_PATH_MTU           |
			  IBV_QP_DEST_QPN           |
			  IBV_QP_RQ_PSN             |
			  IBV_QP_MAX_DEST_RD_ATOMIC |
			  IBV_QP_MIN_RNR_TIMER)) {
		fprintf(stderr, "Failed to modify QP to RTR\n");
		return 1;
	}

	attr.qp_state	    = IBV_QPS_RTS;
	attr.timeout	    = 14;
	attr.retry_cnt	    = 7;
	attr.rnr_retry	    = 7;
	attr.sq_psn	    = my_psn;
	attr.max_rd_atomic  = 1;
	if (ibv_modify_qp(ctx->qp, &attr,
			  IBV_QP_STATE              |
			  IBV_QP_TIMEOUT            |
			  IBV_QP_RETRY_CNT          |
			  IBV_QP_RNR_RETRY          |
			  IBV_QP_SQ_PSN             |
			  IBV_QP_MAX_QP_RD_ATOMIC)) {
		fprintf(stderr, "Failed to modify QP to RTS\n");
		return 1;
	}

	return 0;
}


int pp_close_ctx(struct pingpong_context *ctx)
{
	if (ibv_destroy_qp(ctx->qp)) {
		fprintf(stderr, "Couldn't destroy QP\n");
		return 1;
	}

	if (ibv_destroy_cq(ctx->cq)) {
		fprintf(stderr, "Couldn't destroy CQ\n");
		return 1;
	}

	if (ibv_dereg_mr(ctx->mr)) {
		fprintf(stderr, "Couldn't deregister MR\n");
		return 1;
	}

	if (ibv_dealloc_pd(ctx->pd)) {
		fprintf(stderr, "Couldn't deallocate PD\n");
		return 1;
	}

	if (ctx->channel) {
		if (ibv_destroy_comp_channel(ctx->channel)) {
			fprintf(stderr, "Couldn't destroy completion channel\n");
			return 1;
		}
	}

	if (ibv_close_device(ctx->context)) {
		fprintf(stderr, "Couldn't release context\n");
		return 1;
	}

	free(ctx->buf);
	free(ctx);

	return 0;
}


int pp_post_recv(struct pingpong_context *ctx, int n)
{
	struct ibv_sge list = {
		.addr	= (uintptr_t) ctx->buf,
		.length = ctx->size,
		.lkey	= ctx->mr->lkey
	};
	struct ibv_recv_wr wr = {
		.wr_id	    = PINGPONG_RECV_WRID,
		.sg_list    = &list,
		.num_sge    = 1,
	};
	struct ibv_recv_wr *bad_wr;
	int i;

	for (i = 0; i < n; ++i)
		if (ibv_post_recv(ctx->qp, &wr, &bad_wr))
			break;

	return i;
}

int pp_post_send(struct pingpong_context *ctx)
{
	struct ibv_sge list = {
		.addr	= (uintptr_t) ctx->buf,
		.length = ctx->size,
		.lkey	= ctx->mr->lkey
	};
	struct ibv_send_wr wr = {
		.wr_id	    = PINGPONG_SEND_WRID,
		.sg_list    = &list,
		.num_sge    = 1,
		.opcode     = IBV_WR_SEND,
		.send_flags = IBV_SEND_SIGNALED,
	};
	struct ibv_send_wr *bad_wr;

	return ibv_post_send(ctx->qp, &wr, &bad_wr);
}

int pp_get_port_info(struct ibv_context *context, int port,
		     struct ibv_port_attr *attr)
{
	return ibv_query_port(context, port, attr);
}

void wire_gid_to_gid(const char *wgid, union ibv_gid *gid)
{
	char tmp[9];
	// uint32_t v32;
	uint32_t v32, v32n;
	int i;

	for (tmp[8] = 0, i = 0; i < 4; ++i) {
		memcpy(tmp, wgid + i * 8, 8);
		sscanf(tmp, "%x", &v32);
		// done to fix strict aliasing
		// *(uint32_t *)(&gid->raw[i * 4]) = ntohl(v32);
		v32n = ntohl(v32);
		memcpy(&gid->raw[i*4], &v32n, sizeof(v32n));
	}
}

void gid_to_wire_gid(const union ibv_gid *gid, char wgid[])
{
	int i;
	uint32_t v32;

	/* done to fix strict aliasing
	for (i = 0; i < 4; ++i)
		sprintf(&wgid[i * 8], "%08x",
			htonl(*(uint32_t *)(gid->raw + i * 4)));
	*/
	for (i = 0; i < 4; ++i) {
		memcpy(&v32, gid->raw + i * 4, sizeof(v32));
		sprintf(&wgid[i * 8], "%08x", htonl(v32));
	}
}

/*
 * Handels SIGCHLD Signal
 */
void sigchld_handler(int s)
{
    // waitpid() might overwrite errno, so we save and restore it:
    int saved_errno = errno;

    while(waitpid(-1, NULL, WNOHANG) > 0);

    errno = saved_errno;
}

/*
 * get sockaddr, IPv4 or IPv6
 */
void *get_in_addr(struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET)
    {
        return &(((struct sockaddr_in*)sa)->sin_addr);
    }

    return &(((struct sockaddr_in6*)sa)->sin6_addr);
}
