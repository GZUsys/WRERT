#include <fstream>
#include <iomanip>
#include <sstream>
#include <chrono>    // 添加高精度时间测量头文件
#include <algorithm> // 添加sort函数支持
#include <vector>    // 添加vector支持

#include "ERT_int.h"

/*
 *         begin  len
 * key [______|___________|____________]
 */

#define GET_32BITS(pointer, pos) (*((uint32_t*)(pointer + pos)))

#define _32_BITS_OF_BYTES 4

inline void mfence(void) {
    asm volatile("mfence" ::: "memory");
}

inline void clflush(char* data, size_t len) {
    volatile char* ptr = (char*)((unsigned long)data & (~(CACHELINESIZE - 1)));
    mfence();
    for (; ptr < data + len; ptr += CACHELINESIZE) {
        asm volatile("clflush %0" : "+m"(*(volatile char*)ptr));
    }
    mfence();
}

bool isSame(unsigned char* key1, uint64_t key2, int pos, int length) {
    uint64_t subkey = GET_SUBKEY(key2, pos, length);
    subkey <<= (64 - length);
    for (int i = 0; i < length / SIZE_OF_CHAR; i++) {
        if ((subkey >> 56) != (uint64_t)key1[i]) {
            return false;
        }
        subkey <<= SIZE_OF_CHAR;
    }
    return true;
}

