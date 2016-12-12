/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
#include "memcached.h"
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <assert.h>
#include <unistd.h>
#include <poll.h>


static size_t mem_freelist_limit = 0;
static void *mem_freelist_base = NULL;
static item_metadata *first_free_freelist_item = NULL;

/**
 * Access to the slab allocator is protected by this lock
 */
static pthread_mutex_t freelist_lock = PTHREAD_MUTEX_INITIALIZER;


/* Forward Declarations */
static void item_link_q(item_metadata *it);
static void item_unlink_q(item_metadata *it);
static void *do_freelist_alloc(void);
static void do_freelist_free(void *ptr, const size_t size, unsigned int id);
unsigned int freelist_clsid(const size_t size);

static unsigned int lru_type_map[4] = {HOT_LRU, WARM_LRU, COLD_LRU, NOEXP_LRU};

#define LARGEST_ID POWER_LARGEST
typedef struct {
    uint64_t evicted;
    uint64_t evicted_nonzero;
    uint64_t reclaimed;
    uint64_t outofmemory;
    uint64_t tailrepairs;
    uint64_t expired_unfetched; /* items reclaimed but never touched */
    uint64_t evicted_unfetched; /* items evicted but never touched */
    uint64_t crawler_reclaimed;
    uint64_t crawler_items_checked;
    uint64_t lrutail_reflocked;
    uint64_t moves_to_cold;
    uint64_t moves_to_warm;
    uint64_t moves_within_lru;
    uint64_t direct_reclaims;
    rel_time_t evicted_time;
} itemstats_t;

static itemstats_t itemstats[LARGEST_ID];
static unsigned int sizes[LARGEST_ID];
static uint64_t sizes_bytes[LARGEST_ID];
static unsigned int *stats_sizes_hist = NULL;
static uint64_t stats_sizes_cas_min = 0;
static int stats_sizes_buckets = 0;

static volatile int do_run_lru_maintainer_thread = 0;
static int lru_maintainer_initialized = 0;
static pthread_mutex_t lru_maintainer_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t cas_id_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t stats_sizes_lock = PTHREAD_MUTEX_INITIALIZER;

/**
 * Determines the chunk sizes and initializes the slab class descriptors
 * accordingly.
 */
void freelist_init(const size_t limit) {
    mem_freelist_limit = (limit / sizeof(item_metadata)) * sizeof(item_metadata);

	/* Allocate everything in a big chunk with malloc */
	mem_freelist_base = malloc(mem_freelist_limit);
	if (mem_freelist_base != NULL) {
	} else {
		fprintf(stderr, "Warning: Failed to allocate requested memory in"
				" one large chunk.\nWill allocate in smaller chunks\n");
		exit(1);
	}

	char *ptr = mem_freelist_base;
    memset(mem_freelist_base, 0, (size_t)mem_freelist_limit);
    int x;
    for (x = 0; x < limit / sizeof(item_metadata); x++) {
        item_metadata *it;
        it = (item_metadata *)ptr;
		it->it_flags = ITEM_SLABBED;
		it->slabs_clsid = 0;
		it->get_count = 0;
		if (x == 0) { // first item
			it->prev_free = 0;
			it->next_free = 0;
		} else {
			it->prev_free = 0;
			it->next_free = (item_metadata *)(ptr - sizeof(item_metadata));
			if (it->next_free) it->next_free->prev_free = it;
		}
		first_free_freelist_item = it;
        ptr += sizeof(item_metadata);
    }
}

/*@null@*/
static void *do_freelist_alloc(void) {
    void *ret = NULL;
    item_metadata *it = NULL;

    if (first_free_freelist_item == NULL) {
        return NULL;
    }


	/* return off our freelist */
	it = (item_metadata *)first_free_freelist_item;
	first_free_freelist_item = it->next_free;
	if (it->next_free) it->next_free->prev_free = 0;
	/* Kill flag and initialize refcount here for lock safety in slab
	 * mover's freeness detection. */
	it->it_flags &= ~ITEM_SLABBED;
	it->refcount = 1;
	it->slabs_clsid = 1;
	it->get_count = 0;
	ret = (void *)it;

    return ret;
}

void *freelist_alloc(void) {
    void *ret;

    pthread_mutex_lock(&freelist_lock);
    ret = do_freelist_alloc();
    pthread_mutex_unlock(&freelist_lock);
    return ret;
}

void freelist_free(void *ptr, size_t size, unsigned int id) {
    pthread_mutex_lock(&freelist_lock);
    do_freelist_free(ptr, size, id);
    pthread_mutex_unlock(&freelist_lock);
}

static void do_freelist_free(void *ptr, const size_t size, unsigned int id) {
    item_metadata *it;

    MEMCACHED_SLABS_FREE(size, id, ptr);

    it = (item_metadata *)ptr;
	it->it_flags = ITEM_SLABBED;
	it->slabs_clsid = 0;
	it->prev_free = 0;
	it->next_free = first_free_freelist_item;
	if (it->next_free) it->next_free->prev_free = it;
	first_free_freelist_item = it;

    return;
}

