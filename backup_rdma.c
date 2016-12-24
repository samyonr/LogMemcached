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
#define BACKUP_IP_PORT "18515" // hard coded for now
#define REPLICATION_CHUNK (1024 * 1024 * 2) // 2MB


#ifdef REPLICATION_BENCHMARK
//#define RB_ARRAY_SIZE (1024 * 1024 * 1024)
#define RB_ARRAY_SIZE (1024)
struct timespec *g_rb_item_time;
unsigned long g_rb_current = 0;
#endif

static int g_page_size;

struct backup_ibv_context {
	struct ibv_context *context;
	struct ibv_comp_channel *channel;
	struct ibv_pd *pd;
	struct ibv_mr *mr;
	struct ibv_cq *cq;
	struct ibv_qp *qp;
	void *buf;
	int size;
	int send_flags;
	int rx_depth;
	struct ibv_port_attr portinfo;
};

struct backup_ibv_dest {
	unsigned int lid;
	unsigned int qpn;
	unsigned int psn;
	unsigned long remote_address;
	unsigned int remote_key;
	union ibv_gid gid;
};

struct backup_meta {
	struct ibv_device **ibv_device_list;
	struct ibv_device *ibv_device;
	struct backup_ibv_context *ctx;
	struct backup_ibv_dest ibv_dest;
	struct ip_addr ip_dest;
	enum ibv_mtu mtu;
	int rx_depth;
	int sl;
	int sgid_index;
	char gid[33];
	int ibv_port;
};

struct backup_meta g_backup_meta = {
		.mtu = IBV_MTU_1024,
		.rx_depth = 500,
		.sl = 0,
		.sgid_index = -1,
		.ibv_port = -1
};

static pthread_t g_server_thread;
static pthread_t g_client_thread;
int backup_client(struct ip_addr addr_data);
int backup_server(struct ip_addr addr_data);
void *backup_server_thread(void *arg);
void *backup_client_thread(void *arg);
void *backup_server_connection_handler(void *socket_desc);
void backup_client_replication_handler(struct backup_ibv_dest *rem_dest);

/* Handels SIGCHLD Signal */
void sigchld_handler(int s);
/* get sockaddr, IPv4 or IPv6 */
void *get_in_addr(struct sockaddr *sa);

int connect_ctx(struct backup_ibv_context *ctx, int port, int my_psn, enum ibv_mtu mtu, int sl, struct backup_ibv_dest *dest, int sgid_index);
struct backup_ibv_context *init_ctx(struct ibv_device *ibv_device, int size, int rx_depth, int port, int is_server);
int close_ctx(struct backup_ibv_context *ctx);
void wire_gid_to_gid(const char *wgid, union ibv_gid *gid);
void gid_to_wire_gid(const union ibv_gid *gid, char wgid[]);