void ERTInt::Insert(uint64_t key, uint64_t value, ERTIntNode* _node, int len) {
    // 当前处理的字节位置索引（从0开始）
    ERTIntNode* currentNode = _node;
    if (_node == NULL) {
        currentNode = root; // 如果未指定节点，使用根节点
    }
    unsigned char headerDepth = currentNode->header.depth; // 当前节点的深度
    uint64_t beforeAddress = (uint64_t)&root;              // 记录前驱节点的地址
    uint8_t kvFLAG;                                        // 记录节点标志
    // printf("===========================\n");
    // printf("insert key: 0x%lx, (十进制)%llu, value: %llu, root: %lu, beforeAddress: %lu, len: %d\n",
    //        key, key, value, root, beforeAddress, len);
    // printf("===========================\n");

    uint32_t j = 0;
    while (len < ERT_KEY_LENGTH / SIZE_OF_CHAR) // 循环处理键的每个部分，直到处理完整个键
    {
        int matchedPrefixLen; // 匹配的前缀长度

        // 检查前缀初始化状态
        if (currentNode->header.len > ERT_NODE_PREFIX_MAX_BYTES) // 如果当前节点的前缀长度大于最大值，表示该节点还没有初始化前缀
        {
            // printf("情况0: 当前节点前缀未初始化，正在初始化前缀...\n");
            int size = ERT_NODE_PREFIX_MAX_BITS / ERT_NODE_LENGTH * ERT_NODE_LENGTH;
            // 设置当前节点的前缀长度
            if (len == 0) {
                currentNode->header.len =
                  (ERT_KEY_LENGTH - len * SIZE_OF_CHAR) <= size ? (ERT_KEY_LENGTH - len * SIZE_OF_CHAR) / SIZE_OF_CHAR : size / SIZE_OF_CHAR;
            } else {
                currentNode->header.len = 0;
            }
            // printf("Setting header.len to %d, size: %d, len: %d\n", currentNode->header.len, size, len);
            currentNode->header.assign(key, len); // 分配前缀

            // currentNode->header.depth = currentNode->header.len / (ERT_NODE_LENGTH / SIZE_OF_CHAR); // 设置前缀深度

            // len += size / SIZE_OF_CHAR;           // 移动处理位置

            continue;
        } else {
            // 计算当前键与节点前缀的匹配长度
            matchedPrefixLen = currentNode->header.computePrefix(key, len * SIZE_OF_CHAR);

            // printf("匹配的前缀长度 matchedPrefixLen: %d, 前缀长度 len: %d, 节点头部深度 depth: %d\n",
            //        matchedPrefixLen, currentNode->header.len, currentNode->header.depth);
        }

        if (len + matchedPrefixLen == (ERT_KEY_LENGTH / SIZE_OF_CHAR)) // 情况1: 完全匹配且到达键的末尾 - 直接插入键值对
        {
            ERTIntKeyValue* kv = NewERTIntKeyValue(key, value); // 创建新的键值对
            clflush((char*)kv, sizeof(ERTIntKeyValue));         // 持久化到NVM
            currentNode->nodePut(matchedPrefixLen, kv);         // 插入到当前节点
            // printf("情况1: 完全匹配且到达键的末尾 - 直接插入键值对\n");
            return;
        }

        // if (matchedPrefixLen == currentNode->header.len) // 情况2: 前缀完全匹配 - 继续向下层处理
        if ((matchedPrefixLen / (ERT_NODE_LENGTH / SIZE_OF_CHAR))
            == (currentNode->header.len / (ERT_NODE_LENGTH / SIZE_OF_CHAR))) // 情况2: 前缀完全匹配 - 继续向下层处理
        {
            // printf("情况2: 前缀完全匹配 - 继续向下层处理\n");
            // 前缀匹配成功，移动处理位置
            // len += currentNode->header.len;
            len += currentNode->header.len % (ERT_NODE_LENGTH / SIZE_OF_CHAR) == 0 ? currentNode->header.len : currentNode->header.len / (ERT_NODE_LENGTH / SIZE_OF_CHAR);

            // printf("Current len: %d\n", len);
            // printf("Prefix completely matched. Moving to next part of the key. New len: %d\n", len);
            // 计算子键（从当前处理位置提取指定长度的位）
            uint64_t subkey = GET_SUBKEY(key, len * SIZE_OF_CHAR, ERT_NODE_LENGTH);
            // 在可扩展哈希节点中搜索子键
            uint64_t next = 0;

            // 计算目录索引和段索引
            // GET_SEG_NUM(514, 32, 0) = (514 >> (32-0)) & ((1<<0)-1) = (514 >> 32) & 0 = 0 & 0 = 0
            uint64_t dir_index = GET_SEG_NUM(subkey, ERT_NODE_LENGTH, currentNode->global_depth);

            ERTIntSegment* tmp_seg = *(ERTIntSegment**)GET_SEG_POS(currentNode, dir_index);
            // GET_BUCKET_NUM(514, 8) = 514 & ((1<<8)-1) = 514 & 0xFF = 2
            uint64_t seg_index = GET_BUCKET_NUM(subkey, ERT_BUCKET_MASK_LEN);
            ERTIntBucket* tmp_bucket = &(tmp_seg->bucket[seg_index]);
            // printf("subkey: 0x%08llx, 目录索引: %llu, 桶索引: %llu, 全局深度: %d\n", subkey, dir_index, seg_index, currentNode->global_depth);

            int i;
            bool keyValueFlag = false; // 标记找到的是键值对还是节点指针
            uint64_t beforeA;

            // auto start_time = std::chrono::high_resolution_clock::now();
            // 在桶中搜索子键
            for (i = 0; i < ERT_BUCKET_SIZE; ++i) {
                // printf("tmp_bucket->counter[%d]=%llu, REMOVE_NODE_FLAG=%llu\n", i, tmp_bucket->counter[i].subkey, REMOVE_NODE_FLAG(tmp_bucket->counter[i].subkey));
                if (subkey == REMOVE_NODE_FLAG(tmp_bucket->counter[i].subkey)) {
                    next = tmp_bucket->counter[i].value; // 找到对应的值或指针
                    // printf("Found subkey in bucket at index %d: next=%llu\n", i, next);
                    keyValueFlag = GET_NODE_FLAG(tmp_bucket->counter[i].subkey); // 获取节点标志
                    // printf("keyValueFlag: %d\n", keyValueFlag);
                    beforeA = (uint64_t)&tmp_bucket->counter[i].value; // 记录前驱地址
                    break;
                }
            }
            // auto end_time = std::chrono::high_resolution_clock::now();
            // per_insert_node_traversal_latency_.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count());

            len += ERT_NODE_LENGTH / SIZE_OF_CHAR; // 移动处理位置

            // printf("Current len%d: %d\n", j++, len);
            if (len == 8) // 情况2.1: 到达键的末尾
            {
                if (next == 0) // 子键不存在，直接插入
                {
                    // printf("情况2.1.1: 到达键的末尾 - 子键不存在 - 直接插入\n");
                    currentNode->put(subkey, (uint64_t)value, tmp_seg, tmp_bucket, dir_index, seg_index,
                                     kvFLAG = 1, beforeAddress); // 插入键值对
                    // visualizeMemoryLayout("Insert", key, value);
                    return;
                } else // 子键已存在，更新值
                {
                    // printf("情况2.1.2: 到达键的末尾 - 子键已存在 - 更新值\n");
                    tmp_bucket->counter[i].value = value;
                    clflush((char*)&tmp_bucket->counter[i].value, 8); // 持久化更新值
                    return;
                }
            } else // 情况2.2: 未到达键的末尾
            {
                if (next == 0) // 子键不存在，创建新的键值对
                {
                    // printf("情况2.2.1: 未到达键的末尾 - 子键不存在 - 创建新的键值对\n");
                    ERTIntKeyValue* kv = NewERTIntKeyValue(key, value);
                    // printf("New key: %llu, 0x%llx, value: %llu\n", kv->key, kv->key, kv->value);
                    clflush((char*)kv, sizeof(ERTIntKeyValue));
                    currentNode->put(subkey, (uint64_t)kv, tmp_seg, tmp_bucket, dir_index, seg_index, kvFLAG = 1, beforeAddress); // 插入键值对
                    // visualizeMemoryLayout("Insert", key, value);
                    return;
                } else {
                    if (keyValueFlag) // 找到的是键值对，发生冲突
                    {
                        uint64_t prekey = ((ERTIntKeyValue*)next)->key;     // 已存在的键
                        uint64_t prevalue = ((ERTIntKeyValue*)next)->value; // 已存在的值

                        if (unlikely(key == prekey)) // 键相同，更新值
                        {
                            // printf("情况2.2.2: 未到达键的末尾 - 子键存在 - 找到的是键值对 - 键相同 - 更新键值对\n");
                            ((ERTIntKeyValue*)next)->value = value;
                            clflush((char*)&(((ERTIntKeyValue*)next)->value), 8); // 持久化更新值

                            return;
                        } else // 键不同，需要创建新节点处理冲突
                        {
                            // printf("情况2.2.3: 未到达键的末尾 - 子键存在 - 找到的是键值对 - 键不同 - 创建新节点处理冲突\n");

                            // 创建新的ERT节点
                            ERTIntNode* newNode = NewERTIntNode(ERT_NODE_LENGTH, headerDepth + 1);

                            // printf("newNode header: len = %d, depth = %d\n",
                            //        newNode->header.len, newNode->header.depth);
                            // for (int i = 0; i < 6; i++) {
                            //     printf("array[%d] = %02x\n", i, newNode->header.array[i]);
                            // }

                            // 插入已存在的键值对到新节点
                            Insert(prekey, prevalue, newNode, len);

                            // 插入新的键值对到新节点
                            Insert(key, value, newNode, len);

                            clflush((char*)newNode, sizeof(ERTIntNode)); // 持久化新节点

                            // 更新桶中的指针指向新节点
                            tmp_bucket->counter[i].subkey = REMOVE_NODE_FLAG(tmp_bucket->counter[i].subkey);
                            tmp_bucket->counter[i].value = (uint64_t)newNode;
                            clflush((char*)&tmp_bucket->counter[i].value, 8); // 持久化更新值

                            // printf("情况2.2.3: keyValueFlag: %llu\n", GET_NODE_FLAG(tmp_bucket->counter[i].subkey));

                            return;
                        }
                    } else // 找到的是节点指针，继续向下层处理
                    {
                        // printf("情况2.2.4: 未到达键的末尾 - 子键存在 - 找到的是节点指针，继续向下层处理\n");
                        currentNode = (ERTIntNode*)next;         // 移动到下一层节点
                        beforeAddress = beforeA;                 // 更新前驱地址
                        headerDepth = currentNode->header.depth; // 更新深度
                    }
                }
            }
        } else // 情况3: 前缀不匹配（匹配长度较短） - 需要分裂节点
        {
            // printf("情况3: 前缀不匹配，需要节点路径压缩的分裂\n");

            // 创建新的树节点来处理前缀不匹配的情况
            ERTIntNode* newNode = NewERTIntNode(ERT_NODE_LENGTH, headerDepth);

            // 初始化新节点的前缀（基于匹配的部分）
            newNode->header.init(&currentNode->header, matchedPrefixLen, currentNode->header.depth);

            // 复制当前节点的键值对到新节点（从匹配位置开始）
            for (int j = currentNode->header.len - matchedPrefixLen; j <= currentNode->header.len; j++) {
                newNode->treeNodeValues[j - currentNode->header.len + matchedPrefixLen] = currentNode->treeNodeValues[j];
            }

            // 计算新键的子键
            // uint64_t subkey = GET_SUBKEY(key, (len + matchedPrefixLen) * SIZE_OF_CHAR, ERT_NODE_LENGTH);
            uint64_t subkey = GET_SUBKEY(key, (len + matchedPrefixLen % (ERT_NODE_LENGTH / SIZE_OF_CHAR) != 0 ? 0 : matchedPrefixLen), ERT_NODE_LENGTH);
            // printf("len: %d, matchedPrefixLen: %d\n", len, matchedPrefixLen);

            // 创建新的键值对
            ERTIntKeyValue* kv = NewERTIntKeyValue(key, value);
            clflush((char*)kv, sizeof(ERTIntKeyValue));

            // 将新键值对插入新节点
            newNode->put(subkey, (uint64_t)kv, kvFLAG = 1, (uint64_t)&newNode);

            // 将当前节点插入新节点（作为子节点）
            newNode->put(GET_32BITS(currentNode->header.array, matchedPrefixLen), (uint64_t)currentNode, kvFLAG = 0, (uint64_t)&newNode);

            // 修改当前节点的前缀（移除已匹配的部分）
            // currentNode->header.depth -= matchedPrefixLen * SIZE_OF_CHAR / ERT_NODE_LENGTH;
            currentNode->header.depth =
              newNode->header.depth + newNode->header.len / (ERT_NODE_LENGTH / SIZE_OF_CHAR) + 1; // 更新深度
            // currentNode->header.len -= matchedPrefixLen;
            currentNode->header.len -= (matchedPrefixLen + newNode->header.depth + 1);
            for (int i = 0; i < ERT_NODE_PREFIX_MAX_BYTES - matchedPrefixLen; i++) {
                currentNode->header.array[i] = currentNode->header.array[i + matchedPrefixLen];
            }

            // 分别打印新节点和当前节点的所有头部信息
            // printf("newNode header: len = %d, depth = %d\n",
            //        newNode->header.len, newNode->header.depth);
            // for (int i = 0; i < 6; i++) {
            //     printf("array[%d] = %02x\n", i, newNode->header.array[i]);
            // }

            // printf("currentNode header: len = %d, depth = %d\n",
            //        currentNode->header.len, currentNode->header.depth);
            // for (int i = 0; i < 6; i++) {
            //     printf("array[%d] = %02x\n", i, currentNode->header.array[i]);
            // }

            // 持久化修改
            clflush((char*)&(currentNode->header), 8);
            clflush((char*)newNode, sizeof(ERTIntNode));

            // 更新前驱节点的指针指向新节点
            if (newNode->header.depth != 0) {
                ((ERTIntBucketKeyValue*)beforeAddress)->value = (uint64_t)newNode;
                clflush((char*)(ERTIntBucketKeyValue*)beforeAddress, sizeof(ERTIntBucketKeyValue));
                // printf("前驱地址: %lu, 新节点地址: %lu, slot->subKeyFields.subKey: 0x%08llx, value: %lu\n",
                //        beforeAddress, newNode, ((ERTIntBucketKeyValue*)beforeAddress)->subKeyFields.subKey, ((ERTIntBucketKeyValue*)beforeAddress)->valueFields.value);
            } else {
                *(ERTIntNode**)beforeAddress = newNode;
                clflush((char*)beforeAddress, sizeof(ERTIntNode*));
                // printf("前驱地址: %lu, 新节点地址: %lu\n", beforeAddress, newNode);
            }

            // printf("Search key: %lu, Search result: %llu\n", 1, Search(1));
            // printf("Search key: %lu, Search result: %llu\n", key, Search(key));

            // cout << "情况3: 前缀不匹配 - 需要分裂节点 - 创建新节点处理冲突" << endl;
            return;
        }
    }
}

