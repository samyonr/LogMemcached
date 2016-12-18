/*
 * backup_rdma.h
 *
 */

#ifndef BACKUP_RDMA_H_
#define BACKUP_RDMA_H_


struct ip_addr {
	char		*ip;
	char		*port;
};

int rdma_init(int is_client, char *server_name, char *ibv_device_name);

#endif /* BACKUP_RDMA_H_ */