void item_stats_reset(void) {
    int i;
    for (i = 0; i < LARGEST_ID; i++) {
        pthread_mutex_lock(&lru_locks[i]);
        memset(&itemstats[i], 0, sizeof(itemstats_t));
        pthread_mutex_unlock(&lru_locks[i]);
    }
}

/* called with class lru lock held */
void do_item_stats_add_crawl(const int i, const uint64_t reclaimed,
        const uint64_t unfetched, const uint64_t checked) {
    itemstats[i].crawler_reclaimed += reclaimed;
    itemstats[i].expired_unfetched += unfetched;
    itemstats[i].crawler_items_checked += checked;
}

#define LRU_PULL_EVICT 1
#define LRU_PULL_CRAWL_BLOCKS 2

/* Get the next CAS id for a new item. */
/* TODO: refactor some atomics for this. */
uint64_t get_cas_id(void) {
    static uint64_t cas_id = 0;
    pthread_mutex_lock(&cas_id_lock);
    uint64_t next_id = ++cas_id;
    pthread_mutex_unlock(&cas_id_lock);
    return next_id;
}

int item_is_flushed(item_metadata *it) {
    rel_time_t oldest_live = settings.oldest_live;
    uint64_t cas = ITEM_get_cas(it->item);
    uint64_t oldest_cas = settings.oldest_cas;
    if (oldest_live == 0 || oldest_live > current_time)
        return 0;
    if ((it->time <= oldest_live)
            || (oldest_cas != 0 && cas != 0 && cas < oldest_cas)) {
        return 1;
    }
    return 0;
}


/* Enable this for reference-count debugging. */
#if 0
# define DEBUG_REFCNT(it,op) \
                fprintf(stderr, "item %x refcnt(%c) %d %c%c%c\n", \
                        it, op, it->refcount, \
                        (it->it_flags & ITEM_LINKED) ? 'L' : ' ', \
                        (it->it_flags & ITEM_SLABBED) ? 'S' : ' ')
#else
# define DEBUG_REFCNT(it,op) while(0)
#endif

/**
 * Generates the variable-sized part of the header for an object.
 *
 * key     - The key
 * nkey    - The length of the key
 * flags   - key flags
 * nbytes  - Number of bytes to hold value and addition CRLF terminator
 * suffix  - Buffer for the "VALUE" line suffix (flags, size).
 * nsuffix - The length of the suffix is stored here.
 *
 * Returns the total size of the header.
 */
size_t item_make_header(const uint8_t nkey, const unsigned int flags, const int nbytes,
                     char *suffix, uint8_t *nsuffix) {
    /* suffix is defined at 40 chars elsewhere.. */
    *nsuffix = (uint8_t) snprintf(suffix, 40, " %u %d\r\n", flags, nbytes - 2);
    return sizeof(item_data) + nkey + *nsuffix + nbytes;
}

item_metadata *do_item_alloc(char *key, const size_t nkey, const unsigned int flags,
                    const rel_time_t exptime, const int nbytes, bool *succeed) {
    int i;
    uint8_t nsuffix;
    item_metadata *it = NULL;
    item_data *it_data = NULL;
    char suffix[40];
    size_t ntotal = item_make_header(nkey + 1, flags, nbytes, suffix, &nsuffix);
    if (settings.use_cas) {
        ntotal += sizeof(uint64_t);
    }

    unsigned int id = 1;

    for (i = 0; i < 3; i++) {
        uint64_t total_bytes;
        it_data = memlog_alloc(ntotal, &total_bytes, 0);

        if (it_data == NULL) {
        	usleep(1);
        } else if ((flags & ITEM_DELETED) != 0) {
        	it_data->it_data_flags |= ITEM_DELETED;
        	it_data->nkey = nkey;
        	it_data->nbytes = nbytes;
			memcpy(ITEM_key(it_data), key, nkey);
			memcpy(ITEM_suffix(it_data), suffix, (size_t)nsuffix);
			it_data->nsuffix = nsuffix;
			if (succeed != NULL) {
				*succeed = true;
			}
			return NULL;
        } else { // it_data allocated and it's not a delete item
			it = freelist_alloc();
			// if there was a place for it_data, there should be also a place for item's metadata
			assert(it != NULL);
			it->item = it_data;
			break;
        }
    }

    if (i > 0) {
        pthread_mutex_lock(&lru_locks[id]);
        itemstats[id].direct_reclaims += i;
        pthread_mutex_unlock(&lru_locks[id]);
    }

    if (it_data == NULL) {
        pthread_mutex_lock(&lru_locks[id]);
        itemstats[id].outofmemory++;
        pthread_mutex_unlock(&lru_locks[id]);
        if (succeed != NULL) {
        	*succeed = false;
        }
        return NULL;
    }


    /* Refcount is seeded to 1 by slabs_alloc() */
    it->next_free = it->prev_free = 0;

    it->slabs_clsid = id;

    DEBUG_REFCNT(it, '*');
    it->it_flags |= settings.use_cas ? ITEM_CAS : 0;
    it->item->it_data_flags |= settings.use_cas ? ITEM_CAS : 0;
	it->item->it_data_flags |= ITEM_DIRTY;
    it->item->nkey = nkey;
    it->item->nbytes = nbytes;
    memcpy(ITEM_key(it->item), key, nkey);
    it->item->exptime = exptime;
    memcpy(ITEM_suffix(it->item), suffix, (size_t)nsuffix);
    it->item->nsuffix = nsuffix;
    it->h_next = 0;
    if (succeed != NULL) {
    	*succeed = true;
    }
    return it;
}