/**
 * @brief ERT搜索函数 - 在可扩展基数树中查找指定键对应的值
 *
 * 功能：从ERT根节点开始，通过前缀匹配和子键索引逐层查找，最终返回指定键对应的值
 * 该方法实现了ERT的完整搜索路径：前缀匹配 → 子键提取 → 段索引 → 桶查找 → 值返回
 *
 * @param key 要查找的64位键值
 * @return uint64_t 返回键对应的值，如果键不存在则返回0
 *
 * @note 搜索算法流程：
 * 1. 从根节点开始，逐字节处理64位键
 * 2. 前缀匹配：比较节点前缀与键的当前部分
 * 3. 子键提取：提取剩余32位子键用于段索引
 * 4. 段查找：使用子键和全局深度计算段索引
 * 5. 递归/终止：根据返回类型决定继续递归或返回结果
 */
uint64_t ERTInt::Search(uint64_t key) {
    auto currentNode = root; // 当前处理的节点，初始为根节点
    if (currentNode == NULL) // 安全检查：如果根节点为空，直接返回0
    {
        return 0;
    }
    int pos = 0; // 当前处理的键位置（字节偏移量，从最高位开始）

    // printf("=======================================\n");
    // printf("search key: 0x%lx, (十进制)%llu, root: %lu\n", key, key, root);
    // printf("=======================================\n");

    // 主循环：逐字节处理64位键（64位/8位每字节=8字节）
    while (pos < ERT_KEY_LENGTH / SIZE_OF_CHAR) {
        // 情况1：当前节点有前缀需要匹配
        // if (currentNode->header.len) {
        if (currentNode->header.len / (ERT_NODE_LENGTH / SIZE_OF_CHAR)) {
            // 子情况1.1：剩余键长度 <= 节点前缀长度（到达键末尾）
            if (ERT_KEY_LENGTH / SIZE_OF_CHAR - pos <= currentNode->header.len) {
                // 计算节点值数组中的索引：header.len - (总字节数 - 当前位置)
                int index = currentNode->header.len - ERT_KEY_LENGTH + pos;

                // 检查键是否匹配：比较节点值数组中的键与搜索键
                if (currentNode->treeNodeValues[index].key == key) {
                    // 键匹配成功，返回对应的值
                    return (uint64_t)currentNode->treeNodeValues[index].value;
                } else {
                    // 键不匹配，搜索失败
                    return 0;
                }
            }

            // 子情况1.2：检查前缀是否匹配
            // 比较节点前缀数组与键的当前部分（从pos位置开始，长度为header.len字节）
            if (!isSame((unsigned char*)currentNode->header.array, key, pos * SIZE_OF_CHAR,
                        currentNode->header.len * SIZE_OF_CHAR)) {
                // 前缀不匹配，搜索失败
                return 0;
            }

            // 前缀匹配成功，移动处理位置：跳过已匹配的前缀长度
            pos += currentNode->header.len;
        }

        // 情况2：提取子键用于段索引
        // 从键的当前位置提取32位子键（ERT_NODE_LENGTH=32）
        // uint64_t subkey = GET_16_BITS(key,pos); // 注释掉的旧代码（16位子键）
        uint64_t subkey = GET_SUBKEY(key, pos * SIZE_OF_CHAR, ERT_NODE_LENGTH);
        // printf("提取子键: 0x%08llx\n", subkey);
        bool keyValueFlag = false; // 标志位：false表示找到节点指针，true表示找到键值对

        // auto start_time = std::chrono::high_resolution_clock::now();
        // 在当前节点中根据子键查找下一跳
        auto next = currentNode->get(subkey, keyValueFlag);
        // 结束计时并输出结果
        // auto end_time = std::chrono::high_resolution_clock::now();
        // per_search_per_node_latency_.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count());

        // 移动处理位置：跳过已处理的子键长度（32位=4字节）
        // pos+=_16_BITS_OF_BYTES; // 注释掉的旧代码（16位=2字节）
        pos += _32_BITS_OF_BYTES; // 32位=4字节

        // 安全检查：如果下一跳为空，搜索失败
        if (next == -1) {
            return -1;
        }

        // printf("next: %lu, keyValueFlag: %d\n", next, keyValueFlag);

        // 情况3：根据返回类型决定下一步操作
        if (keyValueFlag) {
            // 子情况3.1：找到的是键值对（叶子节点）
            if (pos == 8) // 检查是否已处理完整个键（8字节=64位）
            {
                // printf("找到子键，且到达键的末尾，说明是叶子节点，直接返回值\n");

                // 已处理完整个键，直接返回找到的值
                return next;
            } else {
                // 未处理完整个键，需要验证键的完整性
                if (key == ((ERTIntKeyValue*)next)->key) {
                    // printf("找到子键，且未到达键的末尾，说明是内部节点，将值转换为指向实际键值的指针，再提取值\n");

                    // 键完整匹配，返回对应的值
                    return ((ERTIntKeyValue*)next)->value;
                } else {
                    // 键不完整匹配，搜索失败
                    return 0;
                }
            }
        } else {
            // printf("找到子键，且存储的是指向下一个节点的指针\n");

            // 子情况3.2：找到的是节点指针，继续向下层递归搜索
            currentNode = (ERTIntNode*)next;
        }
    }

    // 循环结束仍未找到匹配，返回0表示搜索失败
    return 0;
}

