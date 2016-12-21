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
static unsigned int do_memlog_clean(void);
static unsigned int memlog_free_item(void);
static unsigned int memlog_free_chunk(void);

/**
 * Access to the memlog allocator is protected by this lock
 */
static pthread_mutex_t memlog_lock = PTHREAD_MUTEX_INITIALIZER;

static size_t mem_limit = 0;
static void *mem_base = NULL;
static void *mem_current = NULL;
static size_t mem_avail = 0;
static size_t mem_free_from_beginning = 0;
static void *mem_current_freeing = NULL;
static void *mem_current_to_free = NULL;


void memlog_init(void) {
    mem_limit = MEMLOG_DEFAULT_SIZE;

	/* Allocate everything in a big chunk with malloc */
	mem_base = malloc(mem_limit);
	memset(mem_base, 0, mem_limit);
	if (mem_base != NULL) {
		mem_current = mem_base;
		mem_avail = mem_limit;
		mem_free_from_beginning = 0;
		mem_current_freeing = mem_base;
		mem_current_to_free = mem_base;
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
    char null_char = '\0';

	// we are near the physical boundary of the log, lets start from the base
	if (((char *)mem_current - (char *)mem_base) + size > mem_limit - 35) { // 35 bytes is the minimal item size
		// allocate memory for cycle item
		item_data *it_data = (item_data *)mem_current;
		it_data->it_data_flags |= ITEM_DIRTY;
    	it_data->it_data_flags |= ITEM_CYCLE;
    	it_data->nkey = sizeof(null_char);
    	uint8_t nsuffix;
    	char suffix[40];
        char snum[10]; // more than enough for 1MB sized items
        int num =  sprintf(snum, "%d", (int)(mem_avail - sizeof(item_data) - (sizeof(null_char) + 1) - 4));
    	it_data->nbytes = mem_avail - num /* size of "%d" */ /
							- sizeof(item_data) /
							- (sizeof(null_char) + 1) /* nkey + 1 */ /
							- 4 /* the size of " 16 " where 16 = ITEM_CYCLE */ /
							- 2 /* size of "/r/n" */;
		memcpy(ITEM_key(it_data), &null_char, sizeof(null_char));
	    item_make_header(sizeof(null_char) + 1, ITEM_CYCLE,
	    		mem_avail - num /* size of "%d" */
				- sizeof(item_data)
				- (sizeof(null_char) + 1) /* nkey + 1 */
				- 4 /* the size of " 16 " where 16 = ITEM_CYCLE */
				- 2 /* size of "/r/n" */,
				suffix, &nsuffix);
		memcpy(ITEM_suffix(it_data), suffix, (size_t)nsuffix);
		it_data->nsuffix = nsuffix;

		mem_current = mem_base;
		mem_avail = mem_free_from_beginning;
		mem_free_from_beginning = 0;

	    STATS_LOCK();
	    stats.mem_current = (char *)mem_current - (char *)mem_base;
	    STATS_UNLOCK();

	    it_data->it_data_flags &= ~ITEM_DIRTY;
	    it_data->it_data_flags |= ITEM_STORED;
	}

	if (size > mem_avail) {
		return NULL;
	}

	ret = mem_current;

	mem_current = ((char *)mem_current) + size;

	mem_avail -= size;

    STATS_LOCK();
    stats.mem_current = (char *)mem_current - (char *)mem_base;
    STATS_UNLOCK();

    return ret;
}

static void *do_memlog_alloc(const size_t size, uint64_t *total_bytes,
        unsigned int flags) {
    void *ret = NULL;

	char *ptr;

	if ((ptr = memory_allocate((size_t)size)) == 0) {
		return NULL;
	}

	memset(ptr, 0, (size_t)size);
	*total_bytes = size;

	ret = (void *)ptr;
	return ret;
}

unsigned int memlog_clean() {
	// do_memlog_clean handles its own locks
	/*
	 * FIXME: There is a risk of a race here with do_memlog_alloc.
	 * Never occurred to me, but if stange things are happening, this is a place to check
	 */
	return do_memlog_clean();
}

static unsigned int do_memlog_clean() {
	unsigned int freed = 0;
	pthread_mutex_lock(&memlog_lock);
	freed = memlog_free_chunk();
	if (freed > 0) {
		if ((char *)mem_current <= (char *)mem_current_freeing) {
			mem_avail += freed;
		} else {
			mem_free_from_beginning += freed;
		}

		// we reached the buffers border, time to start over
		if (((char *)mem_current_freeing - (char *)mem_base) >= mem_limit) {
			mem_current_freeing = mem_base;
		}

	}
	pthread_mutex_unlock(&memlog_lock);

	return freed;
}

unsigned int get_memory_limit() {
	return mem_limit;
}

unsigned int get_memory_available() {
	return mem_avail;
}

void set_memory_available(unsigned int new_mem_avail) {
	mem_avail = new_mem_avail;
}

unsigned int get_memory_free_from_beginning() {
	return mem_free_from_beginning;
}

void set_memory_free_from_beginning(unsigned int new_mem_free_from_beginning) {
	mem_free_from_beginning = new_mem_free_from_beginning;
}

void *get_memory_base() {
	return mem_base;
}

void *get_memory_current() {
	return mem_current;
}

void set_memory_current(void *new_mem_current) {
	mem_current = new_mem_current;
}

static unsigned int memlog_free_item() {
	item_data *it = NULL;
	unsigned int ntotal = 0;
	uint8_t flags = 0;

	it = (item_data *)mem_current_freeing; // anything in memlog is some kind of item
	ntotal = ITEM_ntotal(it) ;
	flags = it->it_data_flags;

	if ((flags & ITEM_CORRUPTED) != 0 ||
			(flags & ITEM_DELETED) != 0) {
		memset(mem_current_freeing, 0, ntotal);
	    if (settings.verbose > 2)
	        fprintf(stderr, "LRU moved from %u to %u, meta item\n",
	        		(unsigned int)((char *)mem_current_freeing - (char *)mem_base),
					(unsigned int)((char *)mem_current_freeing - (char *)mem_base) + ntotal);
		mem_current_freeing = (char *)mem_current_freeing + ntotal;

		return ntotal;
	} else if ((flags & ITEM_CYCLE) != 0) {
		ntotal = it->nbytes;
		memset(mem_current_freeing, 0, ntotal);
		    if (settings.verbose > 2)
		        fprintf(stderr, "LRU moved from %u to %u, meta item\n",
		        		(unsigned int)((char *)mem_current_freeing - (char *)mem_base),
						(unsigned int)((char *)mem_current_freeing - (char *)mem_base) + ntotal);
			mem_current_freeing = (char *)mem_current_freeing + ntotal;

			return ntotal;

	}

	uint32_t hv = hash(ITEM_key(it), it->nkey);
    item_lock(hv);
    item_metadata *it_meta = assoc_find(ITEM_key(it), it->nkey, hv);
    if (it_meta != NULL && it_meta->item == it) {
	    if (settings.verbose > 2)
	        fprintf(stderr, "LRU deleting active item\n");
    	do_item_unlink_nolock(it_meta, hv);
    }
    item_unlock(hv);

    memset(mem_current_freeing, 0, ntotal);
    if (settings.verbose > 2)
        fprintf(stderr, "LRU moved from %u to %u\n",
        		(unsigned int)((char *)mem_current_freeing - (char *)mem_base),
				(unsigned int)((char *)mem_current_freeing - (char *)mem_base) + ntotal);
    mem_current_freeing = (char *)mem_current_freeing + ntotal;

    return ntotal;
}

static unsigned int memlog_free_chunk() {
	unsigned int freed = 0;
	unsigned int to_free = mem_limit / 8;
	while (freed < to_free) {
		if ((unsigned int)((char *)mem_current_freeing - (char *)mem_base) >= mem_limit) { // we reached the end
			return freed;
		}
		freed += memlog_free_item();
	}

	return freed;
}