void item_free(item_metadata *it) {
    size_t ntotal = ITEM_ntotal(it->item);
    unsigned int clsid;
    assert((it->it_flags & ITEM_LINKED) == 0);
    assert(it->refcount == 0);

    /* so slab size changer can tell later if item is already free or not */
    clsid = ITEM_clsid(it);
    DEBUG_REFCNT(it, 'F');
    freelist_free(it, ntotal, clsid);
}

unsigned int freelist_clsid(const size_t size) {
    if (size == 0) {
    	return 0;
    } else {
    	return 1;
    }
}

/**
 * Returns true if an item will fit in the cache (its size does not exceed
 * the maximum for a cache entry.)
 */
bool item_size_ok(const size_t nkey, const int flags, const int nbytes) {
    char prefix[40];
    uint8_t nsuffix;

    size_t ntotal = item_make_header(nkey + 1, flags, nbytes,
                                     prefix, &nsuffix);
    if (settings.use_cas) {
        ntotal += sizeof(uint64_t);
    }

    return freelist_clsid(ntotal) != 0;
}

static void do_item_link_q(item_metadata *it) { /* item is the new head */
    assert((it->it_flags & ITEM_SLABBED) == 0);
    sizes[it->slabs_clsid]++;
    sizes_bytes[it->slabs_clsid] += it->item->nbytes;
    return;
}

static void item_link_q(item_metadata *it) {
    pthread_mutex_lock(&lru_locks[it->slabs_clsid]);
    do_item_link_q(it);
    pthread_mutex_unlock(&lru_locks[it->slabs_clsid]);
}

static void do_item_unlink_q(item_metadata *it) {
    sizes[it->slabs_clsid]--;
    sizes_bytes[it->slabs_clsid] -= it->item->nbytes;
    return;
}

static void item_unlink_q(item_metadata *it) {
    pthread_mutex_lock(&lru_locks[it->slabs_clsid]);
    do_item_unlink_q(it);
    pthread_mutex_unlock(&lru_locks[it->slabs_clsid]);
}

int do_item_link(item_metadata *it, const uint32_t hv) {
    MEMCACHED_ITEM_LINK(ITEM_key(it->item), it->item->nkey, it->item->nbytes);
    assert((it->it_flags & (ITEM_LINKED|ITEM_SLABBED)) == 0);
    it->it_flags |= ITEM_LINKED;
    it->time = current_time;

    STATS_LOCK();
    stats_state.curr_bytes += ITEM_ntotal(it->item);
    stats_state.curr_items += 1;
    stats.total_items += 1;
    STATS_UNLOCK();

    /* Allocate a new CAS ID on link. */
    ITEM_set_cas(it->item, (settings.use_cas) ? get_cas_id() : 0);
    assoc_insert(it, hv);
    item_link_q(it);
    refcount_incr(&it->refcount);
    item_stats_sizes_add(it);

    return 1;
}

void do_item_unlink(item_metadata *it, const uint32_t hv) {
    MEMCACHED_ITEM_UNLINK(ITEM_key(it), it->nkey, it->nbytes);
    if ((it->it_flags & ITEM_LINKED) != 0) {
        it->it_flags &= ~ITEM_LINKED;
        STATS_LOCK();
        stats_state.curr_bytes -= ITEM_ntotal(it->item);
        stats_state.curr_items -= 1;
        STATS_UNLOCK();
        item_stats_sizes_remove(it);
        assoc_delete(ITEM_key(it->item), it->item->nkey, hv);
        item_unlink_q(it);
        do_item_remove(it);
    }
}

/* FIXME: Is it necessary to keep this copy/pasted code? */
void do_item_unlink_nolock(item_metadata *it, const uint32_t hv) {
    MEMCACHED_ITEM_UNLINK(ITEM_key(it), it->nkey, it->nbytes);
    if ((it->it_flags & ITEM_LINKED) != 0) {
        it->it_flags &= ~ITEM_LINKED;
        STATS_LOCK();
        stats_state.curr_bytes -= ITEM_ntotal(it->item);
        stats_state.curr_items -= 1;
        STATS_UNLOCK();
        item_stats_sizes_remove(it);
        assoc_delete(ITEM_key(it->item), it->item->nkey, hv);
        do_item_unlink_q(it);
        do_item_remove(it);
    }
}