vector<ERTIntKeyValue> ERTInt::scan(uint64_t left, uint64_t right) {
    vector<ERTIntKeyValue> results;

    // printf("=======================================\n");
    // printf("scan: leftKey = %llu, 0x%016llx, rightKey = %llu, 0x%016llx\n", left, left, right, right);
    // printf("=======================================\n");

    nodeScan(root, left, right, results, 0);

    return results;
}

/**
 * @brief 递归扫描节点范围内的键值对（范围查询核心实现）
 *
 * 功能：在指定ERT节点及其子树中执行范围查询，收集所有键在[left, right]范围内的键值对
 * 该方法利用ERT的前缀匹配和段桶系统特性，通过递归遍历实现高效的范围扫描
 *
 * @param tmp 当前处理的ERT节点（可为NULL，表示从根节点开始）
 * @param left 范围查询的左边界键（包含）
 * @param right 范围查询的右边界键（包含）
 * @param res 结果向量引用，用于收集匹配的键值对
 * @param pos 当前处理的键位置（位偏移量，从最高位开始）
 * @param prefix 当前已匹配的前缀值，用于构建完整键
 *
 * @note 算法流程：
 * 1. 边界检查：确定左右边界在当前节点的段位置
 * 2. 前缀处理：更新前缀值和位置偏移
 * 3. 子键计算：提取左右边界的子键
 * 4. 段定位：计算左右边界对应的段索引
 * 5. 范围扫描：遍历目标段范围内的所有桶
 * 6. 递归处理：对子节点继续递归扫描
 *
 * @return void 结果通过res参数返回
 */