int rdma_init(int is_client, char *server_name, char *ibv_device_name, int ibv_port, int sgid_index) {
	srand48(getpid() * time(NULL));

#ifdef REPLICATION_BENCHMARK
	g_rb_item_time = malloc(RB_ARRAY_SIZE * sizeof(struct timespec));
	if (g_rb_item_time == NULL) {
		exit(1);
	}
	memset(g_rb_item_time, 0, RB_ARRAY_SIZE);
	g_rb_current = 0;
#endif

	if (ibv_port > 0) {
		g_backup_meta.ibv_port = ibv_port;
	}

	if (sgid_index > -1) {
		g_backup_meta.sgid_index = sgid_index;
	}

	g_backup_meta.ip_dest.ip = server_name;
	g_backup_meta.ip_dest.port = BACKUP_IP_PORT; // default, not configurable yet

	g_page_size = sysconf(_SC_PAGESIZE);

	g_backup_meta.ibv_device_list = ibv_get_device_list(NULL);
	if (!g_backup_meta.ibv_device_list) {
		perror("Failed to get IB devices list");
		return false;
	}

	if (!ibv_device_name) { // preferable ibv device
		g_backup_meta.ibv_device = *g_backup_meta.ibv_device_list;
		if (!g_backup_meta.ibv_device) {
			fprintf(stderr, "No IB devices found\n");
			return false;
		}
	} else {
		int i;
		for (i = 0; g_backup_meta.ibv_device_list[i]; ++i) {
			if (!strcmp(ibv_get_device_name(g_backup_meta.ibv_device_list[i]), ibv_device_name))
				break;
		}
		g_backup_meta.ibv_device = g_backup_meta.ibv_device_list[i];
		if (!g_backup_meta.ibv_device) {
			fprintf(stderr, "IB device %s not found\n", ibv_device_name);
			return false;
		}
	}

	g_backup_meta.ctx = init_ctx(g_backup_meta.ibv_device, get_memory_limit(),
			g_backup_meta.rx_depth, g_backup_meta.ibv_port, !g_backup_meta.ip_dest.ip);

	if (!g_backup_meta.ctx) {
		return false;
	}

	if (ibv_query_port(g_backup_meta.ctx->context, g_backup_meta.ibv_port, &g_backup_meta.ctx->portinfo)) {
		fprintf(stderr, "Couldn't get port info\n");
		return false;
	}

	g_backup_meta.ibv_dest.lid = g_backup_meta.ctx->portinfo.lid;
	if (g_backup_meta.ctx->portinfo.link_layer == IBV_LINK_LAYER_INFINIBAND
			&& !g_backup_meta.ibv_dest.lid) {
		fprintf(stderr, "Couldn't get local LID or LID is zero\n");
		return false;
	}

	if (g_backup_meta.sgid_index >= 0) {
		if (ibv_query_gid(g_backup_meta.ctx->context, g_backup_meta.ibv_port,
				g_backup_meta.sgid_index, &g_backup_meta.ibv_dest.gid)) {
			fprintf(stderr, "Could not get local gid for gid index %d\n",
					g_backup_meta.sgid_index);
			return false;
		}
	} else
		memset(&g_backup_meta.ibv_dest.gid, 0, sizeof g_backup_meta.ibv_dest.gid);

	g_backup_meta.ibv_dest.qpn = g_backup_meta.ctx->qp->qp_num;
	g_backup_meta.ibv_dest.psn = lrand48() & 0xffffff;
	inet_ntop(AF_INET6, &g_backup_meta.ibv_dest.gid, g_backup_meta.gid, sizeof g_backup_meta.gid);
	printf("  local address:  LID 0x%04x, QPN 0x%06x, PSN 0x%06x, GID %s\n",
			g_backup_meta.ibv_dest.lid, g_backup_meta.ibv_dest.qpn,
			g_backup_meta.ibv_dest.psn, g_backup_meta.gid);


	if (is_client) {
		backup_client(g_backup_meta.ip_dest);
	} else {
		backup_server(g_backup_meta.ip_dest);
	}

	return true;
}

#ifdef REPLICATION_BENCHMARK
void rb_write_time(int just_print) {
	if (!just_print) {
		clock_gettime(CLOCK_REALTIME, &g_rb_item_time[g_rb_current]);
		g_rb_current++;
		if (g_rb_current >= RB_ARRAY_SIZE) {
			for (long i = 0; i < RB_ARRAY_SIZE; i++) {
				printf("%lu rb time: %"PRIdMAX".%03ld seconds since the Epoch\n",
					   i, (intmax_t)g_rb_item_time[i].tv_sec, g_rb_item_time[i].tv_nsec);
			}
			exit(1);
		}
	} else {
		for (long i = 0; i < g_rb_current; i++) {
			printf("%lu rb time: %"PRIdMAX".%03ld seconds since the Epoch\n",
					i, (intmax_t)g_rb_item_time[i].tv_sec, g_rb_item_time[i].tv_nsec);
		}
		exit(1);
	}
}
#endif

int backup_client(struct ip_addr addr_data) {
	int rv;
	struct ip_addr *addr = (struct ip_addr*) malloc(sizeof(struct ip_addr));
	addr->ip = addr_data.ip;
	addr->port = addr_data.port;

	//Create backup server thread
	rv = pthread_create(&g_client_thread, NULL, backup_client_thread,
			(void *) addr);
	if (rv < 0) {
		printf("Error creating backup client thread\n");
		return false;
	}

	return true;
}

