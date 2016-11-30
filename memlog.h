/*
 * memlog.h
 *
 */

#ifndef MEMLOG_H_
#define MEMLOG_H_

void memlog_init(void);
void *memlog_alloc(size_t size, uint64_t *total_bytes, unsigned int flags);

#endif /* MEMLOG_H_ */