void ERTInt::nodeScan(ERTIntNode* tmp, uint64_t left, uint64_t right, vector<ERTIntKeyValue>& res, int pos,
                      uint64_t prefix) {
    // printf("nodeScan: pos=%d, prefix=%llx\n", pos, prefix);
    // 安全检查：如果当前节点为空，默认使用根节点
    if (unlikely(tmp == NULL)) {
        tmp = root;
    }

    // 初始化左右边界在目录中的位置，UINT64_MAX表示未确定
    uint64_t leftPos = UINT64_MAX, rightPos = UINT64_MAX;

    // 左边界前缀匹配检查：确定左边界在当前节点前缀中的位置
    // for (int i = 0; i < tmp->header.len; i++) {
    for (int i = 0; i < tmp->header.len && tmp->header.len % (ERT_NODE_LENGTH / SIZE_OF_CHAR) == 0; i++) {
        // 从左边界键的pos+i位置提取8位子键（1字节）
        uint64_t subkey = GET_SUBKEY(left, pos + i, 8);
        // printf("左边界前缀匹配检查: i = %d, subkey = 0x%02llx, header.len = %lu\n", i, subkey, tmp->header.len);
        // 如果子键与节点前缀的第i个字节匹配，继续检查下一个字节
        if (subkey == (uint64_t)tmp->header.array[i]) {
            continue;
        } else {
            if (subkey > (uint64_t)tmp->header.array[i]) // 子键大于前缀字节：左边界超出当前节点范围，直接返回
            {
                return;
            } else {
                // 子键小于前缀字节：左边界从目录的第一个段开始
                leftPos = 0;
                break;
            }
        }
    }

    // 右边界前缀匹配检查：确定右边界在当前节点前缀中的位置
    // for (int i = 0; i < tmp->header.len; i++) {
    for (int i = 0; i < tmp->header.len && tmp->header.len % (ERT_NODE_LENGTH / SIZE_OF_CHAR) == 0; i++) {
        // 从右边界键的pos+i位置提取8位子键（1字节）
        uint64_t subkey = GET_SUBKEY(right, pos + i, 8);
        // printf("右边界前缀匹配检查: i = %d, subkey = 0x%02llx, header.len = %lu, header.array[%d] = 0x%02llx\n",
        //    i, subkey, tmp->header.len, i, (uint64_t)tmp->header.array[i]);
        // 如果子键与节点前缀的第i个字节匹配，继续检查下一个字节
        if (subkey == (uint64_t)tmp->header.array[i]) {
            continue;
        } else {
            // 子键大于前缀字节：右边界到目录的最后一个段
            if (subkey > (uint64_t)tmp->header.array[i]) {
                rightPos = tmp->dir_size - 1;
                break;
            } else {
                // 子键小于前缀字节：右边界超出当前节点范围，直接返回
                return;
            }
        }
    }

    // 处理当前节点的前缀：更新前缀值和位置偏移
    // if (tmp->header.len > 0) {
    if (tmp->header.len % (ERT_NODE_LENGTH / SIZE_OF_CHAR) == 0) {
        // 将当前前缀左移，为新的前缀字节腾出空间
        prefix = (prefix << tmp->header.len * SIZE_OF_CHAR);

        // 将节点前缀数组的字节按大端序添加到前缀值中
        for (int i = 0; i < tmp->header.len; ++i) {
            prefix += tmp->header.array[i] << (tmp->header.len - i);
        }

        // 更新位置偏移：跳过已处理的前缀长度
        pos += tmp->header.len * SIZE_OF_CHAR;
    }

    // 计算左右边界的子键和段位置（如果尚未确定）
    uint64_t leftSubkey = UINT64_MAX, rightSubkey = UINT64_MAX;

    // 如果左边界位置未确定，计算左子键和段索引
    if (leftPos == UINT64_MAX) {
        leftSubkey = GET_SUBKEY(left, pos, ERT_NODE_LENGTH);
        leftPos = GET_SEG_NUM(leftSubkey, ERT_NODE_LENGTH, tmp->global_depth);
    }

    // 如果右边界位置未确定，计算右子键和段索引
    if (rightPos == UINT64_MAX) {
        rightSubkey = GET_SUBKEY(right, pos, ERT_NODE_LENGTH);
        rightPos = GET_SEG_NUM(rightSubkey, ERT_NODE_LENGTH, tmp->global_depth);
    }

    // 更新前缀：为子键腾出空间
    prefix = (prefix << ERT_NODE_LENGTH);

    // 更新位置：跳过子键长度
    pos += ERT_NODE_LENGTH;

    // printf("prefix: 0x%llx, pos: %d, left: 0x%llx, right: 0x%llx\n", prefix, pos, left, right);

    // 情况1：左右边界子键相同，只需处理单个段
    if (leftSubkey == rightSubkey) {
        // printf("情况3.1：如果左右段索引一致，则说明查询范围在一个段内. 段索引 = %llu\n", leftPos);

        bool keyValueFlag;

        // 计算目录索引和获取对应段
        uint64_t dir_index = GET_SEG_NUM(leftSubkey, ERT_NODE_LENGTH, tmp->global_depth);
        ERTIntSegment* tmp_seg = *(ERTIntSegment**)GET_SEG_POS(tmp, dir_index);

        // 计算段内桶索引
        uint64_t seg_index = GET_BUCKET_NUM(leftSubkey, ERT_BUCKET_MASK_LEN);
        ERTIntBucket* tmp_bucket = &(tmp_seg->bucket[seg_index]);

        // 在桶中查找子键对应的值
        uint64_t value = tmp_seg->bucket[seg_index].get(leftSubkey, keyValueFlag);

        // 验证段一致性：确保段深度匹配
        // if (value == 0 || (tmp_seg != *(ERTIntSegment**)GET_SEG_POS(tmp, GET_SEG_NUM(leftSubkey, ERT_NODE_LENGTH, tmp_seg->depth)))) {
        if (value == 0 || (tmp_seg != *(ERTIntSegment**)GET_SEG_POS(tmp, GET_SEG_NUM(leftSubkey, ERT_NODE_LENGTH, tmp->global_depth)))) {
            return;
        }

        // 如果已处理完所有64位键，构建完整键并添加到结果
        if (pos == 64) {
            ERTIntKeyValue tmp_kv;
            tmp_kv.key = leftSubkey + prefix; // 完整键 = 前缀 + 子键
            tmp_kv.value = value;
            res.push_back(tmp_kv);
        } else {
            // 如果是键值对，直接添加到结果
            if (keyValueFlag) {
                res.push_back(*(ERTIntKeyValue*)value);
            } else {
                // 如果是节点指针，递归扫描子节点
                nodeScan((ERTIntNode*)value, left, right, res, pos, prefix + leftSubkey);
            }
        }
        return;
    }

    // 情况2：左右边界子键不同，需要扫描多个段
    ERTIntSegment* last_seg = NULL;
    // printf("情况3.2：如果左右段索引不一致，则说明查询范围跨多个段. 段索引 = %llu - %llu, 左边界子键 = 0x%08llx, 右边界子键 = 0x%08llx\n",
    //        leftPos, rightPos, leftSubkey, rightSubkey);

    // 遍历左右边界之间的所有段
    for (uint32_t i = leftPos; i <= rightPos; i++) {
        // 获取当前目录索引对应的段
        ERTIntSegment* tmp_seg = *(ERTIntSegment**)GET_SEG_POS(tmp, i);

        // 跳过重复段（段分裂可能导致多个目录项指向同一段）
        if (tmp_seg == last_seg)
            continue;
        else {
            last_seg = tmp_seg;
        }

        // TODO优化：如果leftSubkey == rightSubkey，无需扫描整个段

        // 遍历段内的所有桶（256个桶）
        for (auto j = 0; j < ERT_MAX_BUCKET_NUM; j++) {
            // 遍历桶内的所有槽位（4个槽位）
            for (auto k = 0; k < ERT_BUCKET_SIZE; k++) {
                // 获取子键标志和实际子键
                bool keyValueFlag = GET_NODE_FLAG(tmp_seg->bucket[j].counter[k].subkey);
                uint64_t curSubkey = REMOVE_NODE_FLAG(tmp_seg->bucket[j].counter[k].subkey);
                uint64_t value = tmp_seg->bucket[j].counter[k].value;
                // printf("扫描段 %llu, 桶 %d, 槽位 %d: curSubkey = 0x%08llx, value = %llu, keyValueFlag = %d, dir_index = %llu\n",
                //        i, j, k, curSubkey, value, keyValueFlag, GET_SEG_NUM(curSubkey, ERT_NODE_LENGTH, tmp->global_depth));
                // 跳过空槽位或段不一致的项
                // if ((value == 0 && curSubkey == 0) || (tmp_seg != *(ERTIntSegment**)GET_SEG_POS(tmp, GET_SEG_NUM(curSubkey, ERT_NODE_LENGTH, tmp_seg->depth)))) {
                if ((value == 0 && curSubkey == 0) || (tmp_seg != *(ERTIntSegment**)GET_SEG_POS(tmp, GET_SEG_NUM(curSubkey, ERT_NODE_LENGTH, tmp->global_depth)))) {
                    continue;
                }
                // printf("实际有效键值：扫描段 %llu, 桶 %d, 槽位 %d: curSubkey = 0x%08llx, value = %llu, keyValueFlag = %d, dir_index = %llu\n",
                //        i, j, k, curSubkey, value, keyValueFlag, GET_SEG_NUM(curSubkey, ERT_NODE_LENGTH, tmp->global_depth));
                // 情况2.1：当前子键在左右边界之间（开区间）
                if ((leftSubkey == UINT64_MAX || curSubkey > leftSubkey) && (rightSubkey == UINT64_MAX || curSubkey < rightSubkey)) {
                    // 如果已处理完所有64位键
                    if (pos == 64) {
                        // printf("情况2.1.1: 当前子键在左右边界之间，且到达键的末尾，直接添加到结果\n");
                        ERTIntKeyValue tmp_kv;
                        tmp_kv.key = curSubkey + prefix;
                        tmp_kv.value = value;
                        res.push_back(tmp_kv);
                    } else {
                        // 如果是键值对，直接添加
                        if (keyValueFlag) {
                            // printf("情况2.1.2: 当前子键在左右边界之间，未到达键的末尾，是键值对，直接添加\n");
                            res.push_back(*(ERTIntKeyValue*)value);
                        } else {
                            // printf("情况2.1.3: 当前子键在左右边界之间，未到达键的末尾，是节点指针，递归扫描子节点\n");
                            // 如果是节点指针，获取该节点下的所有键值对
                            // getAllNodes((ERTIntNode*)value, res, prefix + curSubkey);

                            getAllNodes((ERTIntNode*)value, res, pos, prefix + curSubkey);
                        }
                    }
                }
                // 情况2.2：当前子键等于左右边界之一
                else if (curSubkey == leftSubkey || curSubkey == rightSubkey) {
                    // 如果已处理完所有64位键
                    if (pos == 64) {
                        // printf("情况2.2.1: 当前子键等于左右边界之一，且到达键的末尾，直接添加到结果\n");
                        ERTIntKeyValue tmp_kv;
                        tmp_kv.key = curSubkey + prefix;
                        tmp_kv.value = value;
                        res.push_back(tmp_kv);
                    } else {
                        // 如果是键值对或已到达键末尾，直接添加
                        // if (pos == ERT_KEY_LENGTH || keyValueFlag) {
                        if (keyValueFlag) {
                            // printf("情况2.2.2: 当前子键等于左右边界之一，未到达键的末尾，但找到的是键值对，直接添加到结果\n");
                            res.push_back(*(ERTIntKeyValue*)value);
                        } else {
                            // printf("情况2.2.3: 当前子键等于左右边界之一，未到达键的末尾，找到的是节点指针，递归扫描子节点\n");
                            // 如果是节点指针，递归扫描子节点

                            nodeScan((ERTIntNode*)value, left, right, res, pos, prefix + curSubkey);
                        }
                    }
                }
            }
        }
    }
}

