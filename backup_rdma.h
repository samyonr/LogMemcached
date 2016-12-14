/*
 * backup_rdma.h
 *
 */

#ifndef BACKUP_RDMA_H_
#define BACKUP_RDMA_H_


struct addr {
	char		*ip;
	char		*port;
};

int rdma_init(int is_client, char *servername);

#endif /* BACKUP_RDMA_H_ */
