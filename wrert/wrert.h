#ifndef NVMKV_WRERT_H
#define NVMKV_WRERT_H

#include <cstdint>
#include <vector>
#include <memory>
#include <stdexcept>
#include <fstream>
#include <string>
#include <iomanip>

#include "wrert_node.h"

namespace wrert {

class WRERT {
public:
    char* _base_ptr = nullptr;
    uint32_t globalDepthTree = 0;
    WRERTNode* root = nullptr;

    std::vector<uint64_t> per_search_per_node_latency_ = {};
    std::vector<uint64_t> range_segment_scan_latency_ = {};

    std::vector<uint64_t> per_insert_node_split_latency_ = {};
    std::vector<uint64_t> per_insert_node_grow_latency_ = {};
    std::vector<uint64_t> per_insert_node_update_latency_ = {};
    std::vector<uint64_t> per_insert_node_traversal_latency_ = {};

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