void ERTInt::getAllNodes(ERTIntNode* tmp, vector<ERTIntKeyValue>& res, int pos, uint64_t prefix) {
    // printf("getAllNodes called. pos: %d, prefix: %016llx\n", pos, prefix);

    if (tmp == NULL) {
        return;
    }
    // printf("tmp header: len = %d, depth = %d\n",
    //        tmp->header.len, tmp->header.depth);
    // for (int i = 0; i < 6; i++) {
    //     printf("array[%d] = %02x\n", i, tmp->header.array[i]);
    // }
    // if (tmp->header.len > 0) {
    if (tmp->header.len % (ERT_NODE_LENGTH / SIZE_OF_CHAR) == 0) {
        prefix = (prefix << tmp->header.len * SIZE_OF_CHAR);
        for (int i = 0; i < tmp->header.len; ++i) {
            prefix += tmp->header.array[i] << (tmp->header.len - i);
        }
        pos += tmp->header.len * SIZE_OF_CHAR;
    }
    prefix = (prefix << ERT_NODE_LENGTH);
    pos += ERT_NODE_LENGTH;
    ERTIntSegment* last_seg = NULL;

    for (int i = 0; i < tmp->dir_size; i++) {
        ERTIntSegment* tmp_seg = *(ERTIntSegment**)GET_SEG_POS(tmp, i);
        if (tmp_seg == last_seg)
            continue;
        else
            last_seg = tmp_seg;

        for (auto j = 0; j < ERT_MAX_BUCKET_NUM; j++) {
            for (auto k = 0; k < ERT_BUCKET_SIZE; k++) {
                bool keyValueFlag = GET_NODE_FLAG(tmp_seg->bucket[j].counter[k].subkey);
                uint64_t curSubkey = REMOVE_NODE_FLAG(tmp_seg->bucket[j].counter[k].subkey);
                uint64_t value = tmp_seg->bucket[j].counter[k].value;
                if ((curSubkey == 0 && value == 0) || (tmp_seg != *(ERTIntSegment**)GET_SEG_POS(tmp, GET_SEG_NUM(curSubkey, ERT_NODE_LENGTH, tmp->global_depth)))) {
                    continue;
                }
                // printf("getAllNodes 实际有效键值：扫描段 %llu, 桶 %d, 槽位 %d: curSubkey = 0x%08llx, value = %llu, keyValueFlag = %d, dir_index = %llu, pos = %d\n",
                //        i, j, k, curSubkey, value, keyValueFlag, GET_SEG_NUM(curSubkey, ERT_NODE_LENGTH, tmp->global_depth), pos);
                if (pos == 64) {
                    ERTIntKeyValue tmp;
                    tmp.key = curSubkey + prefix;
                    tmp.value = value;
                    res.push_back(tmp);
                } else {
                    // if ((curSubkey == 0 && value == 0) || (tmp_seg != *(ERTIntSegment**)GET_SEG_POS(tmp, GET_SEG_NUM(curSubkey, ERT_NODE_LENGTH, tmp_seg->depth)))) {
                    // if ((curSubkey == 0 && value == 0) || (tmp_seg != *(ERTIntSegment**)GET_SEG_POS(tmp, GET_SEG_NUM(curSubkey, ERT_NODE_LENGTH, tmp->global_depth)))) {
                    //     continue;
                    // }
                    if (keyValueFlag) {
                        res.push_back(*(ERTIntKeyValue*)value);
                    } else {
                        getAllNodes((ERTIntNode*)value, res, pos, prefix + curSubkey);
                    }
                }
            }
        }
    }
}

