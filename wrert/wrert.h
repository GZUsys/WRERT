#ifndef NVMKV_WRERT_H
#define NVMKV_WRERT_H

#include <cstdint>
#include <vector>
#include <memory>
#include <stdexcept>
#include <fstream> // 添加文件流支持
#include <string>  // 添加字符串支持
#include <iomanip> // 添加格式化支持

#include "wrert_node.h"

namespace wrert {

class WRERT {
public:
    char* _base_ptr = nullptr;
    uint32_t globalDepthTree = 0;
    WRERTNode* root = nullptr;

    // 统计查询时访问每个节点的延迟（ns）、访问节点的个数
    std::vector<uint64_t> per_search_per_node_latency_ = {}; // 每次搜索时访问每个节点的延迟

    // // 统计范围查询时树遍历延迟（ns）、段访问个数、每个段扫描延迟（ns）
    std::vector<uint64_t> range_segment_scan_latency_ = {}; // 范围查询每个段扫描延迟

    // // 统计每次插入时树遍历延迟（ns）、节点分裂+节点扩容的延迟（ns）或者是叫节点重新调整延迟（ns）、节点更新的延迟（ns）
    std::vector<uint64_t> per_insert_node_split_latency_ = {};  // 每次插入时更新节点的延迟
    std::vector<uint64_t> per_insert_node_grow_latency_ = {};   // 每次插入时扩展节点的延迟
    std::vector<uint64_t> per_insert_node_update_latency_ = {}; // 每次插入时更新节点的延迟

    std::vector<uint64_t> per_insert_node_traversal_latency_ = {}; // 每次插入时遍历树的延迟

public:
    WRERT();

    WRERT(uint32_t globalDepthTree);

    ~WRERT();

    WRERT(const WRERT&) = delete;

    WRERT& operator=(const WRERT&) = delete;

    void initialize();

    OperationResults insert(uint64_t key, uint64_t value, WRERTNode* node = nullptr, int keyLength = 0, WRERTKeyValue* _kv = nullptr);

    OperationResults remove(uint64_t key, WRERTNode* node = nullptr, int keyLength = 0);

    uint64_t search(uint64_t key, WRERTNode* node = nullptr, int keyLength = 0);

    std::vector<WRERTKeyValue> lookupRange(uint64_t leftKey, uint64_t rightKey);

    void nodeScan(WRERTNode* node, uint64_t leftKey, uint64_t rightKey,
                  std::vector<WRERTKeyValue>& results, int currentKeyPosition = 0, uint64_t prefix = 0);

    void scanAllNodes(WRERTNode* node, std::vector<WRERTKeyValue>& results, int currentKeyPosition = 0, uint64_t prefix = 0);

    uint64_t memory_profile(WRERTNode* node, int position = 0);

    WRERT* recoveryTree(int position = 0, int nodeHeaderDepth = 0);

    void recoveryAllNodes(WRERTNode* node, char* new_base_ptr, uintptr_t old_base, int position = 0, int nodeHeaderDepth = 0);
};

WRERT* NewWRERT();

} // namespace wrert

#endif // NVMKV_WRERT_H
