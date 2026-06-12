#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <emmintrin.h>
#include <assert.h>
#include <x86intrin.h>
#include <math.h>
#include <chrono> // 添加高精度时间测量头文件
#include "wort.h"

/**
 * Macros to manipulate pointer tags
 */
#define IS_LEAF(x) (((uintptr_t)x & 1))                        // 检查是否为叶子
#define SET_LEAF(x) ((void*)((uintptr_t)x | 1))                // 设置叶子标记
#define LEAF_RAW(x) ((wort_leaf*)((void*)((uintptr_t)x & ~1))) // 获取原始叶子指针

#define CACHE_LINE_SIZE 64

// 统计查询时访问每个节点的延迟（ns）、访问节点的个数
std::vector<uint64_t> wort_per_search_per_node_latency_ = {}; // 每次搜索时访问每个节点的延迟

// 统计范围查询时树遍历延迟（ns）、段访问个数、每个段扫描延迟（ns）
std::vector<uint64_t> wort_range_segment_scan_latency_ = {}; // 范围查询每个段扫描延迟
std::vector<uint64_t> wort_range_segment_leaf_latency_ = {}; // 范围查询每个段扫描延迟

// 统计每次插入时树遍历延迟（ns）、节点分裂+节点扩容的延迟（ns）或者是叫节点重新调整延迟（ns）、节点更新的延迟（ns）
std::vector<uint64_t> wort_per_insert_node_split_latency_ = {}; // 每次插入时更新节点的延迟
std::vector<uint64_t> wort_per_insert_node_grow_latency_ = {};  // 每次插入时扩展节点的延迟
std::vector<uint64_t> wort_per_insert_node_update_latency_ = {};

std::vector<uint64_t> wort_insert_segment_split_time_ = {};  // 插入分段时间
std::vector<uint64_t> wort_insert_directory_grow_time_ = {}; // 插入目录扩展时间
uint64_t wort_memory_usage = 0;

static inline void mfence() {
    asm volatile("mfence" ::: "memory");
}

static void flush_buffer(void* buf, unsigned long len, bool fence) {
    unsigned long i;
    len = len + ((unsigned long)(buf) & (CACHE_LINE_SIZE - 1));
    if (fence) {
        mfence();
        for (i = 0; i < len; i += CACHE_LINE_SIZE) {
            asm volatile("clflush %0\n" : "+m"(*((char*)buf + i)));
        }
        mfence();
    } else {
        for (i = 0; i < len; i += CACHE_LINE_SIZE) {
            asm volatile("clflush %0\n" : "+m"(*((char*)buf + i)));
        }
    }
}

static int get_index(unsigned long key, int depth) {
    int index;

    index = ((key >> ((WORT_MAX_DEPTH - depth) * WORT_NODE_BITS)) & WORT_LOW_BIT_MASK);
    return index;
}

/**
 * Allocates a node of the given type,
 * initializes to zero and sets the type.
 */
static wort_node* alloc_node() {
    wort_node* n;
    void* ret;
    ret = concurrency_fast_alloc(sizeof(wort_node16));
    wort_memory_usage += sizeof(wort_node16);
    //    posix_memalign(&ret, 64, sizeof(wort_node16));
    n = static_cast<wort_node*>(ret);
    memset(n, 0, sizeof(wort_node16));
    return n;
}

/**
 * Initializes an wort tree
 * @return 0 on success.
 */
int wort_tree_init(wort_tree* t) {
    t->root = NULL;
    t->size = 0;
    return 0;
}

wort_tree* new_wort_tree() {
    wort_tree* _new_wort_tree = static_cast<wort_tree*>(concurrency_fast_alloc(sizeof(wort_tree)));
    wort_memory_usage += sizeof(wort_tree);
    wort_tree_init(_new_wort_tree);
    return _new_wort_tree;
}

static wort_node** find_child(wort_node* n, unsigned char c) {
    wort_node16* p;

    p = (wort_node16*)n;
    if (p->children[c])
        return &p->children[c];

    return NULL;
}

// Simple inlined if
static inline int min(int a, int b) {
    return (a < b) ? a : b;
}

/**
 * Returns the number of prefix characters shared between
 * the key and node.
 */