uint64_t ERTInt::memory_profile(ERTIntNode* tmp, int pos) {
    if (tmp == NULL) {
        tmp = root;
    }
    uint64_t res = tmp->dir_size * 8 + sizeof(ERTIntNode);
    // uint64_t res = 0;
    pos += ERT_NODE_LENGTH;
    ERTIntSegment* last_seg = NULL;
    // printf("tmp->dir_size: %llu,res: %llu\n", tmp->dir_size, res);

    for (int i = 0; i < tmp->dir_size; i++) {
        ERTIntSegment* tmp_seg = *(ERTIntSegment**)GET_SEG_POS(tmp, i);
        if (tmp_seg == last_seg)
            continue;
        else {
            last_seg = tmp_seg;
            res += ERT_MAX_BUCKET_NUM * sizeof(ERTIntBucket);
            // printf("计算段中所有桶的大小 totalMemory: %llu, bucketSize: %llu\n", res, ERT_MAX_BUCKET_NUM * sizeof(ERTIntBucket));
        }
        for (auto j = 0; j < ERT_MAX_BUCKET_NUM; j++) {
            for (auto k = 0; k < ERT_BUCKET_SIZE; k++) {
                bool keyValueFlag = GET_NODE_FLAG(tmp_seg->bucket[j].counter[k].subkey);
                uint64_t curSubkey = REMOVE_NODE_FLAG(tmp_seg->bucket[j].counter[k].subkey);
                uint64_t value = tmp_seg->bucket[j].counter[k].value;

                if (pos != 64) {
                    // if ((curSubkey == 0 && value == 0) || (tmp_seg != *(ERTIntSegment**)GET_SEG_POS(tmp, GET_SEG_NUM(curSubkey, ERT_NODE_LENGTH, tmp_seg->depth)))) {
                    if ((curSubkey == 0 && value == 0) || (tmp_seg != *(ERTIntSegment**)GET_SEG_POS(tmp, GET_SEG_NUM(curSubkey, ERT_NODE_LENGTH, tmp->global_depth)))) {
                        continue;
                    }
                    if (keyValueFlag) {
                        res += 16;
                    } else {
                        res += memory_profile((ERTIntNode*)value, pos);
                    }
                }
            }
        }
    }
    return res;
}

