#ifndef NVMKV_ERT_INT_H
#define NVMKV_ERT_INT_H

#include "ERT_node_int.h"

// 大小: 16字节; 内存布局:
// ┌─────────────────────────────────────────────────┐
// │ init_depth (4字节) + 填充 (4字节)                │
// ├─────────────────────────────────────────────────┤
// │ root指针 (指向ERTIntNode) (8字节)                │
// └─────────────────────────────────────────────────┘
class ERTInt {
public:
    int init_depth = 0; // represent extendible hash initial global depth
    ERTIntNode* root = NULL;

    // 统计查询时访问每个节点的延迟（ns）、访问节点的个数
    std::vector<uint64_t> per_search_per_node_latency_ = {}; // 每次搜索时访问每个节点的延迟

    // 统计范围查询时树遍历延迟（ns）、段访问个数、每个段扫描延迟（ns）
    std::vector<uint64_t> range_segment_scan_latency_ = {}; // 范围查询每个段扫描延迟

    // 统计每次插入时树遍历延迟（ns）、节点分裂+节点扩容的延迟（ns）或者是叫节点重新调整延迟（ns）、节点更新的延迟（ns）
    std::vector<uint64_t> per_insert_node_split_latency_ = {};  // 每次插入时更新节点的延迟
    std::vector<uint64_t> per_insert_node_grow_latency_ = {};   // 每次插入时扩展节点的延迟
    std::vector<uint64_t> per_insert_node_update_latency_ = {}; // 每次插入时更新节点的延迟

    std::vector<uint64_t> per_insert_node_traversal_latency_ = {}; // 每次插入时遍历树的延迟

public:
    ERTInt();

    ERTInt(int _span, int _init_depth);

    ~ERTInt();

    void init();

    // support variable values, for convenience, we set v to 8 byte
    void Insert(uint64_t key, uint64_t value, ERTIntNode* _node = NULL, int len = 0);

    uint64_t Search(uint64_t key);

    vector<ERTIntKeyValue> scan(uint64_t left, uint64_t right);

    void nodeScan(ERTIntNode* tmp, uint64_t left, uint64_t right, vector<ERTIntKeyValue>& res, int pos = 0,
                  uint64_t prefix = 0);

    void getAllNodes(ERTIntNode* tmp, vector<ERTIntKeyValue>& res, int pos = 0, uint64_t prefix = 0);

    uint64_t memory_profile(ERTIntNode* tmp, int pos = 0);
};

ERTInt* NewExtendibleRadixTreeInt();

#endif // NVMKV_ERT_INT_H
