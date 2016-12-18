#define HOT_LRU 0
#define WARM_LRU 64
#define COLD_LRU 128
#define NOEXP_LRU 192

#define CLEAR_LRU(id) (id & ~(3<<6))

/* See items.c */
uint64_t get_cas_id(void);


void freelist_init(const size_t limit);
/** Allocate object */ /*@null@*/
void *freelist_alloc(void);
/** Free previously allocated object */
void freelist_free(void *ptr, size_t size, unsigned int id);

/*@null@*/
item_metadata *do_item_alloc(char *key, const size_t nkey, const unsigned int flags, const rel_time_t exptime, const int nbytes, bool *succeed);
void item_free(item_metadata *it);
bool item_size_ok(const size_t nkey, const int flags, const int nbytes);
size_t item_make_header(const uint8_t nkey, const unsigned int flags, const int nbytes, char *suffix, uint8_t *nsuffix);
int  do_item_link(item_metadata *it, const uint32_t hv);     /** may fail if transgresses limits */
void do_item_unlink(item_metadata *it, const uint32_t hv);
void do_item_unlink_nolock(item_metadata *it, const uint32_t hv);
void do_item_remove(item_metadata *it);
enum store_item_type do_item_update(item_metadata *it);  /** update LRU time to current and reposition */
void do_item_update_nolock(item_metadata *it);
int  do_item_replace(item_metadata *it, item_metadata *new_it, const uint32_t hv);

int item_is_flushed(item_metadata *it);

void do_item_linktail_q(item_metadata *it);
void do_item_unlinktail_q(item_metadata *it);
item_metadata *do_item_crawl_q(item_metadata *it);

/*@null@*/
char *item_cachedump(const unsigned int slabs_clsid, const unsigned int limit, unsigned int *bytes);
void item_stats(ADD_STAT add_stats, void *c);
void do_item_stats_add_crawl(const int i, const uint64_t reclaimed,
        const uint64_t unfetched, const uint64_t checked);
void item_stats_totals(ADD_STAT add_stats, void *c);
/*@null@*/
void item_stats_sizes(ADD_STAT add_stats, void *c);
void item_stats_sizes_init(void);
void item_stats_sizes_enable(ADD_STAT add_stats, void *c);
void item_stats_sizes_disable(ADD_STAT add_stats, void *c);
void item_stats_sizes_add(item_metadata *it);
void item_stats_sizes_remove(item_metadata *it);
bool item_stats_sizes_status(void);

item_metadata *do_item_get(const char *key, const size_t nkey, const uint32_t hv, conn *c);
item_metadata *do_item_touch(const char *key, const size_t nkey, uint32_t exptime, const uint32_t hv, conn *c);
item_metadata *do_item_get_update(item_metadata *old_it, const uint32_t hv, enum store_item_type *stored_state);
void item_stats_reset(void);
extern pthread_mutex_t lru_locks[POWER_LARGEST];

int start_lru_maintainer_thread(void);
int stop_lru_maintainer_thread(void);
int init_lru_maintainer(void);
void lru_maintainer_pause(void);
void lru_maintainer_resume(void);
