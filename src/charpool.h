#ifndef CHARPOOL_H
#define CHARPOOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "aligned/aligned.h"
#include "bit_utils/bit_utils.h"

#ifndef CHARPOOL_MALLOC
#define CHARPOOL_MALLOC(size) malloc(size)
#define CHARPOOL_MALLOC_DEFINED
#endif
#ifndef CHARPOOL_CALLOC
#define CHARPOOL_CALLOC(num, size) calloc(num, size)
#define CHARPOOL_CALLOC_DEFINED
#endif
#ifndef CHARPOOL_FREE
#define CHARPOOL_FREE(ptr) free(ptr)
#define CHARPOOL_FREE_DEFINED
#endif

#ifndef CHARPOOL_ALIGNMENT
#define CHARPOOL_ALIGNMENT CACHE_LINE_SIZE
#define CHARPOOL_ALIGNMENT_DEFINED
#endif

#ifndef CHARPOOL_ALIGNED_MALLOC
#define CHARPOOL_ALIGNED_MALLOC(size, alignment) aligned_malloc(size, alignment)
#define CHARPOOL_ALIGNED_MALLOC_DEFINED
#endif
#ifndef CHARPOOL_ALIGNED_FREE
#define CHARPOOL_ALIGNED_FREE(ptr) aligned_free(ptr)
#define CHARPOOL_ALIGNED_FREE_DEFINED
#endif


#ifndef CHARPOOL_DEFAULT_BLOCK_SIZE
#define CHARPOOL_DEFAULT_BLOCK_SIZE 4096
#define CHARPOOL_DEFAULT_BLOCK_SIZE_DEFINED
#endif

typedef struct charpool_block {
    struct charpool_block *next;
    size_t block_size;
    size_t block_index;
    char *data;
} charpool_block_t;

#define STACK_NAME small_string_stack
#define STACK_TYPE char *
#include "stack/stack.h"
#undef STACK_NAME
#undef STACK_TYPE

#if ((UINTPTR_MAX == 0xFFFFFFFFFFFFFFFFu))
#define MAX_SMALL_STRING_SIZE 8
#else
#define MAX_SMALL_STRING_SIZE 4
#endif

typedef union charpool_free_string {
    union charpool_free_string *next;
    char *value;
} charpool_free_string_t;

typedef struct charpool {
    uint8_t small_string_min_size;
    uint8_t small_string_max_size;
    uint8_t small_string_level_threshold;
    uint8_t num_free_lists;
    size_t block_size;
    small_string_stack_node_memory_pool *small_string_free_list_node_pool;
    small_string_stack *small_string_free_lists;
    charpool_free_string_t *free_lists;
    charpool_block_t *block;
} charpool_t;


static charpool_block_t *charpool_block_new(size_t block_size) {
    charpool_block_t *block = CHARPOOL_MALLOC(sizeof(charpool_block_t));
    if (block == NULL) return NULL;

    char *data = CHARPOOL_ALIGNED_MALLOC(block_size, CHARPOOL_ALIGNMENT);
    if (data == NULL) {
        CHARPOOL_FREE(block);
        return NULL;
    }
    block->data = data;
    block->next = NULL;
    block->block_index = 0;
    return block;
}

static void charpool_block_free_data(charpool_block_t *block) {
    if (block == NULL || block->data == NULL) return;
    char *str = block->data;
    block->data = NULL;
    CHARPOOL_ALIGNED_FREE(str);
}

static void charpool_block_destroy(charpool_block_t *block) {
    if (block == NULL) return;
    charpool_block_free_data(block);
    CHARPOOL_FREE(block);
}

typedef struct charpool_options {
    uint8_t small_string_min_size;
    uint8_t small_string_max_size;
    size_t block_size;
} charpool_options_t;

static charpool_options_t charpool_default_options(void) {
    return (charpool_options_t) {
        .small_string_min_size = 1,
        .small_string_max_size = MAX_SMALL_STRING_SIZE,
        .block_size = CHARPOOL_DEFAULT_BLOCK_SIZE,
    };
}