void do_item_remove(item_metadata *it) {
    MEMCACHED_ITEM_REMOVE(ITEM_key(it), it->nkey, it->nbytes);
    assert((it->it_flags & ITEM_SLABBED) == 0);
    assert(it->refcount > 0);

    if (refcount_decr(&it->refcount) == 0) {
        item_free(it);
    }
}

/* Copy/paste to avoid adding two extra branches for all common calls, since
 * _nolock is only used in an uncommon case where we want to relink. */
void do_item_update_nolock(item_metadata *it) {
    MEMCACHED_ITEM_UPDATE(ITEM_key(it), it->nkey, it->nbytes);
    if (it->time < current_time - ITEM_UPDATE_INTERVAL) {
        assert((it->it_flags & ITEM_SLABBED) == 0);

        if ((it->it_flags & ITEM_LINKED) != 0) {
            do_item_unlink_q(it);
            it->time = current_time;
            do_item_link_q(it);
        }
    }
}

/* Bump the last accessed time, or relink if we're in compat mode */
void do_item_update(item_metadata *it) {
    MEMCACHED_ITEM_UPDATE(ITEM_key(it), it->nkey, it->nbytes);
    if (it->time < current_time - ITEM_UPDATE_INTERVAL) {
        assert((it->it_flags & ITEM_SLABBED) == 0);

        if ((it->it_flags & ITEM_LINKED) != 0) {
            it->time = current_time;
            if (settings.lru_log_enabled) {
            	if (it->get_count >= settings.lru_log_get_count) {
					if ((it = item_get_update(it)) != NULL) {
						it->get_count = 0;
					}
            	} else {
            		it->get_count++;
            	}
            }
            if (!settings.lru_maintainer_thread) {
                item_unlink_q(it);
                item_link_q(it);
            }
        }
    }
}

int do_item_replace(item_metadata *it, item_metadata *new_it, const uint32_t hv) {
    MEMCACHED_ITEM_REPLACE(ITEM_key(it), it->nkey, it->nbytes,
                           ITEM_key(new_it), new_it->nkey, new_it->nbytes);
    assert((it->it_flags & ITEM_SLABBED) == 0);

    do_item_unlink(it, hv);
    return do_item_link(new_it, hv);
}

/*@null@*/
/* This is walking the line of violating lock order, but I think it's safe.
 * If the LRU lock is held, an item in the LRU cannot be wiped and freed.
 * The data could possibly be overwritten, but this is only accessing the
 * headers.
 * It may not be the best idea to leave it like this, but for now it's safe.
 * FIXME: only dumps the hot LRU with the new LRU's.
 */
char *item_cachedump(const unsigned int slabs_clsid, const unsigned int limit, unsigned int *bytes) {
    return NULL;
}

void item_stats_totals(ADD_STAT add_stats, void *c) {
    itemstats_t totals;
    memset(&totals, 0, sizeof(itemstats_t));
    int n;
    for (n = 0; n < MAX_NUMBER_OF_SLAB_CLASSES; n++) {
        int x;
        int i;
        for (x = 0; x < 4; x++) {
            i = n | lru_type_map[x];
            pthread_mutex_lock(&lru_locks[i]);
            totals.expired_unfetched += itemstats[i].expired_unfetched;
            totals.evicted_unfetched += itemstats[i].evicted_unfetched;
            totals.evicted += itemstats[i].evicted;
            totals.reclaimed += itemstats[i].reclaimed;
            totals.crawler_reclaimed += itemstats[i].crawler_reclaimed;
            totals.crawler_items_checked += itemstats[i].crawler_items_checked;
            totals.lrutail_reflocked += itemstats[i].lrutail_reflocked;
            totals.moves_to_cold += itemstats[i].moves_to_cold;
            totals.moves_to_warm += itemstats[i].moves_to_warm;
            totals.moves_within_lru += itemstats[i].moves_within_lru;
            totals.direct_reclaims += itemstats[i].direct_reclaims;
            pthread_mutex_unlock(&lru_locks[i]);
        }
    }
    APPEND_STAT("expired_unfetched", "%llu",
                (unsigned long long)totals.expired_unfetched);
    APPEND_STAT("evicted_unfetched", "%llu",
                (unsigned long long)totals.evicted_unfetched);
    APPEND_STAT("evictions", "%llu",
                (unsigned long long)totals.evicted);
    APPEND_STAT("reclaimed", "%llu",
                (unsigned long long)totals.reclaimed);
    APPEND_STAT("crawler_reclaimed", "%llu",
                (unsigned long long)totals.crawler_reclaimed);
    APPEND_STAT("crawler_items_checked", "%llu",
                (unsigned long long)totals.crawler_items_checked);
    APPEND_STAT("lrutail_reflocked", "%llu",
                (unsigned long long)totals.lrutail_reflocked);
    if (settings.lru_maintainer_thread) {
        APPEND_STAT("moves_to_cold", "%llu",
                    (unsigned long long)totals.moves_to_cold);
        APPEND_STAT("moves_to_warm", "%llu",
                    (unsigned long long)totals.moves_to_warm);
        APPEND_STAT("moves_within_lru", "%llu",
                    (unsigned long long)totals.moves_within_lru);
        APPEND_STAT("direct_reclaims", "%llu",
                    (unsigned long long)totals.direct_reclaims);
    }
}