static int check_prefix(const wort_node* n, const unsigned long key, int key_len, int depth) {
    //	int max_cmp = min(min(n->pwortial_len, WORT_MAX_PREFIX_LEN), (key_len * INDEX_BITS) - depth);
    int max_cmp = min(min(n->pwortial_len, WORT_MAX_PREFIX_LEN), WORT_MAX_HEIGHT - depth);
    int idx;
    for (idx = 0; idx < max_cmp; idx++) {
        if (n->pwortial[idx] != get_index(key, depth + idx))
            return idx;
    }
    return idx;
}

/**
 * Checks if a leaf matches
 * @return 0 on success.
 */
static int leaf_matches(const wort_leaf* n, unsigned long key, int key_len, int depth) {
    (void)depth;
    // Fail if the key lengths are different
    if (n->key_len != (uint32_t)key_len)
        return 1;

    // Compare the keys stworting at the depth
    //	return memcmp(n->key, key, key_len);
    return !(n->key == key);
}

// Find the minimum leaf under a node
static wort_leaf* minimum(const wort_node* n) {
    // Handle base cases
    if (!n)
        return NULL;
    if (IS_LEAF(n))
        return LEAF_RAW(n);

    int idx = 0;

    while (!((wort_node16*)n)->children[idx])
        idx++;
    return minimum(((wort_node16*)n)->children[idx]);
}

static int longest_common_prefix(wort_leaf* l1, wort_leaf* l2, int depth) {
    //	int idx, max_cmp = (min(l1->key_len, l2->key_len) * INDEX_BITS) - depth;
    int idx, max_cmp = WORT_MAX_HEIGHT - depth;

    for (idx = 0; idx < max_cmp; idx++) {
        if (get_index(l1->key, depth + idx) != get_index(l2->key, depth + idx))
            return idx;
    }
    return idx;
}

/**
 * Searches for a value in the wort tree
 * @arg t The tree
 * @arg key The key
 * @arg key_len The length of the key
 * @return NULL if the item was not found, otherwise
 * the value pointer is returned.
 */
uint64_t wort_get(const wort_tree* t, const unsigned long key, int key_len) {
    wort_node** child;
    wort_node* n = t->root;
    int prefix_len, depth = 0;

    while (n) {
        // Might be a leaf
        if (IS_LEAF(n)) {
            n = (wort_node*)LEAF_RAW(n);
            // Check if the expanded path matches
            if (!leaf_matches((wort_leaf*)n, key, key_len, depth)) {
                return *(uint64_t*)((wort_leaf*)n)->value;
            }
            return NULL;
        }

        // 检查当前节点的深度是否等于目标深度，决定如何处理前缀匹配
        if (n->depth == depth) {
            // Bail if the prefix does not match
            // 如果当前节点有前缀需要匹配
            if (n->pwortial_len) {
                // 检查输入键与节点前缀的匹配程度
                prefix_len = check_prefix(n, key, key_len, depth);
                // 如果前缀不完全匹配（匹配长度不等于最大可能匹配长度），则返回NULL
                if (prefix_len != min(WORT_MAX_PREFIX_LEN, n->pwortial_len)) {
                    return NULL;
                }
                // 更新深度，跳过已匹配的前缀部分
                depth = depth + n->pwortial_len;
            }
        } else {
            // 如果当前节点深度不等于目标深度，需要重构路径前缀
            wort_leaf* leaf[2];
            int cnt, pos, i;

            // 遍历 16 叉节点的所有子节点，找到前两个最小的叶子节点
            for (pos = 0, cnt = 0; pos < 16; pos++) {
                if (((wort_node16*)n)->children[pos]) {
                    // 获取子树中的最小叶子节点
                    leaf[cnt] = minimum(((wort_node16*)n)->children[pos]);
                    cnt++;
                    // 找到两个叶子节点后停止遍历
                    if (cnt == 2)
                        break;
                }
            }

            // 计算两个叶子节点之间的最长公共前缀
            int prefix_diff = longest_common_prefix(leaf[0], leaf[1], depth);
            wort_node old_path;
            old_path.pwortial_len = prefix_diff;
            // 复制公共前缀字符到临时节点
            for (i = 0; i < min(WORT_MAX_PREFIX_LEN, prefix_diff); i++)
                old_path.pwortial[i] = get_index(leaf[1]->key, depth + i);

            // 检查输入键与重构前缀的匹配程度
            prefix_len = check_prefix(&old_path, key, key_len, depth);
            if (prefix_len != min(WORT_MAX_PREFIX_LEN, old_path.pwortial_len)) {
                return NULL;
            }
            depth = depth + old_path.pwortial_len;
        }

        // 递归搜索：根据当前深度对应的键字符找到下一个子节点
        // Recursively search
        child = find_child(n, get_index(key, depth));
        n = (child) ? *child : NULL;
        // 结束计时并输出结果
        depth++;
    }
    return NULL;
}

