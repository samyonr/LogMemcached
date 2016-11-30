/*
 * memlog.c
 *
 */
#include "memcached.h"
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <assert.h>
#include <pthread.h>

static void *memory_allocate(size_t size);
static void *do_memlog_alloc(const size_t size, uint64_t *total_bytes, unsigned int flags);

/**
 * Access to the memlog allocator is protected by this lock
 */
static pthread_mutex_t memlog_lock = PTHREAD_MUTEX_INITIALIZER;

static size_t mem_limit = 0;
static void *mem_base = NULL;
static void *mem_current = NULL;
static size_t mem_avail = 0;

void memlog_init(void) {
    mem_limit = MEMLOG_DEFAULT_SIZE;

	/* Allocate everything in a big chunk with malloc */
	mem_base = malloc(mem_limit);
	if (mem_base != NULL) {
		mem_current = mem_base;
		mem_avail = mem_limit;
	} else {
		fprintf(stderr, "Warning: Failed to allocate requested memory in"
				" one large chunk.\nWill allocate in smaller chunks\n");
		exit(1); // TODO: can we allocate it in small chunks?
	}
}

void *memlog_alloc(size_t size, uint64_t *total_bytes,
        unsigned int flags) {
    void *ret;

    pthread_mutex_lock(&memlog_lock);
    ret = do_memlog_alloc(size, total_bytes, flags);
    pthread_mutex_unlock(&memlog_lock);
    return ret;
}

static void *memory_allocate(size_t size) {
    void *ret;

	ret = mem_current;

	if (size > mem_avail) {
		return NULL;
	}

	mem_current = ((char*)mem_current) + size;

	mem_avail -= size;

    return ret;
}

static void *do_memlog_alloc(const size_t size, uint64_t *total_bytes,
        unsigned int flags) {
    void *ret = NULL;

    if (size > mem_avail) {
    	return NULL;
    }
	char *ptr;

	if ((ptr = memory_allocate((size_t)size)) == 0) {
		return NULL;
	}

	memset(ptr, 0, (size_t)size);
	*total_bytes = size;

	ret = (void *)ptr;
	return ret;
}