static bool charpool_init_options(charpool_t *pool, const charpool_options_t options) {
    if (pool == NULL || options.small_string_min_size < 1 || options.small_string_min_size > options.small_string_max_size || !is_power_of_two(options.block_size) || !is_power_of_two(options.small_string_max_size)) {
        return false;
    }

    pool->small_string_min_size = options.small_string_min_size;
    pool->small_string_max_size = options.small_string_max_size;
    pool->block_size = options.block_size;

    pool->small_string_free_lists = CHARPOOL_MALLOC(sizeof(small_string_stack) * (pool->small_string_max_size - pool->small_string_min_size));
    if (pool->small_string_free_lists == NULL) {
        return false;
    }

    small_string_stack_node_memory_pool *stack_node_pool = small_string_stack_node_memory_pool_new();
    if (stack_node_pool == NULL) {
        CHARPOOL_FREE(pool->small_string_free_lists);
        pool->small_string_free_lists = NULL;
        return false;
    }

    pool->small_string_free_list_node_pool = stack_node_pool;

    for (uint8_t i = 0; i < (pool->small_string_max_size - pool->small_string_min_size); i++) {
        if (!small_string_stack_init_pool(&pool->small_string_free_lists[i], stack_node_pool)) {
            small_string_stack_node_memory_pool_destroy(stack_node_pool);
            CHARPOOL_FREE(pool->small_string_free_lists);
            pool->small_string_free_lists = NULL;
            return false;
        }
    }

    pool->small_string_level_threshold = floor_log2((size_t)pool->small_string_max_size);
    uint8_t num_free_lists = floor_log2(pool->block_size) - pool->small_string_level_threshold;
    if (num_free_lists == 0) {
        num_free_lists = 1;
    }

    pool->free_lists = NULL;
    pool->free_lists = CHARPOOL_MALLOC(sizeof(charpool_free_string_t) * num_free_lists);

    if (pool->free_lists == NULL) {
        small_string_stack_node_memory_pool_destroy(stack_node_pool);
        CHARPOOL_FREE(pool->small_string_free_lists);
        pool->small_string_free_lists = NULL;
        return false;
    }
    pool->num_free_lists = num_free_lists;
    for (size_t i = 0; i < num_free_lists; i++) {
        pool->free_lists[i].value = NULL;
    }

    charpool_block_t *block = charpool_block_new(pool->block_size);
    if (block == NULL) {
        CHARPOOL_FREE(pool->free_lists);
        pool->free_lists = NULL;
        small_string_stack_node_memory_pool_destroy(stack_node_pool);
        CHARPOOL_FREE(pool->small_string_free_lists);
        pool->small_string_free_lists = NULL;
        return false;
    }
    pool->block = block;
    return true;
}

static bool charpool_init(charpool_t *pool) {
    return charpool_init_options(pool, charpool_default_options());
}

static charpool_t *charpool_new(void) {
    charpool_t *pool = CHARPOOL_MALLOC(sizeof(charpool_t));
    if (pool == NULL) return NULL;

    if (!charpool_init(pool)) {
        CHARPOOL_FREE(pool);
        return NULL;
    }
    
    return pool;
}

static charpool_t *charpool_new_options(const charpool_options_t options) {
    charpool_t *pool = CHARPOOL_MALLOC(sizeof(charpool_t));
    if (pool == NULL) return NULL;
    if (!charpool_init_options(pool, options)) {
        CHARPOOL_FREE(pool);
        return NULL;
    }
    return pool;
}


static bool charpool_release_size(charpool_t *pool, char *str, size_t size) {
    if (pool == NULL || str == NULL || size < pool->small_string_min_size) return false;
    if (size < pool->small_string_max_size) {
        if (small_string_stack_push(&pool->small_string_free_lists[size - pool->small_string_min_size], str)) {
            return true;
        } else {
            return false;
        }
    }

    if (size >= pool->block_size) {
        CHARPOOL_ALIGNED_FREE(str);
        return true;
    }

    /* Release to floor(log2(size)) free list, which guarantees that the free list at level i
     * contains all strings of size 2^i or larger
    */
    uint8_t level = (uint8_t)floor_log2(size) - pool->small_string_level_threshold;

    charpool_free_string_t *head = &pool->free_lists[level];
    ((charpool_free_string_t *)str)->next = head;
    pool->free_lists[level].value = str;

    return true;
}

