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

int rdma_init(int is_client, char *server_name, char *ibv_device_name, int ibv_port, int sgid_index);

#ifdef REPLICATION_BENCHMARK
void rb_write_time(int just_print, int sparse);
unsigned long int get_current_seconds(void);
unsigned long int get_current_useconds(void);
#endif

#endif /* BACKUP_RDMA_H_ */