void *backup_client_thread(void *arg) {
	struct ip_addr *addr = (struct ip_addr *) arg;
	struct addrinfo hints, *servinfo, *p;
	int rv;
	char s[INET6_ADDRSTRLEN];
	int sockfd = -1;

	char msg[sizeof "0000:000000:000000:00000000000000000000000000000000"];
	char msg2[128];
	struct backup_ibv_dest *rem_dest = NULL;
	char gid[33];

	// create TCP connection
	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	rv = getaddrinfo(addr->ip, addr->port, &hints, &servinfo);
	if (rv != 0) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
		return NULL;
	}

	// loop through all the results and connect to the first we can
	for (p = servinfo; p != NULL; p = p->ai_next) {
		sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
		if (sockfd == -1) {
			perror("client: socket\n");
			continue;
		}
		rv = connect(sockfd, p->ai_addr, p->ai_addrlen);
		if (rv == -1) {
			close(sockfd);
			perror("client: connect\n");
			continue;
		}

		break;
	}

	if (p == NULL) {
		fprintf(stderr, "client: failed to connect\n");
		return NULL;
	}

	inet_ntop(p->ai_family, get_in_addr((struct sockaddr *) p->ai_addr), s,
			sizeof s);
	printf("client: connecting to %s\n", s);

	freeaddrinfo(servinfo); // all done with this structure

	// exchange ibv information
	gid_to_wire_gid(&g_backup_meta.ibv_dest.gid, gid);
	sprintf(msg, "%04x:%06x:%06x:%s", g_backup_meta.ibv_dest.lid,
			g_backup_meta.ibv_dest.qpn, g_backup_meta.ibv_dest.psn, gid);

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

	if (read(sockfd, msg2, sizeof msg2) != sizeof msg2) {
		perror("client read");
		fprintf(stderr, "Couldn't read remote buffer address\n");
		close(sockfd);
		return NULL;
	}

	sscanf(msg2, "RDMA addr %lx rkey %x", &rem_dest->remote_address, &rem_dest->remote_key);
	printf("RDMA addr %lx rkey %x\n",rem_dest->remote_address,rem_dest->remote_key);
	rem_dest->remote_key = ntohl(rem_dest->remote_key);
	rem_dest->remote_address = ntohll(rem_dest->remote_address);
	printf("RDMA addr %lx rkey %x\n",rem_dest->remote_address,rem_dest->remote_key);

	write(sockfd, "done", sizeof "done");

	// ibv information exchanged, close socket
	close(sockfd);

	inet_ntop(AF_INET6, &rem_dest->gid, gid, sizeof gid);
	printf("  remote address: LID 0x%04x, QPN 0x%06x, PSN 0x%06x, GID %s\n",
			rem_dest->lid, rem_dest->qpn, rem_dest->psn, gid);

	// connect ibv qp
	if (connect_ctx(g_backup_meta.ctx, g_backup_meta.ibv_port,
			g_backup_meta.ibv_dest.psn, g_backup_meta.mtu, g_backup_meta.sl, rem_dest,
			g_backup_meta.sgid_index)) {
		return NULL;
	}

	backup_client_replication_handler(rem_dest);

	if (close_ctx(g_backup_meta.ctx))
		return NULL;

	ibv_free_device_list(g_backup_meta.ibv_device_list);
	free(rem_dest);

	return NULL;
}

int backup_server(struct ip_addr addr_data) {
	int rv;
	struct ip_addr *addr = (struct ip_addr*) malloc(sizeof(struct ip_addr));
	addr->ip = addr_data.ip;
	addr->port = addr_data.port;

	// create backup server thread
	rv = pthread_create(&g_server_thread, NULL, backup_server_thread,
			(void *) addr);
	if (rv < 0) {
		printf("Error creating backup server thread\n");
		return false;
	}

	return true;
}