// 创建新的可扩展基数树实例
// 返回值：指向新创建的ERTInt对象的指针
ERTInt* NewExtendibleRadixTreeInt() {
    // 使用快速内存分配器分配 ERTInt 对象的内存空间
    // 大小：sizeof(ERTInt) - 包含 init_depth 和 root 指针
    ERTInt* _new_hash_tree = static_cast<ERTInt*>(concurrency_fast_alloc(sizeof(ERTInt)));
    // 初始化新创建的可扩展基数树
    _new_hash_tree->init();
    // 返回初始化后的基数树指针
    return _new_hash_tree;
}

ERTInt::ERTInt() {
    root = NewERTIntNode(ERT_NODE_LENGTH);
}

ERTInt::ERTInt(int _span, int _init_depth) {
    init_depth = _init_depth;
    root = NewERTIntNode(ERT_NODE_LENGTH);
}

ERTInt::~ERTInt() {
    delete root;
}

// ERTInt类的初始化函数
// 功能：初始化可扩展基数树，创建根节点
// 说明：该函数在构造函数或需要重新初始化树时被调用
void ERTInt::init() {
    // 创建新的根节点，节点长度为 ERT_NODE_LENGTH（32位）
    // NewERTIntNode 函数会分配内存并初始化节点结构
    root = NewERTIntNode(ERT_NODE_LENGTH);
}