void item_stats(ADD_STAT add_stats, void *c) {
    itemstats_t totals;
    int n;
    for (n = 0; n < MAX_NUMBER_OF_SLAB_CLASSES; n++) {
        memset(&totals, 0, sizeof(itemstats_t));
        int x;
        int i;
        unsigned int size = 0;
        unsigned int age  = 0;
        unsigned int lru_size_map[4];
        const char *fmt = "items:%d:%s";
        char key_str[STAT_KEY_LEN];
        char val_str[STAT_VAL_LEN];
        int klen = 0, vlen = 0;
        for (x = 0; x < 4; x++) {
            i = n | lru_type_map[x];
            pthread_mutex_lock(&lru_locks[i]);
            totals.evicted += itemstats[i].evicted;
            totals.evicted_nonzero += itemstats[i].evicted_nonzero;
            totals.outofmemory += itemstats[i].outofmemory;
            totals.tailrepairs += itemstats[i].tailrepairs;
            totals.reclaimed += itemstats[i].reclaimed;
            totals.expired_unfetched += itemstats[i].expired_unfetched;
            totals.evicted_unfetched += itemstats[i].evicted_unfetched;
            totals.crawler_reclaimed += itemstats[i].crawler_reclaimed;
            totals.crawler_items_checked += itemstats[i].crawler_items_checked;
            totals.lrutail_reflocked += itemstats[i].lrutail_reflocked;
            totals.moves_to_cold += itemstats[i].moves_to_cold;
            totals.moves_to_warm += itemstats[i].moves_to_warm;
            totals.moves_within_lru += itemstats[i].moves_within_lru;
            totals.direct_reclaims += itemstats[i].direct_reclaims;
            size += sizes[i];
            lru_size_map[x] = sizes[i];
            pthread_mutex_unlock(&lru_locks[i]);
        }
        if (size == 0)
            continue;
        APPEND_NUM_FMT_STAT(fmt, n, "number", "%u", size);
        if (settings.lru_maintainer_thread) {
            APPEND_NUM_FMT_STAT(fmt, n, "number_hot", "%u", lru_size_map[0]);
            APPEND_NUM_FMT_STAT(fmt, n, "number_warm", "%u", lru_size_map[1]);
            APPEND_NUM_FMT_STAT(fmt, n, "number_cold", "%u", lru_size_map[2]);
            if (settings.expirezero_does_not_evict) {
                APPEND_NUM_FMT_STAT(fmt, n, "number_noexp", "%u", lru_size_map[3]);
            }
        }
        APPEND_NUM_FMT_STAT(fmt, n, "age", "%u", age);
        APPEND_NUM_FMT_STAT(fmt, n, "evicted",
                            "%llu", (unsigned long long)totals.evicted);
        APPEND_NUM_FMT_STAT(fmt, n, "evicted_nonzero",
                            "%llu", (unsigned long long)totals.evicted_nonzero);
        APPEND_NUM_FMT_STAT(fmt, n, "evicted_time",
                            "%u", totals.evicted_time);
        APPEND_NUM_FMT_STAT(fmt, n, "outofmemory",
                            "%llu", (unsigned long long)totals.outofmemory);
        APPEND_NUM_FMT_STAT(fmt, n, "tailrepairs",
                            "%llu", (unsigned long long)totals.tailrepairs);
        APPEND_NUM_FMT_STAT(fmt, n, "reclaimed",
                            "%llu", (unsigned long long)totals.reclaimed);
        APPEND_NUM_FMT_STAT(fmt, n, "expired_unfetched",
                            "%llu", (unsigned long long)totals.expired_unfetched);
        APPEND_NUM_FMT_STAT(fmt, n, "evicted_unfetched",
                            "%llu", (unsigned long long)totals.evicted_unfetched);
        APPEND_NUM_FMT_STAT(fmt, n, "crawler_reclaimed",
                            "%llu", (unsigned long long)totals.crawler_reclaimed);
        APPEND_NUM_FMT_STAT(fmt, n, "crawler_items_checked",
                            "%llu", (unsigned long long)totals.crawler_items_checked);
        APPEND_NUM_FMT_STAT(fmt, n, "lrutail_reflocked",
                            "%llu", (unsigned long long)totals.lrutail_reflocked);
        if (settings.lru_maintainer_thread) {
            APPEND_NUM_FMT_STAT(fmt, n, "moves_to_cold",
                                "%llu", (unsigned long long)totals.moves_to_cold);
            APPEND_NUM_FMT_STAT(fmt, n, "moves_to_warm",
                                "%llu", (unsigned long long)totals.moves_to_warm);
            APPEND_NUM_FMT_STAT(fmt, n, "moves_within_lru",
                                "%llu", (unsigned long long)totals.moves_within_lru);
            APPEND_NUM_FMT_STAT(fmt, n, "direct_reclaims",
                                "%llu", (unsigned long long)totals.direct_reclaims);
        }
    }

    /* getting here means both ascii and binary terminators fit */
    add_stats(NULL, 0, NULL, 0, c);
}