void *backup_server_thread(void *arg) {
	int sockfd, new_fd;  // listen on sock_fd, new connection on new_fd
	struct addrinfo hints, *servinfo, *p;
	struct sockaddr_storage their_addr; // connector's address information
	socklen_t sin_size;
	struct sigaction sa;
	int yes = 1;
	char s[INET6_ADDRSTRLEN];
	int rv;
	struct ip_addr *addr = (struct ip_addr *) arg;

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE; // use my IP

	rv = getaddrinfo(NULL, addr->port, &hints, &servinfo);
	if (rv != 0) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
		exit(1);
	}

	// loop through all the results and bind to the first we can
	for (p = servinfo; p != NULL; p = p->ai_next) {
		sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
		if (sockfd == -1) {
			perror("server: socket\n");
			continue;
		}

		rv = setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));
		if (rv == -1) {
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

	if (p == NULL) {
		fprintf(stderr, "server: failed to bind\n");
		exit(1);
	}

	rv = listen(sockfd, BACKLOG);
	if (rv == -1) {
		perror("listen\n");
		exit(1);
	}

	sa.sa_handler = sigchld_handler; // reap all dead processes
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART;

	rv = sigaction(SIGCHLD, &sa, NULL);
	if (rv == -1) {
		perror("sigaction\n");
		exit(1);
	}

	printf("server: waiting for connections...\n");

	while (1) // main accept() loop
	{
		sin_size = sizeof their_addr;
		new_fd = accept(sockfd, (struct sockaddr *) &their_addr, &sin_size);
		if (new_fd == -1) {
			perror("accept\n");
			continue;
		}

		inet_ntop(their_addr.ss_family,
				get_in_addr((struct sockaddr *) &their_addr), s, sizeof s);
		printf("server: got connection from %s\n", s);

		//Create receive thread
		pthread_t thread;
		rv = pthread_create(&thread, NULL, backup_server_connection_handler,
				(void*) &new_fd);
		if (rv < 0) {
			printf("Error creating receive thread\n");
		}
	}

	exit(0);
}

/*
 * Exchange RDMA destination
 */
void *backup_server_connection_handler(void *socket_desc) {
	char msg[sizeof "0000:000000:000000:00000000000000000000000000000000"];
	char msg2[128];
	int read_cnt;
	int conn_fd = (int) (*(char *) socket_desc);
	struct backup_ibv_dest *rem_dest = NULL;
	char gid[33];

	read_cnt = read(conn_fd, msg, sizeof msg);
	if (read_cnt != sizeof msg) {
		perror("server read");
		fprintf(stderr, "%d/%d: Couldn't read remote address\n", read_cnt,
				(int) sizeof msg);
		close(conn_fd);
		return rem_dest;
	}

	rem_dest = malloc(sizeof *rem_dest);
	if (!rem_dest) {
		close(conn_fd);
		return NULL;
	}

	sscanf(msg, "%x:%x:%x:%s", &rem_dest->lid, &rem_dest->qpn, &rem_dest->psn,
			gid);
	wire_gid_to_gid(gid, &rem_dest->gid);

	if (connect_ctx(g_backup_meta.ctx, g_backup_meta.ibv_port,
			g_backup_meta.ibv_dest.psn, g_backup_meta.mtu, g_backup_meta.sl, rem_dest,
			g_backup_meta.sgid_index)) {
		fprintf(stderr, "Couldn't connect to remote QP\n");
		free(rem_dest);
		rem_dest = NULL;
		close(conn_fd);
		return NULL;
	}

	gid_to_wire_gid(&g_backup_meta.ibv_dest.gid, gid);
	sprintf(msg, "%04x:%06x:%06x:%s", g_backup_meta.ibv_dest.lid,
			g_backup_meta.ibv_dest.qpn, g_backup_meta.ibv_dest.psn, gid);
	if (write(conn_fd, msg, sizeof msg) != sizeof msg) {
		fprintf(stderr, "Couldn't send local address\n");
		free(rem_dest);
		rem_dest = NULL;
		close(conn_fd);
		return NULL;
	}

	read(conn_fd, msg, sizeof msg); // read done

	sprintf(msg2, "RDMA addr %lx rkey %x", htonll((uint64_t) (unsigned long) g_backup_meta.ctx->buf),
			htonl(g_backup_meta.ctx->mr->rkey));
	printf("RDMA addr %lx rkey %x\n", htonll((uint64_t) (unsigned long) g_backup_meta.ctx->buf),
				htonl(g_backup_meta.ctx->mr->rkey));
	if (write(conn_fd, msg2, sizeof msg2) != sizeof msg2) {
		fprintf(stderr, "Couldn't send local buffer address\n");
		free(rem_dest);
		rem_dest = NULL;
		close(conn_fd);
		return NULL;
	}

	read(conn_fd, msg, sizeof msg); // read done

	close(conn_fd);

	if (!rem_dest)
		return NULL;

	inet_ntop(AF_INET6, &rem_dest->gid, gid, sizeof gid);
	printf("  remote address: LID 0x%04x, QPN 0x%06x, PSN 0x%06x, GID %s\n",
			rem_dest->lid, rem_dest->qpn, rem_dest->psn, gid);

	while(1);

	if (close_ctx(g_backup_meta.ctx))
		return NULL;

	ibv_free_device_list(g_backup_meta.ibv_device_list);
	free(rem_dest);

	return NULL;
}

