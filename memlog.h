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
void set_memory_free_from_beginning(unsigned int new_mem_free_from_beginning);
void *get_memory_base(void);
void *get_memory_current(void);
void set_memory_current(void *new_mem_current);
void set_memory_available(unsigned int new_mem_avail);

#endif /* MEMLOG_H_ */