static wort_leaf* make_leaf(const unsigned long key, int key_len, void* value, bool flush) {
    // wort_leaf *l = (wort_leaf*)malloc(sizeof(wort_leaf));
    wort_leaf* l;
    void* ret;
    ret = concurrency_fast_alloc(sizeof(wort_leaf));
    wort_memory_usage += sizeof(wort_leaf);
    //    posix_memalign(&ret, 64, sizeof(wort_leaf));
    l = static_cast<wort_leaf*>(ret);
    l->value = value;
    l->key_len = key_len;
    l->key = key;

    if (flush == true)
        flush_buffer(l, sizeof(wort_leaf), true);
    return l;
}

static void add_child(wort_node16* n, wort_node** ref, unsigned char c, void* child) {
    (void)ref;
    n->children[c] = (wort_node*)child;
}

/**
 * Calculates the index at which the prefixes mismatch
 */
static int prefix_mismatch(const wort_node* n, const unsigned long key, int key_len, int depth, wort_leaf** l) {
    int max_cmp = min(min(WORT_MAX_PREFIX_LEN, n->pwortial_len), WORT_MAX_HEIGHT - depth);
    int idx;
    for (idx = 0; idx < max_cmp; idx++) {
        if (n->pwortial[idx] != get_index(key, depth + idx))
            return idx;
    }

    // If the prefix is short we can avoid finding a leaf
    if (n->pwortial_len > WORT_MAX_PREFIX_LEN) {
        // Prefix is longer than what we've checked, find a leaf
        *l = minimum(n);
        max_cmp = WORT_MAX_HEIGHT - depth;
        for (; idx < max_cmp; idx++) {
            if (get_index((*l)->key, idx + depth) != get_index(key, depth + idx))
                return idx;
        }
    }
    return idx;
}

void recovery_prefix(wort_node* n, int depth) {
    wort_leaf* leaf[2];
    int cnt, pos, i, j;

    for (pos = 0, cnt = 0; pos < 16; pos++) {
        if (((wort_node16*)n)->children[pos]) {
            leaf[cnt] = minimum(((wort_node16*)n)->children[pos]);
            cnt++;
            if (cnt == 2)
                break;
        }
    }

    int prefix_diff = longest_common_prefix(leaf[0], leaf[1], depth);
    wort_node old_path;
    old_path.pwortial_len = prefix_diff;
    for (i = 0; i < min(WORT_MAX_PREFIX_LEN, prefix_diff); i++)
        old_path.pwortial[i] = get_index(leaf[1]->key, depth + i);
    old_path.depth = depth;
    *((uint64_t*)n) = *((uint64_t*)&old_path);
    flush_buffer(n, sizeof(wort_node), true);
}