bool item_stats_sizes_status(void) {
    bool ret = false;
    mutex_lock(&stats_sizes_lock);
    if (stats_sizes_hist != NULL)
        ret = true;
    mutex_unlock(&stats_sizes_lock);
    return ret;
}

void item_stats_sizes_init(void) {
    if (stats_sizes_hist != NULL)
        return;
    stats_sizes_buckets = settings.item_size_max / 32 + 1;
    stats_sizes_hist = calloc(stats_sizes_buckets, sizeof(int));
    stats_sizes_cas_min = (settings.use_cas) ? get_cas_id() : 0;
}

void item_stats_sizes_enable(ADD_STAT add_stats, void *c) {
    mutex_lock(&stats_sizes_lock);
    if (!settings.use_cas) {
        APPEND_STAT("sizes_status", "error", "");
        APPEND_STAT("sizes_error", "cas_support_disabled", "");
    } else if (stats_sizes_hist == NULL) {
        item_stats_sizes_init();
        if (stats_sizes_hist != NULL) {
            APPEND_STAT("sizes_status", "enabled", "");
        } else {
            APPEND_STAT("sizes_status", "error", "");
            APPEND_STAT("sizes_error", "no_memory", "");
        }
    } else {
        APPEND_STAT("sizes_status", "enabled", "");
    }
    mutex_unlock(&stats_sizes_lock);
}

void item_stats_sizes_disable(ADD_STAT add_stats, void *c) {
    mutex_lock(&stats_sizes_lock);
    if (stats_sizes_hist != NULL) {
        free(stats_sizes_hist);
        stats_sizes_hist = NULL;
    }
    APPEND_STAT("sizes_status", "disabled", "");
    mutex_unlock(&stats_sizes_lock);
}

void item_stats_sizes_add(item_metadata *it) {
    if (stats_sizes_hist == NULL || stats_sizes_cas_min > ITEM_get_cas(it->item))
        return;
    int ntotal = ITEM_ntotal(it->item);
    int bucket = ntotal / 32;
    if ((ntotal % 32) != 0) bucket++;
    if (bucket < stats_sizes_buckets) stats_sizes_hist[bucket]++;
}

/* I think there's no way for this to be accurate without using the CAS value.
 * Since items getting their time value bumped will pass this validation.
 */
void item_stats_sizes_remove(item_metadata *it) {
    if (stats_sizes_hist == NULL || stats_sizes_cas_min > ITEM_get_cas(it->item))
        return;
    int ntotal = ITEM_ntotal(it->item);
    int bucket = ntotal / 32;
    if ((ntotal % 32) != 0) bucket++;
    if (bucket < stats_sizes_buckets) stats_sizes_hist[bucket]--;
}

/** dumps out a list of objects of each size, with granularity of 32 bytes */
/*@null@*/
/* Locks are correct based on a technicality. Holds LRU lock while doing the
 * work, so items can't go invalid, and it's only looking at header sizes
 * which don't change.
 */
void item_stats_sizes(ADD_STAT add_stats, void *c) {
    mutex_lock(&stats_sizes_lock);

    if (stats_sizes_hist != NULL) {
        int i;
        for (i = 0; i < stats_sizes_buckets; i++) {
            if (stats_sizes_hist[i] != 0) {
                char key[8];
                snprintf(key, sizeof(key), "%d", i * 32);
                APPEND_STAT(key, "%u", stats_sizes_hist[i]);
            }
        }
    } else {
        APPEND_STAT("sizes_status", "disabled", "");
    }

    add_stats(NULL, 0, NULL, 0, c);
    mutex_unlock(&stats_sizes_lock);
}

