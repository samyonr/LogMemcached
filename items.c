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

static item_metadata *heads[LARGEST_ID];
static item_metadata *tails[LARGEST_ID];
static itemstats_t itemstats[LARGEST_ID];
static unsigned int sizes[LARGEST_ID];
static uint64_t sizes_bytes[LARGEST_ID];
static unsigned int *stats_sizes_hist = NULL;
static uint64_t stats_sizes_cas_min = 0;
static int stats_sizes_buckets = 0;

static volatile int do_run_lru_maintainer_thread = 0;
static int lru_maintainer_initialized = 0;
static int lru_maintainer_check_clsid = 0;
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
		if (x == 0) { // first item
			it->prev = 0;
			it->next = 0;
		} else {
			it->prev = 0;
			it->next = (item_metadata *)(ptr - sizeof(item_metadata));
			if (it->next) it->next->prev = it;
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
	first_free_freelist_item = it->next;
	if (it->next) it->next->prev = 0;
	/* Kill flag and initialize refcount here for lock safety in slab
	 * mover's freeness detection. */
	it->it_flags &= ~ITEM_SLABBED;
	it->refcount = 1;
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
	it->prev = 0;
	it->next = first_free_freelist_item;
	if (it->next) it->next->prev = it;
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

static int lru_pull_tail(const int orig_id, const int cur_lru,
        const uint64_t total_bytes, uint8_t flags);

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

static unsigned int noexp_lru_size(int slabs_clsid) {
    int id = CLEAR_LRU(slabs_clsid);
    id |= NOEXP_LRU;
    unsigned int ret;
    pthread_mutex_lock(&lru_locks[id]);
    ret = sizes_bytes[id];
    pthread_mutex_unlock(&lru_locks[id]);
    return ret;
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
static size_t item_make_header(const uint8_t nkey, const unsigned int flags, const int nbytes,
                     char *suffix, uint8_t *nsuffix) {
    /* suffix is defined at 40 chars elsewhere.. */
    *nsuffix = (uint8_t) snprintf(suffix, 40, " %u %d\r\n", flags, nbytes - 2);
    return sizeof(item_data) + nkey + *nsuffix + nbytes;
}

item_metadata *do_item_alloc(char *key, const size_t nkey, const unsigned int flags,
                    const rel_time_t exptime, const int nbytes) {
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

    /* If no memory is available, attempt a direct LRU juggle/eviction */
    /* This is a race in order to simplify lru_pull_tail; in cases where
     * locked items are on the tail, you want them to fall out and cause
     * occasional OOM's, rather than internally work around them.
     * This also gives one fewer code path for slab alloc/free
     */
    /* TODO: if power_largest, try a lot more times? or a number of times
     * based on how many chunks the new object should take up?
     * or based on the size of an object lru_pull_tail() says it evicted?
     * This is a classical GC problem if "large items" are of too varying of
     * sizes. This is actually okay here since the larger the data, the more
     * bandwidth it takes, the more time we can loop in comparison to serving
     * and replacing small items.
     */
    for (i = 0; i < 10; i++) {
        uint64_t total_bytes;
        /* Try to reclaim memory first */
        if (!settings.lru_maintainer_thread) {
            lru_pull_tail(id, COLD_LRU, 0, 0);
        }
        it_data = memlog_alloc(ntotal, &total_bytes, 0);

        if (settings.expirezero_does_not_evict)
            total_bytes -= noexp_lru_size(id);

        if (it_data == NULL) {
            if (settings.lru_maintainer_thread) {
                lru_pull_tail(id, HOT_LRU, total_bytes, 0);
                lru_pull_tail(id, WARM_LRU, total_bytes, 0);
                if (lru_pull_tail(id, COLD_LRU, total_bytes, LRU_PULL_EVICT) <= 0)
                    break;
            } else {
                if (lru_pull_tail(id, COLD_LRU, 0, LRU_PULL_EVICT) <= 0)
                    break;
            }
        } else if ((flags & ITEM_DELETED) != 0) {
        	it_data->it_data_flags |= ITEM_DELETED;
        	it_data->nkey = nkey;
        	it_data->nbytes = nbytes;
			memcpy(ITEM_key(it_data), key, nkey);
			memcpy(ITEM_suffix(it_data), suffix, (size_t)nsuffix);
			it_data->nsuffix = nsuffix;
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
        return NULL;
    }

    //assert(it != heads[id]);

    /* Refcount is seeded to 1 by slabs_alloc() */
    it->next = it->prev = 0;

    /* Items are initially loaded into the HOT_LRU. This is '0' but I want at
     * least a note here. Compiler (hopefully?) optimizes this out.
     */
    if (settings.lru_maintainer_thread) {
        if (exptime == 0 && settings.expirezero_does_not_evict) {
            id |= NOEXP_LRU;
        } else {
            id |= HOT_LRU;
        }
    } else {
        /* There is only COLD in compat-mode */
        id |= COLD_LRU;
    }
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

    return it;
}

void item_free(item_metadata *it) {
    size_t ntotal = ITEM_ntotal(it->item);
    unsigned int clsid;
    assert((it->it_flags & ITEM_LINKED) == 0);
    assert(it != heads[it->slabs_clsid]);
    assert(it != tails[it->slabs_clsid]);
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
	item_metadata **head, **tail;
    assert((it->it_flags & ITEM_SLABBED) == 0);

    head = &heads[it->slabs_clsid];
    tail = &tails[it->slabs_clsid];
    assert(it != *head);
    assert((*head && *tail) || (*head == 0 && *tail == 0));
    it->prev = 0;
    it->next = *head;
    if (it->next) it->next->prev = it;
    *head = it;
    if (*tail == 0) *tail = it;
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
	item_metadata **head, **tail;
    head = &heads[it->slabs_clsid];
    tail = &tails[it->slabs_clsid];

    if (*head == it) {
        assert(it->prev == 0);
        *head = it->next;
    }
    if (*tail == it) {
        assert(it->next == 0);
        *tail = it->prev;
    }
    assert(it->next != it);
    assert(it->prev != it);

    if (it->next) it->next->prev = it->prev;
    if (it->prev) it->prev->next = it->next;
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
    unsigned int memlimit = 2 * 1024 * 1024;   /* 2MB max response size */
    char *buffer;
    unsigned int bufcurr;
    item_metadata *it;
    unsigned int len;
    unsigned int shown = 0;
    char key_temp[KEY_MAX_LENGTH + 1];
    char temp[512];
    unsigned int id = slabs_clsid;
    if (!settings.lru_maintainer_thread)
        id |= COLD_LRU;

    pthread_mutex_lock(&lru_locks[id]);
    it = heads[id];

    buffer = malloc((size_t)memlimit);
    if (buffer == 0) {
        return NULL;
    }
    bufcurr = 0;

    while (it != NULL && (limit == 0 || shown < limit)) {
        assert(it->item->nkey <= KEY_MAX_LENGTH);
        if (it->item->nbytes == 0 && it->item->nkey == 0) {
            it = it->next;
            continue;
        }
        /* Copy the key since it may not be null-terminated in the struct */
        strncpy(key_temp, ITEM_key(it->item), it->item->nkey);
        key_temp[it->item->nkey] = 0x00; /* terminate */
        len = snprintf(temp, sizeof(temp), "ITEM %s [%d b; %lu s]\r\n",
                       key_temp, it->item->nbytes - 2,
                       it->item->exptime == 0 ? 0 :
                       (unsigned long)it->item->exptime + process_started);
        if (bufcurr + len + 6 > memlimit)  /* 6 is END\r\n\0 */
            break;
        memcpy(buffer + bufcurr, temp, len);
        bufcurr += len;
        shown++;
        it = it->next;
    }

    memcpy(buffer + bufcurr, "END\r\n", 6);
    bufcurr += 5;

    *bytes = bufcurr;
    pthread_mutex_unlock(&lru_locks[id]);
    return buffer;
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
            if (lru_type_map[x] == COLD_LRU && tails[i] != NULL)
                age = current_time - tails[i]->time;
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
            it->it_flags |= ITEM_FETCHED|ITEM_ACTIVE;
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
		/* we have it and old_it here - alloc memory to hold both */
		/* flags was already lost - so recover them from ITEM_suffix(it) */

    	uint32_t flags = (uint32_t) strtoul(ITEM_suffix(old_it->item), (char **) NULL, 10);

    	new_it = do_item_alloc(ITEM_key(old_it->item), nkey, flags, exptime, old_it->item->nbytes);

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

/*** LRU MAINTENANCE THREAD ***/

/* Returns number of items remove, expired, or evicted.
 * Callable from worker threads or the LRU maintainer thread */
static int lru_pull_tail(const int orig_id, const int cur_lru,
        const uint64_t total_bytes, uint8_t flags) {
	item_metadata *it = NULL;
    int id = orig_id;
    int removed = 0;
    if (id == 0)
        return 0;

    int tries = 5;
    item_metadata *search;
    item_metadata *next_it;
    void *hold_lock = NULL;
    unsigned int move_to_lru = 0;
    uint64_t limit = 0;

    id |= cur_lru;
    pthread_mutex_lock(&lru_locks[id]);
    search = tails[id];
    /* We walk up *only* for locked items, and if bottom is expired. */
    for (; tries > 0 && search != NULL; tries--, search=next_it) {
        /* we might relink search mid-loop, so search->prev isn't reliable */
        next_it = search->prev;
        if (search->item->nbytes == 0 && search->item->nkey == 0 && search->it_flags == 1) {
            /* We are a crawler, ignore it. */
            if (flags & LRU_PULL_CRAWL_BLOCKS) {
                pthread_mutex_unlock(&lru_locks[id]);
                return 0;
            }
            tries++;
            continue;
        }
        uint32_t hv = hash(ITEM_key(search->item), search->item->nkey);
        /* Attempt to hash item lock the "search" item. If locked, no
         * other callers can incr the refcount. Also skip ourselves. */
        if ((hold_lock = item_trylock(hv)) == NULL)
            continue;
        /* Now see if the item is refcount locked */
        if (refcount_incr(&search->refcount) != 2) {
            /* Note pathological case with ref'ed items in tail.
             * Can still unlink the item, but it won't be reusable yet */
            itemstats[id].lrutail_reflocked++;
            /* In case of refcount leaks, enable for quick workaround. */
            /* WARNING: This can cause terrible corruption */
            if (settings.tail_repair_time &&
                    search->time + settings.tail_repair_time < current_time) {
                itemstats[id].tailrepairs++;
                search->refcount = 1;
                /* This will call item_remove -> item_free since refcnt is 1 */
                do_item_unlink_nolock(search, hv);
                item_trylock_unlock(hold_lock);
                continue;
            }
        }

        /* Expired or flushed */
        if ((search->item->exptime != 0 && search->item->exptime < current_time)
            || item_is_flushed(search)) {
            itemstats[id].reclaimed++;
            if ((search->it_flags & ITEM_FETCHED) == 0) {
                itemstats[id].expired_unfetched++;
            }
            /* refcnt 2 -> 1 */
            do_item_unlink_nolock(search, hv);
            /* refcnt 1 -> 0 -> item_free */
            do_item_remove(search);
            item_trylock_unlock(hold_lock);
            removed++;

            /* If all we're finding are expired, can keep going */
            continue;
        }

        /* If we're HOT_LRU or WARM_LRU and over size limit, send to COLD_LRU.
         * If we're COLD_LRU, send to WARM_LRU unless we need to evict
         */
        switch (cur_lru) {
            case HOT_LRU:
                limit = total_bytes * settings.hot_lru_pct / 100;
            case WARM_LRU:
                if (limit == 0)
                    limit = total_bytes * settings.warm_lru_pct / 100;
                if (sizes_bytes[id] > limit) {
                    itemstats[id].moves_to_cold++;
                    move_to_lru = COLD_LRU;
                    do_item_unlink_q(search);
                    it = search;
                    removed++;
                    break;
                } else if ((search->it_flags & ITEM_ACTIVE) != 0) {
                    /* Only allow ACTIVE relinking if we're not too large. */
                    itemstats[id].moves_within_lru++;
                    search->it_flags &= ~ITEM_ACTIVE;
                    do_item_update_nolock(search);
                    do_item_remove(search);
                    item_trylock_unlock(hold_lock);
                } else {
                    /* Don't want to move to COLD, not active, bail out */
                    it = search;
                }
                break;
            case COLD_LRU:
                it = search; /* No matter what, we're stopping */
                if (flags & LRU_PULL_EVICT) {
                    if (settings.evict_to_free == 0) {
                        /* Don't think we need a counter for this. It'll OOM.  */
                        break;
                    }
                    itemstats[id].evicted++;
                    itemstats[id].evicted_time = current_time - search->time;
                    if (search->item->exptime != 0)
                        itemstats[id].evicted_nonzero++;
                    if ((search->it_flags & ITEM_FETCHED) == 0) {
                        itemstats[id].evicted_unfetched++;
                    }
                    LOGGER_LOG(NULL, LOG_EVICTIONS, LOGGER_EVICTION, search);
                    do_item_unlink_nolock(search, hv);
                    removed++;
                    if (settings.slab_automove == 2) {
                        //slabs_reassign(-1, orig_id);
                    }
                } else if ((search->it_flags & ITEM_ACTIVE) != 0
                        && settings.lru_maintainer_thread) {
                    itemstats[id].moves_to_warm++;
                    search->it_flags &= ~ITEM_ACTIVE;
                    move_to_lru = WARM_LRU;
                    do_item_unlink_q(search);
                    removed++;
                }
                break;
        }
        if (it != NULL)
            break;
    }

    pthread_mutex_unlock(&lru_locks[id]);

    if (it != NULL) {
        if (move_to_lru) {
            it->slabs_clsid = ITEM_clsid(it);
            it->slabs_clsid |= move_to_lru;
            item_link_q(it);
        }
        do_item_remove(it);
        item_trylock_unlock(hold_lock);
    }

    return removed;
}

/* Loop up to N times:
 * If too many items are in HOT_LRU, push to COLD_LRU
 * If too many items are in WARM_LRU, push to COLD_LRU
 * If too many items are in COLD_LRU, poke COLD_LRU tail
 * 1000 loops with 1ms min sleep gives us under 1m items shifted/sec. The
 * locks can't handle much more than that. Leaving a TODO for how to
 * autoadjust in the future.
 */
static int lru_maintainer_juggle(const int slabs_clsid) {
    int i;
    int did_moves = 0;
    //bool mem_limit_reached = false;
    uint64_t total_bytes = 0;
    unsigned int chunks_perslab = 0;
    unsigned int chunks_free = 0;
    /* TODO: if free_chunks below high watermark, increase aggressiveness */
    //chunks_free = slabs_available_chunks(slabs_clsid, &mem_limit_reached,
    //        &total_bytes, &chunks_perslab);
    if (settings.expirezero_does_not_evict)
        total_bytes -= noexp_lru_size(slabs_clsid);

    /* If slab automove is enabled on any level, and we have more than 2 pages
     * worth of chunks free in this class, ask (gently) to reassign a page
     * from this class back into the global pool (0)
     */
    if (settings.slab_automove > 0 && chunks_free > (chunks_perslab * 2.5)) {
        //slabs_reassign(slabs_clsid, SLAB_GLOBAL_PAGE_POOL);
    }

    /* Juggle HOT/WARM up to N times */
    for (i = 0; i < 1000; i++) {
        int do_more = 0;
        if (lru_pull_tail(slabs_clsid, HOT_LRU, total_bytes, LRU_PULL_CRAWL_BLOCKS) ||
            lru_pull_tail(slabs_clsid, WARM_LRU, total_bytes, LRU_PULL_CRAWL_BLOCKS)) {
            do_more++;
        }
        do_more += lru_pull_tail(slabs_clsid, COLD_LRU, total_bytes, LRU_PULL_CRAWL_BLOCKS);
        if (do_more == 0)
            break;
        did_moves++;
    }
    return did_moves;
}

/* Will crawl all slab classes a minimum of once per hour */
#define MAX_MAINTCRAWL_WAIT 60 * 60

/* Hoping user input will improve this function. This is all a wild guess.
 * Operation: Kicks crawler for each slab id. Crawlers take some statistics as
 * to items with nonzero expirations. It then buckets how many items will
 * expire per minute for the next hour.
 * This function checks the results of a run, and if it things more than 1% of
 * expirable objects are ready to go, kick the crawler again to reap.
 * It will also kick the crawler once per minute regardless, waiting a minute
 * longer for each time it has no work to do, up to an hour wait time.
 * The latter is to avoid newly started daemons from waiting too long before
 * retrying a crawl.
 */
static void lru_maintainer_crawler_check(struct crawler_expired_data *cdata) {
    int i;
    static rel_time_t next_crawls[MAX_NUMBER_OF_SLAB_CLASSES];
    static rel_time_t next_crawl_wait[MAX_NUMBER_OF_SLAB_CLASSES];
    uint8_t todo[MAX_NUMBER_OF_SLAB_CLASSES];
    memset(todo, 0, sizeof(uint8_t) * MAX_NUMBER_OF_SLAB_CLASSES);
    bool do_run = false;
    if (!cdata->crawl_complete) {
        return;
    }

    for (i = POWER_SMALLEST; i < MAX_NUMBER_OF_SLAB_CLASSES; i++) {
        crawlerstats_t *s = &cdata->crawlerstats[i];
        /* We've not successfully kicked off a crawl yet. */
        if (s->run_complete) {
            pthread_mutex_lock(&cdata->lock);
            int x;
            /* Should we crawl again? */
            uint64_t possible_reclaims = s->seen - s->noexp;
            uint64_t available_reclaims = 0;
            /* Need to think we can free at least 1% of the items before
             * crawling. */
            /* FIXME: Configurable? */
            uint64_t low_watermark = (s->seen / 100) + 1;
            rel_time_t since_run = current_time - s->end_time;
            /* Don't bother if the payoff is too low. */
            if (settings.verbose > 1)
                fprintf(stderr, "maint crawler[%d]: low_watermark: %llu, possible_reclaims: %llu, since_run: %u\n",
                        i, (unsigned long long)low_watermark, (unsigned long long)possible_reclaims,
                        (unsigned int)since_run);
            for (x = 0; x < 60; x++) {
                available_reclaims += s->histo[x];
                if (available_reclaims > low_watermark) {
                    if (next_crawl_wait[i] < (x * 60)) {
                        next_crawl_wait[i] += 60;
                    } else if (next_crawl_wait[i] >= 60) {
                        next_crawl_wait[i] -= 60;
                    }
                    break;
                }
            }

            if (available_reclaims == 0) {
                next_crawl_wait[i] += 60;
            }

            if (next_crawl_wait[i] > MAX_MAINTCRAWL_WAIT) {
                next_crawl_wait[i] = MAX_MAINTCRAWL_WAIT;
            }

            next_crawls[i] = current_time + next_crawl_wait[i] + 5;
            if (settings.verbose > 1)
                fprintf(stderr, "maint crawler[%d]: next_crawl: %u, [%d] now: [%d]\n",
                        i, next_crawl_wait[i], next_crawls[i], current_time);
            // Got our calculation, avoid running until next actual run.
            s->run_complete = false;
            pthread_mutex_unlock(&cdata->lock);
        }
        if (current_time > next_crawls[i]) {
            todo[i] = 1;
            do_run = true;
            next_crawls[i] = current_time + 5; // minimum retry wait.
        }
    }
    if (do_run) {
        lru_crawler_start(todo, 0, CRAWLER_EXPIRED, cdata, NULL, 0);
    }
}

static pthread_t lru_maintainer_tid;

#define MAX_LRU_MAINTAINER_SLEEP 1000000
#define MIN_LRU_MAINTAINER_SLEEP 1000

static void *lru_maintainer_thread(void *arg) {
    int i;
    useconds_t to_sleep = MIN_LRU_MAINTAINER_SLEEP;
    rel_time_t last_crawler_check = 0;
    struct crawler_expired_data cdata;
    memset(&cdata, 0, sizeof(struct crawler_expired_data));
    pthread_mutex_init(&cdata.lock, NULL);
    cdata.crawl_complete = true; // kick off the crawler.

    pthread_mutex_lock(&lru_maintainer_lock);
    if (settings.verbose > 2)
        fprintf(stderr, "Starting LRU maintainer background thread\n");
    while (do_run_lru_maintainer_thread) {
        int did_moves = 0;
        pthread_mutex_unlock(&lru_maintainer_lock);
        usleep(to_sleep);
        pthread_mutex_lock(&lru_maintainer_lock);

        STATS_LOCK();
        stats.lru_maintainer_juggles++;
        STATS_UNLOCK();
        /* We were asked to immediately wake up and poke a particular slab
         * class due to a low watermark being hit */
        if (lru_maintainer_check_clsid != 0) {
            did_moves = lru_maintainer_juggle(lru_maintainer_check_clsid);
            lru_maintainer_check_clsid = 0;
        } else {
            for (i = POWER_SMALLEST; i < MAX_NUMBER_OF_SLAB_CLASSES; i++) {
                did_moves += lru_maintainer_juggle(i);
            }
        }
        if (did_moves == 0) {
            if (to_sleep < MAX_LRU_MAINTAINER_SLEEP)
                to_sleep += 1000;
        } else {
            to_sleep /= 2;
            if (to_sleep < MIN_LRU_MAINTAINER_SLEEP)
                to_sleep = MIN_LRU_MAINTAINER_SLEEP;
        }
        /* Once per second at most */
        if (settings.lru_crawler && last_crawler_check != current_time) {
            lru_maintainer_crawler_check(&cdata);
            last_crawler_check = current_time;
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
	item_metadata **head, **tail;
    assert(it->it_flags == 1);
    assert(it->item->nbytes == 0);

    head = &heads[it->slabs_clsid];
    tail = &tails[it->slabs_clsid];
    //assert(*tail != 0);
    assert(it != *tail);
    assert((*head && *tail) || (*head == 0 && *tail == 0));
    it->prev = *tail;
    it->next = 0;
    if (it->prev) {
        assert(it->prev->next == 0);
        it->prev->next = it;
    }
    *tail = it;
    if (*head == 0) *head = it;
    return;
}

void do_item_unlinktail_q(item_metadata *it) {
	item_metadata **head, **tail;
    head = &heads[it->slabs_clsid];
    tail = &tails[it->slabs_clsid];

    if (*head == it) {
        assert(it->prev == 0);
        *head = it->next;
    }
    if (*tail == it) {
        assert(it->next == 0);
        *tail = it->prev;
    }
    assert(it->next != it);
    assert(it->prev != it);

    if (it->next) it->next->prev = it->prev;
    if (it->prev) it->prev->next = it->next;
    return;
}

/* This is too convoluted, but it's a difficult shuffle. Try to rewrite it
 * more clearly. */
item_metadata *do_item_crawl_q(item_metadata *it) {
	item_metadata **head, **tail;
    assert(it->it_flags == 1);
    assert(it->item->nbytes == 0);
    head = &heads[it->slabs_clsid];
    tail = &tails[it->slabs_clsid];

    /* We've hit the head, pop off */
    if (it->prev == 0) {
        assert(*head == it);
        if (it->next) {
            *head = it->next;
            assert(it->next->prev == it);
            it->next->prev = 0;
        }
        return NULL; /* Done */
    }

    /* Swing ourselves in front of the next item */
    /* NB: If there is a prev, we can't be the head */
    assert(it->prev != it);
    if (it->prev) {
        if (*head == it->prev) {
            /* Prev was the head, now we're the head */
            *head = it;
        }
        if (*tail == it) {
            /* We are the tail, now they are the tail */
            *tail = it->prev;
        }
        assert(it->next != it);
        if (it->next) {
            assert(it->prev->next == it);
            it->prev->next = it->next;
            it->next->prev = it->prev;
        } else {
            /* Tail. Move this above? */
            it->prev->next = 0;
        }
        /* prev->prev's next is it->prev */
        it->next = it->prev;
        it->prev = it->next->prev;
        it->next->prev = it;
        /* New it->prev now, if we're not at the head. */
        if (it->prev) {
            it->prev->next = it;
        }
    }
    assert(it->next != it);
    assert(it->prev != it);

    return it->next; /* success */
}
