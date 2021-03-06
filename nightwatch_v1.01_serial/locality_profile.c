#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include "config.h"
#include "utl_builtin.h"
#include "utl_list.h"
#include "utl_hash_map.h"
#include "context_key.h"
#include "allocator.h"
#include "chunk_monitor.h"
#include "locality_profile.h"
#include "nightwatch.h"



struct size2mtype_entry {
	int check_interval;
	int to_check;
	int mapping_type;
	int valid;
	int n_open;
	int n_restrict;
};

static __thread struct alloc_context map_only_ctx_;

#define S2MT_ENTRY_DECAY_INTERVAL 64

struct locality_profile {
	unsigned long chunk_idx;

	struct hash_map *context_hash_map;
	struct size2mtype_entry *s2t_map;
	
	struct list_head context;
	unsigned long context_idx;
} locality_profile;


static struct list_head free_context;
static struct list_head active_context;

static struct list_head free_chunk;
static struct list_head active_chunk;


static inline void
context_free(struct alloc_context *context)
{
	list_del(&context->p);
	list_del(&context->sibling);
	list_del(&context->chunk);
	list_del(&context->s2c_set);
	list_add(&context->p, &free_context);
}

static inline struct alloc_context *
context_alloc()
{
	struct alloc_context *context;

	if (unlikely(list_empty(&free_context))) {
		context = (struct alloc_context*)
			internal_malloc(OPEN_MAPPING, sizeof(struct alloc_context));
#ifdef USE_ASSERT
		assert(!!context);
#endif /* USE_ASSERT */
	} else {
		context = next_entry(&free_context, struct alloc_context, p);	
		list_del(&context->p);
	}

	list_add(&context->p, &active_context);
	list_init(&context->chunk);
	list_init(&context->sibling);
	list_init(&context->s2c_set);

	return context;
}


static inline void
chunk_free(struct memory_chunk *chunk)
{
	list_del(&chunk->p);
	list_del(&chunk->sibling);
	list_add(&chunk->p, &free_chunk);
}

static inline struct memory_chunk *
chunk_alloc()
{
	struct memory_chunk *chunk;

	if (unlikely(list_empty(&free_chunk))) {
		chunk = (struct memory_chunk*)
			internal_malloc(OPEN_MAPPING, sizeof(struct memory_chunk));
#ifdef USE_ASSERT
		assert(!!chunk);
#endif /* USE_ASSERT */
	} else {
		chunk = next_entry(&free_chunk, struct memory_chunk, p);	
		list_del(&chunk->p);
	}

	chunk->llc_victim_ref = 0;
	chunk->llc_pollutor_ref = 0;
	chunk->total_ref = 0;

	chunk->i_victim_ref = 0;
	chunk->i_pollutor_ref = 0;
	chunk->i_total_ref = 0;

	chunk->sample_cycle = 0;

	chunk->nr_sample = 0;

	list_add(&chunk->p, &active_chunk);
	list_init(&chunk->sample);
	list_init(&chunk->sibling);

	return chunk;
}


#ifdef AT_HOME
void
print_chunk(struct memory_chunk *chunk)
{
	disable_cache_management();
	printf("\n");
	printf("chunk info");
	printf("idx:   %lu\n", chunk->idx);
	printf("size:  %d\n", (int)chunk->size);
	printf("mapping_type:    %d\n", chunk->mapping_type);
	printf("llc_mr:  %.4f\n", chunk->mr);
	printf("victim_sample:   %u\n", chunk->llc_victim_ref);
	printf("pollutor_sample: %u\n", chunk->llc_pollutor_ref);
	printf("total_sample:    %u\n", chunk->total_ref);
	printf("sample_page:     %d\n", chunk->nr_sample);
	printf("sample_cycle:    %d\n", chunk->sample_cycle);
	printf("\n");
	enable_cache_management();
}
#endif /* AT_HOME */


int 
locality_profile_init()
{
	locality_profile.context_hash_map
		=  new_hash_map();
	locality_profile.s2t_map = (struct size2mtype_entry *)
		internal_calloc(RESTRICT_MAPPING, S2C_MAP_SIZE + 1, 
			sizeof(struct size2mtype_entry));

	list_init(&locality_profile.context);
	locality_profile.context_idx = 1;
	locality_profile.chunk_idx = 0;

	list_init(&free_context);
	list_init(&active_context);

	list_init(&free_chunk);
	list_init(&active_chunk);
	
	return 0;
}


void
locality_profile_destroy()
{
	struct alloc_context *context;
	struct memory_chunk *chunk;
	
	list_for_each_entry (context, &locality_profile.context, sibling) {
		list_for_each_entry (chunk, &context->chunk, sibling) {
			stop_monit_chunk(chunk);
#ifdef AT_HOME
			print_chunk(chunk);
#endif /* AT_HOME */
		}
	}

	delete_hash_map(locality_profile.context_hash_map);
	internal_free(locality_profile.s2t_map);
}