static char *charpool_alloc(charpool_t *pool, size_t size) {
    if (pool == NULL || size < pool->small_string_min_size) return NULL;

    char *result = NULL;

    // Large string allocation (>= block size)
    if (size >= pool->block_size) {
        return CHARPOOL_ALIGNED_MALLOC(size, CHARPOOL_ALIGNMENT);
    }

    // Small string allocation (< small_string_max_size, typically the pointer size)
    for (size_t i = size - pool->small_string_min_size; i < pool->small_string_max_size - pool->small_string_min_size; i++) {
        if (small_string_stack_pop(&pool->small_string_free_lists[i], &result)) {
            return result;
        }
    }

    uint8_t level = (uint8_t)ceil_log2(size) - pool->small_string_level_threshold;
    uint8_t max_level = pool->num_free_lists;
    for (uint8_t j = level; j < max_level; j++) {
        charpool_free_string_t *head = &pool->free_lists[j];
        if (head->value != NULL) {
            result = head->value;
            pool->free_lists[j].next = head->next;
            return result;
        }
    }

    size_t index = pool->block->block_index;

    if (index + size > pool->block_size) {
        charpool_block_t *block = pool->block;
        charpool_block_t *new_block = charpool_block_new(size);
        if (new_block == NULL) {
            return NULL;
        }
        new_block->next = pool->block;
        pool->block = new_block;
        new_block->block_index = size;

        if (index < pool->block_size && pool->block_size - index >= pool->small_string_min_size) {
            charpool_release_size(pool, block->data + index, pool->block_size - index);
        }
        return new_block->data;
    }

    result = pool->block->data + index;
    pool->block->block_index += size;
    return result;
}


static char *charpool_strndup(charpool_t *pool, const char *str, size_t n) {
    if (pool == NULL || str == NULL || n == 0) return NULL;

    char *result = charpool_alloc(pool, n + 1);
    if (result == NULL) {
        return NULL;
    }
    memcpy(result, str, n);
    result[n] = '\0';
    return result;
}

static char *charpool_strdup(charpool_t *pool, const char *str) {
    if (pool == NULL || str == NULL) return NULL;

    return charpool_strndup(pool, str, strlen(str));
}


static void charpool_destroy(charpool_t *pool) {
    if (pool == NULL) return;

    charpool_block_t *block = pool->block;
    while (block != NULL) {
        charpool_block_t *next = block->next;
        charpool_block_destroy(block);
        block = next;
    }

    if (pool->small_string_free_lists != NULL) {
        CHARPOOL_FREE(pool->small_string_free_lists);
        pool->small_string_free_lists = NULL;
    }

    if (pool->small_string_free_list_node_pool != NULL) {
        small_string_stack_node_memory_pool_destroy(pool->small_string_free_list_node_pool);
        pool->small_string_free_list_node_pool = NULL;
    }

    if (pool->free_lists != NULL) {
        CHARPOOL_FREE(pool->free_lists);
        pool->free_lists = NULL;
    }
    CHARPOOL_FREE(pool);
}


#ifdef CHARPOOL_DEFAULT_BLOCK_SIZE_DEFINED
#undef CHARPOOL_DEFAULT_BLOCK_SIZE
#undef CHARPOOL_DEFAULT_BLOCK_SIZE_DEFINED
#endif
#ifdef CHARPOOL_MALLOC_DEFINED
#undef CHARPOOL_MALLOC
#undef CHARPOOL_MALLOC_DEFINED
#endif
#ifdef CHARPOOL_CALLOC_DEFINED
#undef CHARPOOL_CALLOC
#undef CHARPOOL_CALLOC_DEFINED
#endif
#ifdef CHARPOOL_FREE_DEFINED
#undef CHARPOOL_FREE
#undef CHARPOOL_FREE_DEFINED
#endif
#ifdef CHARPOOL_ALIGNMENT_DEFINED
#undef CHARPOOL_ALIGNMENT
#undef CHARPOOL_ALIGNMENT_DEFINED
#endif
#ifdef CHARPOOL_ALIGNED_MALLOC_DEFINED
#undef CHARPOOL_ALIGNED_MALLOC
#undef CHARPOOL_ALIGNED_MALLOC_DEFINED
#endif
#ifdef CHARPOOL_ALIGNED_FREE_DEFINED
#undef CHARPOOL_ALIGNED_FREE
#undef CHARPOOL_ALIGNED_FREE_DEFINED
#endif

#endif // CHARPOOL_H