struct backup_ibv_context *init_ctx(struct ibv_device *ibv_device, int size,
		int rx_depth, int port, int is_server) {
	struct backup_ibv_context *ctx;

	ctx = calloc(1, sizeof *ctx);
	if (!ctx)
		return NULL;

	ctx->size = size;
	ctx->rx_depth = rx_depth;

	ctx->buf = get_memory_base();

	ctx->context = ibv_open_device(ibv_device);
	if (!ctx->context) {
		fprintf(stderr, "Couldn't get context for %s\n",
				ibv_get_device_name(ibv_device));
		return NULL;
	}

	ctx->channel = NULL;

	ctx->pd = ibv_alloc_pd(ctx->context);
	if (!ctx->pd) {
		fprintf(stderr, "Couldn't allocate PD\n");
		return NULL;
	}

	ctx->mr = ibv_reg_mr(ctx->pd, ctx->buf, size, IBV_ACCESS_REMOTE_READ | IBV_ACCESS_LOCAL_WRITE);
	if (!ctx->mr) {
		fprintf(stderr, "Couldn't register MR\n");
		return NULL;
	}

	ctx->cq = ibv_create_cq(ctx->context, rx_depth + 1, NULL, ctx->channel, 0);
	if (!ctx->cq) {
		fprintf(stderr, "Couldn't create CQ\n");
		return NULL;
	}

	{
		struct ibv_qp_init_attr attr = {
				.send_cq = ctx->cq,
				.recv_cq = ctx->cq,
				.cap = {
						.max_send_wr = 1,
						.max_recv_wr = rx_depth,
						.max_send_sge = 1,
						.max_recv_sge = 1
				},
				.qp_type = IBV_QPT_RC,
				.sq_sig_all = 1
		};

		ctx->qp = ibv_create_qp(ctx->pd, &attr);
		if (!ctx->qp) {
			fprintf(stderr, "Couldn't create QP\n");
			return NULL;
		}
	}

	{
		struct ibv_qp_attr attr = {
				.qp_state = IBV_QPS_INIT,
				.pkey_index = 0,
				.port_num = port,
				.qp_access_flags = IBV_ACCESS_REMOTE_READ
		};

		if (ibv_modify_qp(ctx->qp, &attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS)) {
			fprintf(stderr, "Failed to modify QP to INIT\n");
			return NULL;
		}
	}

	return ctx;
}

int connect_ctx(struct backup_ibv_context *ctx, int port, int my_psn,
		enum ibv_mtu mtu, int sl, struct backup_ibv_dest *dest, int sgid_index) {
	struct ibv_qp_attr attr =
			{
					.qp_state = IBV_QPS_RTR,
					.path_mtu = mtu,
					.dest_qp_num = dest->qpn,
					.rq_psn = dest->psn,
					.max_dest_rd_atomic = 1,
					.min_rnr_timer = 12,
					.ah_attr = {
							.is_global = 0,
							.dlid = dest->lid,
							.sl = sl,
							.src_path_bits = 0,
							.port_num = port
					}
			};

	if (dest->gid.global.interface_id) {
		attr.ah_attr.is_global = 1;
		attr.ah_attr.grh.hop_limit = 1;
		attr.ah_attr.grh.dgid = dest->gid;
		attr.ah_attr.grh.sgid_index = sgid_index;
	}
	if (ibv_modify_qp(ctx->qp, &attr,
			IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN
					| IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC
					| IBV_QP_MIN_RNR_TIMER)) {
		fprintf(stderr, "Failed to modify QP to RTR\n");
		return 1;
	}

	attr.qp_state = IBV_QPS_RTS;
	attr.timeout = 14;
	attr.retry_cnt = 7;
	attr.rnr_retry = 7;
	attr.sq_psn = my_psn;
	attr.max_rd_atomic = 1;
	if (ibv_modify_qp(ctx->qp, &attr,
			IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY
					| IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC)) {
		fprintf(stderr, "Failed to modify QP to RTS\n");
		return 1;
	}

	return 0;
}