static void* recursive_insert(wort_node* n, wort_node** ref, const unsigned long key,
                              int key_len, void* value, int depth, int* old) {
    // 如果当前节点为空(NULL)，则创建一个新的叶子节点
    // If we are at a NULL node, inject a leaf
    if (!n) {
        // 创建新的叶子节点并设置到引用位置
        *ref = (wort_node*)SET_LEAF(make_leaf(key, key_len, value, true));
        // 刷新缓冲区以确保数据持久化到非易失性内存
        flush_buffer(ref, sizeof(uintptr_t), true);
        return NULL;
    }

    // 如果当前节点是一个叶子节点，则需要将其替换为内部节点
    // If we are at a leaf, we need to replace it with a node
    if (IS_LEAF(n)) {
        wort_leaf* l = LEAF_RAW(n);

        // 检查是否是在更新现有值（键相同的情况）
        // Check if we are updating an existing value
        if (!leaf_matches(l, key, key_len, depth)) {
            *old = 1;
            void* old_val = l->value;
            l->value = value;
            flush_buffer(&l->value, sizeof(uintptr_t), true);
            return old_val;
        }

        // 如果是新值，需要将叶子节点分割成一个内部节点
        // New value, we must split the leaf into a node4
        wort_node16* new_node = (wort_node16*)alloc_node();
        new_node->n.depth = depth;

        // 创建新叶子节点
        // Create a new leaf
        wort_leaf* l2 = make_leaf(key, key_len, value, false);

        // Determine longest prefix
        int i, longest_prefix = longest_common_prefix(l, l2, depth);
        new_node->n.pwortial_len = longest_prefix;
        for (i = 0; i < min(WORT_MAX_PREFIX_LEN, longest_prefix); i++)
            new_node->n.pwortial[i] = get_index(key, depth + i);

        // Add the leafs to the new node4
        add_child(new_node, ref, get_index(l->key, depth + longest_prefix), SET_LEAF(l));
        add_child(new_node, ref, get_index(l2->key, depth + longest_prefix), SET_LEAF(l2));

        mfence();
        flush_buffer(new_node, sizeof(wort_node16), false);
        flush_buffer(l2, sizeof(wort_leaf), false);
        mfence();

        *ref = (wort_node*)new_node;
        flush_buffer(ref, 8, true);

        return NULL;
    }

    // 如果当前节点的深度与期望深度不匹配，需要恢复前缀
    if (n->depth != depth) {
        recovery_prefix(n, depth);
    }

    // 检查给定节点是否具有前缀
    // Check if given node has a prefix
    if (n->pwortial_len) {
        // 确定前缀是否不同，因为需要进行分裂
        // Determine if the prefixes differ, since we need to split
        wort_leaf* l = NULL;
        int prefix_diff = prefix_mismatch(n, key, key_len, depth, &l);

        // 如果前缀差异大于等于节点的前缀长度，说明待插入键的前缀完全匹配当前节点
        if ((uint32_t)prefix_diff >= n->pwortial_len) {
            depth += n->pwortial_len;
            goto RECURSE_SEARCH;
        }

        // Create a new node
        wort_node16* new_node = (wort_node16*)alloc_node();
        new_node->n.depth = depth;
        new_node->n.pwortial_len = prefix_diff;
        memcpy(new_node->n.pwortial, n->pwortial, min(WORT_MAX_PREFIX_LEN, prefix_diff));

        // 调整原节点的前缀
        // Adjust the prefix of the old node
        wort_node temp_path;
        if (n->pwortial_len <= WORT_MAX_PREFIX_LEN) {
            // 如果原节点前缀长度在限制范围内，直接添加子节点
            add_child(new_node, ref, n->pwortial[prefix_diff], n);
            temp_path.pwortial_len = n->pwortial_len - (prefix_diff + 1);
            temp_path.depth = (depth + prefix_diff + 1);
            memcpy(temp_path.pwortial, n->pwortial + prefix_diff + 1,
                   min(WORT_MAX_PREFIX_LEN, temp_path.pwortial_len));
        } else {
            // 如果原节点前缀长度超过限制，需要重构路径
            int i;
            if (l == NULL)
                l = minimum(n);
            add_child(new_node, ref, get_index(l->key, depth + prefix_diff), n);
            temp_path.pwortial_len = n->pwortial_len - (prefix_diff + 1);
            for (i = 0; i < min(WORT_MAX_PREFIX_LEN, temp_path.pwortial_len); i++)
                temp_path.pwortial[i] = get_index(l->key, depth + prefix_diff + 1 + i);
            temp_path.depth = (depth + prefix_diff + 1);
        }

        // Insert the new leaf
        l = make_leaf(key, key_len, value, false);
        add_child(new_node, ref, get_index(key, depth + prefix_diff), SET_LEAF(l));

        mfence();
        flush_buffer(new_node, sizeof(wort_node16), false);
        flush_buffer(l, sizeof(wort_leaf), false);
        mfence();

        *ref = (wort_node*)new_node;
        *((uint64_t*)n) = *((uint64_t*)&temp_path);

        mfence();
        flush_buffer(n, sizeof(wort_node), false);
        flush_buffer(ref, sizeof(uintptr_t), false);
        mfence();

        return NULL;
    }

RECURSE_SEARCH:;

    // 查找要递归到的子节点
    // 根据当前深度对应的键字符获取子节点指针
    // Find a child to recurse to
    wort_node** child = find_child(n, get_index(key, depth));
    if (child) {
        return recursive_insert(*child, child, key, key_len, value, depth + 1, old);
    }

    // 如果没有找到对应的子节点，则将新叶子节点添加到当前节点
    // 创建新的叶子节点
    // No child, node goes within us
    wort_leaf* l = make_leaf(key, key_len, value, true);

    add_child((wort_node16*)n, ref, get_index(key, depth), SET_LEAF(l));
    flush_buffer(&((wort_node16*)n)->children[get_index(key, depth)], sizeof(uintptr_t), true);

    return NULL;
}