/** wrapper around assoc_find which does the lazy expiration logic */
item_metadata *do_item_get(const char *key, const size_t nkey, const uint32_t hv, conn *c) {
	item_metadata *it = assoc_find(key, nkey, hv);
    if (it != NULL) {
        refcount_incr(&it->refcount);
        /* Optimization for slab reassignment. prevents popular items from
         * jamming in busy wait. Can only do this here to satisfy lock order
         * of item_lock, slabs_lock. */
        /* This was made unsafe by removal of the cache_lock:
         * slab_rebalance_signal and slab_rebal.* are modified in a separate
         * thread under slabs_lock. If slab_rebalance_signal = 1, slab_start =
         * NULL (0), but slab_end is still equal to some value, this would end
         * up unlinking every item fetched.
         * This is either an acceptable loss, or if slab_rebalance_signal is
         * true, slab_start/slab_end should be put behind the slabs_lock.
         * Which would cause a huge potential slowdown.
         * Could also use a specific lock for slab_rebal.* and
         * slab_rebalance_signal (shorter lock?)
         */
        /*if (slab_rebalance_signal &&
            ((void *)it >= slab_rebal.slab_start && (void *)it < slab_rebal.slab_end)) {
            do_item_unlink(it, hv);
            do_item_remove(it);
            it = NULL;
        }*/
    }
    int was_found = 0;

    if (settings.verbose > 2) {
        int ii;
        if (it == NULL) {
            fprintf(stderr, "> NOT FOUND ");
        } else {
            fprintf(stderr, "> FOUND KEY ");
        }
        for (ii = 0; ii < nkey; ++ii) {
            fprintf(stderr, "%c", key[ii]);
        }
    }

    if (it != NULL) {
        was_found = 1;
        if (item_is_flushed(it)) {
            do_item_unlink(it, hv);
            do_item_remove(it);
            it = NULL;
            pthread_mutex_lock(&c->thread->stats.mutex);
            c->thread->stats.get_flushed++;
            pthread_mutex_unlock(&c->thread->stats.mutex);
            if (settings.verbose > 2) {
                fprintf(stderr, " -nuked by flush");
            }
            was_found = 2;
        } else if (it->item->exptime != 0 && it->item->exptime <= current_time) {
            do_item_unlink(it, hv);
            do_item_remove(it);
            it = NULL;
            pthread_mutex_lock(&c->thread->stats.mutex);
            c->thread->stats.get_expired++;
            pthread_mutex_unlock(&c->thread->stats.mutex);
            if (settings.verbose > 2) {
                fprintf(stderr, " -nuked by expire");
            }
            was_found = 3;
        } else {
            it->it_flags |= ITEM_FETCHED;
            DEBUG_REFCNT(it, '+');
        }
    }

    if (settings.verbose > 2)
        fprintf(stderr, "\n");
    /* For now this is in addition to the above verbose logging. */
    LOGGER_LOG(c->thread->l, LOG_FETCHERS, LOGGER_ITEM_GET, NULL, was_found, key, nkey);

    return it;
}

item_metadata *do_item_touch(const char *key, size_t nkey, uint32_t exptime,
                    const uint32_t hv, conn *c) {
	item_metadata *it = NULL;
	item_metadata *new_it = NULL;
	item_metadata *old_it = do_item_get(key, nkey, hv, c);
    if (old_it != NULL) {
    	enum store_item_type stored = NOT_STORED;
    	int failed_alloc = 0;
		/* we have old_it here - alloc memory to hold new_it */
		/* flags was already lost - so recover them from ITEM_suffix(it) */

    	uint32_t flags = (uint32_t) strtoul(ITEM_suffix(old_it->item), (char **) NULL, 10);

    	new_it = do_item_alloc(ITEM_key(old_it->item), nkey, flags, exptime, old_it->item->nbytes, NULL);

		if (new_it == NULL) {
			failed_alloc = 1;
			stored = NOT_STORED;
		} else {
			/* copy data from old_it to new_it */
			memcpy(ITEM_data(new_it->item), ITEM_data(old_it->item), old_it->item->nbytes);

			it = new_it;
		}

        if (stored == NOT_STORED && failed_alloc == 0) {
            if (old_it != NULL)
                item_replace(old_it, it, hv);

            c->cas = ITEM_get_cas(it->item);

            stored = STORED;
        }
    }

    if (old_it != NULL)
        do_item_remove(old_it);         /* release our reference */
    if (new_it != NULL)
        do_item_remove(new_it);

    return it;
}

item_metadata *do_item_get_update(item_metadata *old_it, const uint32_t hv) {
	item_metadata *it = NULL;
	item_metadata *new_it = NULL;
    if (old_it != NULL) {
    	enum store_item_type stored = NOT_STORED;
    	int failed_alloc = 0;
		/* we have old_it here - alloc memory to hold new_it */
		/* flags was already lost - so recover them from ITEM_suffix(it) */

    	uint32_t flags = (uint32_t) strtoul(ITEM_suffix(old_it->item), (char **) NULL, 10);

    	new_it = do_item_alloc(ITEM_key(old_it->item), old_it->item->nkey, flags, old_it->item->exptime, old_it->item->nbytes, NULL);

		if (new_it == NULL) {
			failed_alloc = 1;
			stored = NOT_STORED;
		} else {
			/* copy data from old_it to new_it */
			memcpy(ITEM_data(new_it->item), ITEM_data(old_it->item), old_it->item->nbytes);
			it = new_it;
		}

        if (stored == NOT_STORED && failed_alloc == 0) {
            if (old_it != NULL)
                item_replace(old_it, it, hv);

            //c->cas = ITEM_get_cas(it->item);

            stored = STORED;
        }
    }

    if (old_it != NULL)
        do_item_remove(old_it);         /* release our reference */
    if (new_it != NULL)
        do_item_remove(new_it);

    return it;
}