int close_ctx(struct backup_ibv_context *ctx) {
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

void wire_gid_to_gid(const char *wgid, union ibv_gid *gid) {
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
		memcpy(&gid->raw[i * 4], &v32n, sizeof(v32n));
	}
}

void gid_to_wire_gid(const union ibv_gid *gid, char wgid[]) {
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
void sigchld_handler(int s) {
	// waitpid() might overwrite errno, so we save and restore it:
	int saved_errno = errno;

	while (waitpid(-1, NULL, WNOHANG) > 0)
		;

	errno = saved_errno;
}

/*
 * get sockaddr, IPv4 or IPv6
 */
void *get_in_addr(struct sockaddr *sa) {
	if (sa->sa_family == AF_INET) {
		return &(((struct sockaddr_in*) sa)->sin_addr);
	}

	return &(((struct sockaddr_in6*) sa)->sin6_addr);
}


void backup_client_replication_handler(struct backup_ibv_dest *rem_dest) {
	int run = true;
	//TODO: stop running if connection is close
	//TODO: stop running if someone from outside called for stop running
	uint32_t replication_offset = 0;
	unsigned long long term = 0;
	while (run)
	{
		term++;
		uint32_t size_to_replicate;
		if (REPLICATION_CHUNK < g_backup_meta.ctx->size - replication_offset) {
			size_to_replicate = REPLICATION_CHUNK;
		} else {
			size_to_replicate = g_backup_meta.ctx->size - replication_offset;
		}

		/*
		 * FIXME: in case the replicating machine it too slow, a problem can occur here
		 * it can be easily reproduced with REPLICATION_CHUNK of size 128B, and memlog size of 256B
		 * by using memlog as the destination buffer (sg_list), we can replicate already cleaned memory
		 * directly into the memlog, causing the maintenance thread to go crazy, reaching empty space,
		 * and not knowing the size of next item (it will thing that its size is 17B, but its not a real item)
		 * The solution for that is to create a separate destination buffer, wit REPLICATION_CHUNK size,
		 * replicate memory to that buffer, and then copy internally only the real items, without copying empty space.
		 * In real system, with fast enough replication, that should not occur.
		 */
		struct ibv_sge list = {
				.addr = (uintptr_t) ((char *)g_backup_meta.ctx->buf + replication_offset),
				.length = size_to_replicate,
				.lkey = g_backup_meta.ctx->mr->lkey
		};

		struct ibv_send_wr wr = {
				.wr_id = 0,
				.sg_list = &list,
				.num_sge = 1,
				.opcode = IBV_WR_RDMA_READ,
				.send_flags = 0,
				.wr.rdma.remote_addr = rem_dest->remote_address + replication_offset,
				.wr.rdma.rkey        = rem_dest->remote_key
		};
		struct ibv_send_wr *bad_wr;

		int res = 0;
		int failed_res_count = 0;
		do {
			res = ibv_post_send(g_backup_meta.ctx->qp, &wr, &bad_wr);
			if (failed_res_count > 100) {

#ifdef REPLICATION_BENCHMARK
				rb_write_time(true);
#endif

				exit(1);
			}
			if (res != 0) {
				failed_res_count++;
				//printf ("%llu res is %d\n",term, res);
				usleep(1000); //synchronization with the HW?
			}
		} while (res != 0);
		int ne = 0;
		do {
			struct ibv_wc wc;
			ne = ibv_poll_cq(g_backup_meta.ctx->cq, 1, &wc);
			if (ne < 0) {
					fprintf(stderr, "Failed to poll completions from the CQ: ret = %d\n", ne);
					exit(1);
			}
			/* there may be an extra event with no completion in the CQ */
			if (ne == 0)
					continue;

			if (wc.status != IBV_WC_SUCCESS) {
					fprintf(stderr, "Completion with status 0x%x was found\n", wc.status);
					exit(1);
			}
		} while (ne);

		replication_offset = do_store_replication(g_backup_meta.ctx->buf, size_to_replicate, replication_offset);
		usleep(1000); //synchronization with the HW?
	}
}