/**
 * WORT树插入函数 - 将键值对插入到WORT（Write-Optimized Radix Tree）中
 *
 * @param t WORT树指针
 * @param key 要插入的键（无符号长整型）
 * @param key_len 键的长度（字节数）
 * @param value 要插入的值指针
 * @param value_len 值的长度（字节数）
 * @return void* 返回被替换的旧值指针（如果键已存在），否则返回NULL
 */
void* wort_put(wort_tree* t, const unsigned long key, int key_len, void* value, int value_len) {
    int old_val = 0; // 标志位：0表示新插入，1表示更新已有键

    // 使用快速内存分配器为值分配空间
    void* value_allocated = concurrency_fast_alloc(value_len);

    // 更新内存使用统计
    wort_memory_usage += value_len;

    // 将传入的值复制到新分配的内存中
    memcpy(value_allocated, value, value_len);

    // 持久化新分配的值到NVM（非易失性内存）
    flush_buffer(value_allocated, value_len, true);

    // 递归插入键值对到WORT树中
    // 参数说明：
    // - t->root: 当前处理的节点（从根节点开始）
    // - &t->root: 根节点的指针地址（用于可能修改根节点的情况）
    // - key: 要插入的键
    // - key_len: 键的长度
    // - value_allocated: 已分配并持久化的值指针
    // - 0: 当前处理的键位位置（从0开始）
    // - &old_val: 标志位指针，用于返回插入类型
    void* old = recursive_insert(t->root, &t->root, key, key_len, value_allocated, 0, &old_val);

    // 如果是新插入（不是更新已有键），则增加树的大小计数
    if (!old_val)
        t->size++;

    // 返回被替换的旧值（如果键已存在），否则返回NULL
    return old;
}

void wort_all_subtree_kv(wort_node* n, vector<wort_key_value>& res) {
    if (n == NULL)
        return;
    wort_node* tmp = n;
    wort_node** child;
    if (IS_LEAF(tmp)) {
        tmp = (wort_node*)LEAF_RAW(tmp);
        wort_key_value tmp_kv;
        tmp_kv.key = ((wort_leaf*)tmp)->key;
        tmp_kv.value = *(uint64_t*)(((wort_leaf*)tmp)->value);
        res.push_back(tmp_kv);

    } else {
        // Recursively search
        for (int i = 0; i < 16; ++i) {
            child = find_child(tmp, i);
            wort_node* next = (child) ? *child : NULL;
            wort_all_subtree_kv(next, res);
        }
    }
}

