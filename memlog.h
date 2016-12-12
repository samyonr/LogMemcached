/*
 * memlog.h
 *
 */

#ifndef MEMLOG_H_
#define MEMLOG_H_

void memlog_init(void);
void *memlog_alloc(size_t size, uint64_t *total_bytes, unsigned int flags);
unsigned int memlog_clean(void);
unsigned int get_memory_limit(void);
unsigned int get_memory_available(void);
unsigned int get_memory_free_from_beginning(void);

#endif /* MEMLOG_H_ */