static inline void
update_s2t_skip_interval(struct size2mtype_entry *sz2mtype_entry)
{
	if(!sz2mtype_entry->valid) {
		sz2mtype_entry->check_interval = 1;
		sz2mtype_entry->to_check = 1;
	} else if (sz2mtype_entry->to_check == 0) {
	    if (sz2mtype_entry->check_interval < S2C_MAP_UPDATE_INTERVAL_MAX) {
	        sz2mtype_entry->check_interval 
	            += random() % (sz2mtype_entry->check_interval << 1);
	    } else {
	        sz2mtype_entry->check_interval 
	            = (S2C_MAP_UPDATE_INTERVAL_MAX << 1) 
	            + (random() % S2C_MAP_UPDATE_INTERVAL_MAX);
	    }
	    sz2mtype_entry->to_check
	        = sz2mtype_entry->check_interval;
	}
}


void
update_s2c_map(size_t size, int mapping)
{
	struct size2mtype_entry *sz2mtype_entry;
	float r;

	sz2mtype_entry = &locality_profile.s2t_map[size];

	if (mapping == OPEN_MAPPING) {
		sz2mtype_entry->n_open++;	
	} else if (mapping == RESTRICT_MAPPING) {
		sz2mtype_entry->n_restrict++;	
	}

	if (sz2mtype_entry->n_restrict == 0) {
		sz2mtype_entry->mapping_type = OPEN_MAPPING;
		sz2mtype_entry->valid = 1;
	} else if (sz2mtype_entry->n_open == 0) {
		sz2mtype_entry->mapping_type = RESTRICT_MAPPING;
		sz2mtype_entry->valid = 1;
	} else {
		r = (float)sz2mtype_entry->n_open
		    / (float)sz2mtype_entry->n_restrict;
		if (r < 0.1) {
			sz2mtype_entry->valid = 1;
			sz2mtype_entry->mapping_type = RESTRICT_MAPPING;	
		} else if (r <= 10) {
			sz2mtype_entry->valid = 0;
			sz2mtype_entry->mapping_type = OPEN_MAPPING;	
		} else {
			sz2mtype_entry->valid = 1;
			sz2mtype_entry->mapping_type = OPEN_MAPPING;	
		}
	}

	if (sz2mtype_entry->n_open + sz2mtype_entry->n_restrict 
			>= S2MT_ENTRY_DECAY_INTERVAL) {
		sz2mtype_entry->n_open >>= 1;
		sz2mtype_entry->n_restrict >>= 1;
	}
}


static inline void
init_context(uint64_t key, struct alloc_context *context)
{
	context->context_key = key;
	context->predict_type[0] = context->predict_type[1] = UNKNOWN_MAPPING;
	context->predict_mr[0] = context->predict_mr[1] = -1;
	context->sample_skip = 0;
	context->skip_interval = 0;
	context->idx = locality_profile.context_idx++;
	hash_map_add_member(locality_profile.context_hash_map, key, context);
	list_add(&context->sibling, &locality_profile.context);
}


void * 
NightWatch_get_alloc_context(size_t size) 
{
	struct alloc_context *context;
	uint64_t key;
	struct size2mtype_entry *sz2mtype_entry;

	/* quick path */
	sz2mtype_entry = NULL;
	if (unlikely(size > S2C_MAP_SIZE)) {
		goto general_path;
	}

	sz2mtype_entry = &locality_profile.s2t_map[size];

	if (unlikely(!sz2mtype_entry->valid)) {
		goto general_path;
	}

	if (sz2mtype_entry->to_check > 0) {
		sz2mtype_entry->to_check--;
		map_only_ctx_.map_only_context_flag = MAP_ONLY_CONTEXT; 
		map_only_ctx_.map_type = sz2mtype_entry->mapping_type; 
		return &map_only_ctx_;
	}

general_path:
	key = get_context_key();

	context = (struct alloc_context *) 
		hash_map_find_member(locality_profile.context_hash_map, key);
	if (likely(context != NULL)) {
		goto done;
	}
	
	/* new context detected */
	context = context_alloc();	
	init_context(key, context);

done:
	if (likely(size <= S2C_MAP_SIZE)) {
		update_s2t_skip_interval(sz2mtype_entry);
	}
	return context;
}


struct memory_chunk *
attach_chunk_to_context(void *p, size_t size,
		struct alloc_context *context) 
{
	struct memory_chunk *chunk;

	chunk = chunk_alloc();

	chunk->addr = (unsigned long)p;
	chunk->size = size;
	chunk->idx = locality_profile.chunk_idx++;
	chunk->context = context;
	list_add(&chunk->sibling, &context->chunk);

	return chunk;
}


void
detach_chunk_from_context(struct memory_chunk *chunk)
{
#ifdef AT_HOME
	print_chunk(chunk);
#endif /* AT_HOME */
	chunk->context = NULL;
	chunk_free(chunk);
}


void
process_active_chunk(process_func func)
{
	struct memory_chunk *chunk;

	list_for_each_entry (chunk, &active_chunk, p) {
		func(chunk);
	}
}