void wort_node_scan(wort_node* n, uint64_t left, uint64_t right, uint64_t depth, vector<wort_key_value>& res, int key_len) {
    // depth first search
    if (n == NULL)
        return;
    wort_node* tmp = n;
    wort_node** child;
    if (IS_LEAF(tmp)) {
        // auto start_time = std::chrono::high_resolution_clock::now();
        tmp = (wort_node*)LEAF_RAW(tmp);
        // Check if the expanded path matches
        uint64_t tmp_key = ((wort_leaf*)tmp)->key;
        if (tmp_key >= left && tmp_key <= right) {
            wort_key_value tmp_kv;
            tmp_kv.key = tmp_key;
            tmp_kv.value = *(uint64_t*)(((wort_leaf*)tmp)->value);
            res.push_back(tmp_kv);
        }
        // auto end_time = std::chrono::high_resolution_clock::now();
        // wort_range_segment_leaf_latency_.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count());

    } else {
        // 检查当前节点是否有前缀需要匹配
        if (tmp->pwortial_len) {
            int max_cmp = min(min(tmp->pwortial_len, WORT_MAX_PREFIX_LEN), WORT_MAX_HEIGHT - depth);
            // 检查当前节点的前缀是否与范围的左边界匹配
            for (int idx = 0; idx < max_cmp; idx++) {
                if (tmp->pwortial[idx] > get_index(left, depth + idx)) {
                    break;
                } else if (tmp->pwortial[idx] < get_index(left, depth + idx)) {
                    return;
                }
            }
            // 检查当前节点的前缀是否与范围的右边界匹配
            for (int idx = 0; idx < max_cmp; idx++) {
                if (tmp->pwortial[idx] < get_index(right, depth + idx)) {
                    break;
                } else if (tmp->pwortial[idx] > get_index(left, depth + idx)) {
                    return;
                }
            }
            depth = depth + tmp->pwortial_len;
        }

        // 递归搜索：处理跨越当前节点子节点的范围查询
        // 获取左边界和右边界在当前深度对应的键索引
        // Recursively search
        unsigned char left_index = get_index(left, depth);
        unsigned char right_index = get_index(right, depth);

        if (left_index != right_index) {
            // 如果左右边界的索引不同，说明范围跨越了多个子节点
            // 处理左边界所在的子树：从左边界到最大值
            child = find_child(tmp, left_index);
            wort_node* next = (child) ? *child : NULL;
            wort_node_scan(next, left, 0xffffffffffffffff, depth + 1, res);
            // 处理右边界所在的子树：从最小值到右边界
            child = find_child(tmp, right_index);
            next = (child) ? *child : NULL;
            wort_node_scan(next, 0, right, depth + 1, res);
        } else {
            // 如果左右边界的索引相同，说明整个范围都在同一个子树中
            child = find_child(tmp, left_index);
            wort_node* next = (child) ? *child : NULL;
            wort_node_scan(next, left, right, depth + 1, res);
        }

        // uint64_t temp_total_time = 0;
        // auto start_time = std::chrono::high_resolution_clock::now();
        // 处理左右边界之间所有的中间子节点（这些子节点完全在查询范围内）
        for (int i = left_index + 1; i < right_index; ++i) {
            // 查找每个中间索引对应的子节点
            child = find_child(tmp, i);
            wort_node* next = (child) ? *child : NULL;
            // 将该子树中的所有键值对添加到结果中（因为整个子树都在查询范围内）
            // auto temp_start_time = std::chrono::high_resolution_clock::now();
            wort_all_subtree_kv(next, res);
            // auto temp_end_time = std::chrono::high_resolution_clock::now();
            // temp_total_time += std::chrono::duration_cast<std::chrono::nanoseconds>(temp_end_time - temp_start_time).count();
        }
        // auto end_time = std::chrono::high_resolution_clock::now();
        // wort_range_segment_scan_latency_.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count() - temp_total_time);
    }
}

vector<wort_key_value> wort_scan(const wort_tree* t, uint64_t left, uint64_t right, int key_len) {
    vector<wort_key_value> res;
    wort_node_scan(t->root, left, right, 0, res);
    return res;
}

uint64_t wort_memory_profile(wort_node* n) {
    return wort_memory_usage;
    //    if (n == NULL) {
    //        return 0;
    //    }
    //    uint64_t res = 0;
    //    wort_node *tmp = n;
    //    wort_node **child;
    //    if (IS_LEAF(tmp)) {
    //        res += sizeof(wort_leaf);
    //    } else {
    //        // Recursively search
    //        res += sizeof(wort_node16);
    //        for (int i = 0; i < 16; ++i) {
    //            child = find_child(tmp, i);
    //            wort_node *next = (child) ? *child : NULL;
    //            res += wort_memory_profile(next);
    //        }
    //    }
    //    return res;
}