//
// Created by 王柯 on 2021-03-31.
//

#ifndef NVMKV_WORT_H
#define NVMKV_WORT_H

#include <stdint.h>
#include <stdbool.h>
#include <vector>
#include "../fastalloc/fastalloc.h"

/* If you want to change the number of entries,
 * change the values of WORT_NODE_BITS & WORT_MAX_DEPTH */
#define WORT_NODE_BITS 4
#define WORT_MAX_DEPTH 15
#define WORT_NUM_NODE_ENTRIES (0x1UL << WORT_NODE_BITS)
#define WORT_LOW_BIT_MASK ((0x1UL << WORT_NODE_BITS) - 1)

#define WORT_MAX_PREFIX_LEN 6
#define WORT_MAX_HEIGHT (WORT_MAX_DEPTH + 1)

#if defined(__GNUC__) && !defined(__clang__)
#if __STDC_VERSION__ >= 199901L && 402 == (__GNUC__ * 100 + __GNUC_MINOR__)
/*
 * GCC 4.2.2's C99 inline keyword support is pretty broken; avoid. Introduced in
 * GCC 4.2.something, fixed in 4.3.0. So checking for specific major.minor of
 * 4.2 is fine.
 */
#define BROKEN_GCC_C99_INLINE
#endif
#endif

typedef int (*wort_callback)(void* data, const unsigned char* key, uint32_t key_len, void* value);

// 统计查询时访问每个节点的延迟（ns）、访问节点的个数
extern std::vector<uint64_t> wort_per_search_per_node_latency_; // 每次搜索时访问每个节点的延迟

// 统计范围查询时树遍历延迟（ns）、段访问个数、每个段扫描延迟（ns）
extern std::vector<uint64_t> wort_range_segment_scan_latency_; // 范围查询每个段扫描延迟
extern std::vector<uint64_t> wort_range_segment_leaf_latency_; // 范围查询每个叶子节点扫描延迟

// 统计每次插入时树遍历延迟（ns）、节点分裂+节点扩容的延迟（ns）或者是叫节点重新调整延迟（ns）、节点更新的延迟（ns）
extern std::vector<uint64_t> wort_per_insert_node_split_latency_;  // 每次插入时更新节点的延迟
extern std::vector<uint64_t> wort_per_insert_node_grow_latency_;   // 每次插入时扩展节点的延迟
extern std::vector<uint64_t> wort_per_insert_node_update_latency_; // 每次插入时更新节点的延迟

extern std::vector<uint64_t> wort_insert_segment_split_time_;  // 插入分段时间
extern std::vector<uint64_t> wort_insert_directory_grow_time_; // 插入目录扩展时间

extern uint64_t wort_memory_usage;

struct wort_key_value {
    uint64_t key;
    uint64_t value;
};

/**
 * This struct is included as pwort
 * of all the various node sizes
 */
typedef struct
{
    unsigned char depth;
    unsigned char pwortial_len;
    unsigned char pwortial[WORT_MAX_PREFIX_LEN];
} wort_node;

/**
 * Full node with 16 children
 */
typedef struct
{
    wort_node n;
    wort_node* children[WORT_NUM_NODE_ENTRIES];
} wort_node16;

/**
 * Represents a leaf. These are
 * of arbitrary size, as they include the key.
 */
typedef struct
{
    void* value;
    uint32_t key_len;
    unsigned long key;
} wort_leaf;

/**
 * Main struct, points to root.
 */
typedef struct
{
    wort_node* root;
    uint64_t size;
} wort_tree;

/**
 * Initializes an wort tree
 * @return 0 on success.
 */
int wort_tree_init(wort_tree* t);

wort_tree* new_wort_tree();

/**
 * Inserts a new value into the wort tree
 * @arg t The tree
 * @arg key The key
 * @arg key_len The length of the key
 * @arg value Opaque value.
 * @return NULL if the item was newly inserted, otherwise
 * the old value pointer is returned.
 */
void* wort_put(wort_tree* t, const unsigned long key, int key_len, void* value, int value_len = 8);

/**
 * Searches for a value in the wort tree
 * @arg t The tree
 * @arg key The key
 * @arg key_len The length of the key
 * @return NULL if the item was not found, otherwise
 * the value pointer is returned.
 */
uint64_t wort_get(const wort_tree* t, const unsigned long key, int key_len);

void wort_all_subtree_kv(wort_node* n, vector<wort_key_value>& res);

void wort_node_scan(wort_node* n, uint64_t left, uint64_t right, uint64_t depth, vector<wort_key_value>& res,
                    int key_len = 8);

vector<wort_key_value> wort_scan(const wort_tree* t, uint64_t left, uint64_t right, int key_len = 8);

uint64_t wort_memory_profile(wort_node* n);

#endif // NVMKV_WORT_H