/*** LRU MAINTENANCE THREAD ***/


static int lru_maintainer_free() {
    int did_moves = 0;
    unsigned int freed = 0;

    if ((get_memory_limit() / 4 >= get_memory_available()) && (get_memory_limit() / 4 >= get_memory_free_from_beginning())) {
    	freed = memlog_clean();
        if (settings.verbose > 2)
            fprintf(stderr, "LRU freed %d bytes\n",freed);
    	if (freed > 0) {
    		did_moves++;
    	}
    }

    return did_moves;
}

/* Will crawl all slab classes a minimum of once per hour */
#define MAX_MAINTCRAWL_WAIT 60 * 60

static pthread_t lru_maintainer_tid;

#define MAX_LRU_MAINTAINER_SLEEP 1000000
#define MIN_LRU_MAINTAINER_SLEEP 1000

static void *lru_maintainer_thread(void *arg) {
    useconds_t to_sleep = MIN_LRU_MAINTAINER_SLEEP;
    rel_time_t last_crawler_check = 0;

    pthread_mutex_lock(&lru_maintainer_lock);
    if (settings.verbose > 2)
        fprintf(stderr, "Starting LRU maintainer background thread\n");
    while (do_run_lru_maintainer_thread) {
        int did_moves = 0;
        pthread_mutex_unlock(&lru_maintainer_lock);
        usleep(to_sleep);
        pthread_mutex_lock(&lru_maintainer_lock);

        /* Once per second at most */
        if (last_crawler_check != current_time) {
        	did_moves = lru_maintainer_free();

            last_crawler_check = current_time;
        }

        if (did_moves == 0) {
            if (to_sleep < MAX_LRU_MAINTAINER_SLEEP)
                to_sleep += 1000;
        } else {
            to_sleep /= 2;
            if (to_sleep < MIN_LRU_MAINTAINER_SLEEP)
                to_sleep = MIN_LRU_MAINTAINER_SLEEP;
        }

    }
    pthread_mutex_unlock(&lru_maintainer_lock);
    if (settings.verbose > 2)
        fprintf(stderr, "LRU maintainer thread stopping\n");

    return NULL;
}
int stop_lru_maintainer_thread(void) {
    int ret;
    pthread_mutex_lock(&lru_maintainer_lock);
    /* LRU thread is a sleep loop, will die on its own */
    do_run_lru_maintainer_thread = 0;
    pthread_mutex_unlock(&lru_maintainer_lock);
    if ((ret = pthread_join(lru_maintainer_tid, NULL)) != 0) {
        fprintf(stderr, "Failed to stop LRU maintainer thread: %s\n", strerror(ret));
        return -1;
    }
    settings.lru_maintainer_thread = false;
    return 0;
}

int start_lru_maintainer_thread(void) {
    int ret;

    pthread_mutex_lock(&lru_maintainer_lock);
    do_run_lru_maintainer_thread = 1;
    settings.lru_maintainer_thread = true;
    if ((ret = pthread_create(&lru_maintainer_tid, NULL,
        lru_maintainer_thread, NULL)) != 0) {
        fprintf(stderr, "Can't create LRU maintainer thread: %s\n",
            strerror(ret));
        pthread_mutex_unlock(&lru_maintainer_lock);
        return -1;
    }
    pthread_mutex_unlock(&lru_maintainer_lock);

    return 0;
}

/* If we hold this lock, crawler can't wake up or move */
void lru_maintainer_pause(void) {
    pthread_mutex_lock(&lru_maintainer_lock);
}

void lru_maintainer_resume(void) {
    pthread_mutex_unlock(&lru_maintainer_lock);
}

int init_lru_maintainer(void) {
    if (lru_maintainer_initialized == 0) {
        pthread_mutex_init(&lru_maintainer_lock, NULL);
        lru_maintainer_initialized = 1;
    }
    return 0;
}

/* Tail linkers and crawler for the LRU crawler. */
void do_item_linktail_q(item_metadata *it) { /* item is the new tail */
    return;
}

void do_item_unlinktail_q(item_metadata *it) {
    return;
}

/* This is too convoluted, but it's a difficult shuffle. Try to rewrite it
 * more clearly. */
item_metadata *do_item_crawl_q(item_metadata *it) {
    return NULL; /* success */
}
