#include <iostream>
#include <cstring>
#include <algorithm>
#include <vector>
#include <queue>
#include <chrono>
#include <algorithm>
#include <vector>
#include <iomanip>
#include <unistd.h>
#include <cstdint>

#include "wrert.h"

namespace wrert {

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

OperationResults WRERT::insert(uint64_t key, uint64_t value, WRERTNode* node, int keyLength, WRERTKeyValue* _kv) {
    WRERTNode* currentNode = node;
    if (WRERT_UNLIKELY(node == nullptr)) {
        currentNode = root;
    }

    uint32_t currentKeyPosition = keyLength;
    uint64_t beforeAddress = (uint64_t)&root;
    uint32_t arrayMatchLength = 0;
    uint8_t nodeFlag;
    uint16_t fingerprint = 0;

    int prefixMatchLength;
    while (currentKeyPosition < WRERT_KEY_MAX_BYTES) {
        // 步骤1: 计算匹配的前缀
        // prefixMatchLength = currentNode->directoryNode.header.computePrefix(key, currentKeyPosition * WRERT_SIZE_OF_CHAR, &arrayMatchLength);
        prefixMatchLength = currentNode->directoryNode.header.computePrefix(key, currentKeyPosition << 2, &arrayMatchLength);
        // 情况0：前缀数组为空
        if (prefixMatchLength == -1) {
            // printf("情况0: 前缀数组为空 - 为空前缀分配新前缀\n");

            currentNode->directoryNode.header.prefixLength = WRERT_NODE_PREFIX_MAX_BYTES / WRERT_NODE_SUBKEY_MAX_BYTES;
            currentNode->directoryNode.header.assignPrefix(key, currentKeyPosition * WRERT_SIZE_OF_CHAR);
            continue;
        }
        // 情况1: 前缀匹配成功，到达键的末尾 - 直接插入当前 globalSegment
        else if (currentKeyPosition + arrayMatchLength == WRERT_KEY_MAX_BYTES) {
            // printf("情况1: 前缀匹配成功，到达键的末尾 - 直接插入当前 globalSegment\n");
            WRERTKeyValue* globalSegment = reinterpret_cast<WRERTKeyValue*>(currentNode->directoryNode.directory.globalSegmentPtr);
            currentNode->putDirNode(globalSegment, key, value);
            return OperationResults::Success;
        }
        // 情况2：前缀长度为与匹配长度相等 - 直接插入当前节点或者其子节点
        else if (currentNode->directoryNode.header.prefixLength == prefixMatchLength) {
            currentKeyPosition += currentNode->directoryNode.header.prefixLength << 2;
            const uint64_t subKey = WRERT_GET_SUBKEY(key, (currentKeyPosition << 3), WRERT_NODE_SPAN);
            const uint8_t globalDepth = currentNode->directoryNode.directory.globalDepth;
            const uint64_t segmentIndex = WRERT_GET_SEGMENT_NUMBER(subKey, WRERT_NODE_SPAN, globalDepth);
            WRERTSegmentNode* segmentNode = reinterpret_cast<WRERTSegmentNode*>(
              WRERT_GET_SEGMENT_POSITION(&currentNode->directoryNode, segmentIndex));
            WRERTSegment* segment = reinterpret_cast<WRERTSegment*>(segmentNode->segmentPtr);

            const uint8_t sharedBucketOffset = segmentNode->sharedBucketOffset == 0 ? 0 : segmentNode->sharedBucketOffset + 1;
            const uint8_t localDepth = segmentNode->localDepth;
            const int8_t sharedBucketVersion = localDepth - 1;

            const uint64_t sharedBucketIndex = WRERT_GET_SHARED_BUCKET_INDEX(subKey, localDepth, WRERT_MAIN_BUCKETS_MAX_BITS);
            const uint64_t mainBucketIndex = WRERT_GET_MAIN_BUCKET_INDEX(subKey, localDepth, WRERT_MAIN_BUCKETS_MAX_BITS);
            const uint64_t sharedBucketPosition = sharedBucketOffset + (sharedBucketIndex & WRERT_SHARED_BUCKETS_PER_SEGMENT_MASK);
            const uint64_t mainBucketPosition = calcBucketPosition(sharedBucketOffset, mainBucketIndex);
            WRERTBucket* sharedBucket = &segment->buckets[sharedBucketPosition];
            WRERTBucket* mainBucket = &segment->buckets[mainBucketPosition];
            _mm_prefetch((const char*)sharedBucket, _MM_HINT_T0);
            _mm_prefetch((const char*)mainBucket, _MM_HINT_T0);

            uint64_t shareSlotIndex, mainSlotIndex;
            // 0 shared bucket，1 main bucket，-1 not found
            int8_t _isShareOrMainFlag = 0;
            WRERTSlotKeyValue* slot;
            bool _isValidSharedSlot = false, _isValidMainSlot = false, _isFindFlag = false;

            // for (shareSlotIndex = 0; shareSlotIndex < WRERT_SLOTS_PER_BUCKET; ++shareSlotIndex) {
            //     slot = &sharedBucket->counters[shareSlotIndex];
            //     // if (slot->subKeyFields.validFlag == 0 || slot->subKeyFields.migVersion != sharedBucketVersion) {
            //     //     _isValidSharedSlot = true;
            //     //     break;
            //     // } else if (slot->subKeyFields.subKey == subKey) {
            //     //     _isFindFlag = true;
            //     //     break;
            //     // }
            //     if (slot->subKeyFields.validFlag == 1 && slot->subKeyFields.migVersion == sharedBucketVersion) {
            //         if (slot->subKeyFields.subKey == subKey) {
            //             _isFindFlag = true;
            //             break;
            //         }
            //     } else {
            //         _isValidSharedSlot = true;
            //         break;
            //     }
            // }

            // const __m512i v_offsets = _mm512_load_si512((__m512i*)offsets);
            const __m512i idx = _mm512_set_epi64(14, 12, 10, 8, 6, 4, 2, 0);
            const __m512i v_subKey_target = _mm512_set1_epi64(subKey);
            const __m512i v_shared_validFlag_migVersion_target = _mm512_set1_epi64((1ULL << 41) | ((uint64_t)(sharedBucketVersion & WRERT_MIGVERSION_BITS) << 32));
            const __m512i v_validFlag_migVersion_mask = _mm512_set1_epi64(WRERT_VALIDFLAG_MIGVERSION_MASK);
            const __m512i v_subKey_mask = _mm512_set1_epi64(WRERT_SUBKEY_MASK);
            for (shareSlotIndex = 0; shareSlotIndex < WRERT_SLOTS_PER_BUCKET; shareSlotIndex += 8) {
                // __m512i keys = _mm512_mask_i64gather_epi64(_mm512_setzero_si512(), summary_mask, v_offsets, (void*)&sharedBucket->counters[shareSlotIndex], 1);
                __m512i shared_data0 = _mm512_load_si512((const __m512i*)&sharedBucket->counters[shareSlotIndex]);
                __m512i shared_data1 = _mm512_load_si512((const __m512i*)&sharedBucket->counters[shareSlotIndex + 4]);
                __m512i keys = _mm512_permutex2var_epi64(shared_data0, idx, shared_data1);
                __m512i v_validFlag_migVersion_keys = _mm512_and_si512(keys, v_validFlag_migVersion_mask);
                __mmask8 validFlag_migVersion_match_mask = _mm512_cmpeq_epi64_mask(v_validFlag_migVersion_keys, v_shared_validFlag_migVersion_target);
                __mmask8 bad_slot_mask = summary_mask & (~validFlag_migVersion_match_mask);
                if (validFlag_migVersion_match_mask) {
                    __m512i v_subKey_keys = _mm512_and_si512(keys, v_subKey_mask);
                    __mmask8 subKey_match_mask = _mm512_cmpeq_epi64_mask(v_subKey_keys, v_subKey_target);
                    if (subKey_match_mask) {
                        int x = __builtin_ctz(subKey_match_mask);
                        int y = 31 - __builtin_clz(validFlag_migVersion_match_mask);
                        if (x <= y) {
                            _isFindFlag = true;
                            shareSlotIndex += __builtin_ctz(subKey_match_mask);
                            slot = &sharedBucket->counters[shareSlotIndex];
                            break;
                        } else {
                            _isValidSharedSlot = true;
                            shareSlotIndex += __builtin_ctz(bad_slot_mask);
                            slot = &sharedBucket->counters[shareSlotIndex];
                            break;
                        }
                    }
                    if (bad_slot_mask) {
                        _isValidSharedSlot = true;
                        shareSlotIndex += __builtin_ctz(bad_slot_mask);
                        slot = &sharedBucket->counters[shareSlotIndex];
                        break;
                    }
                } else {
                    _isValidSharedSlot = true;
                    slot = &sharedBucket->counters[shareSlotIndex];
                    break;
                }
            }

            if (!_isFindFlag) {
                // for (mainSlotIndex = 0; mainSlotIndex < WRERT_SLOTS_PER_BUCKET; ++mainSlotIndex) {
                //     slot = &mainBucket->counters[mainSlotIndex];
                //     // if (slot->subKeyFields.validFlag == 0 || slot->subKeyFields.migVersion != localDepth) {
                //     //     _isValidMainSlot = true;
                //     //     break;
                //     // } else if (slot->subKeyFields.subKey == subKey) {
                //     //     _isFindFlag = true;
                //     //     break;
                //     // }
                //     if (slot->subKeyFields.validFlag == 1 && slot->subKeyFields.migVersion == localDepth) {
                //         if (slot->subKeyFields.subKey == subKey) {
                //             _isFindFlag = true;
                //             break;
                //         }
                //     } else {
                //         _isValidMainSlot = true;
                //         break;
                //     }
                // }

                const __m512i v_main_validFlag_migVersion_target = _mm512_set1_epi64((1ULL << 41) | ((uint64_t)localDepth << 32));
                for (mainSlotIndex = 0; mainSlotIndex < WRERT_SLOTS_PER_BUCKET; mainSlotIndex += 8) {
                    // __m512i keys = _mm512_mask_i64gather_epi64(_mm512_setzero_si512(), summary_mask, v_offsets, (void*)&mainBucket->counters[mainSlotIndex], 1);
                    __m512i main_data0 = _mm512_load_si512((const __m512i*)&mainBucket->counters[mainSlotIndex]);
                    __m512i main_data1 = _mm512_load_si512((const __m512i*)&mainBucket->counters[mainSlotIndex + 4]);
                    __m512i keys = _mm512_permutex2var_epi64(main_data0, idx, main_data1);
                    __m512i v_validFlag_migVersion_keys = _mm512_and_si512(keys, v_validFlag_migVersion_mask);
                    __mmask8 validFlag_migVersion_match_mask = _mm512_cmpeq_epi64_mask(v_validFlag_migVersion_keys, v_main_validFlag_migVersion_target);
                    __mmask8 bad_slot_mask = summary_mask & (~validFlag_migVersion_match_mask);
                    if (validFlag_migVersion_match_mask) {
                        __m512i v_subKey_keys = _mm512_and_si512(keys, v_subKey_mask);
                        __mmask8 subKey_match_mask = _mm512_cmpeq_epi64_mask(v_subKey_keys, v_subKey_target);
                        if (subKey_match_mask) {
                            int x = __builtin_ctz(subKey_match_mask);
                            int y = 31 - __builtin_clz(validFlag_migVersion_match_mask);
                            if (x <= y) {
                                _isFindFlag = true;
                                mainSlotIndex += __builtin_ctz(subKey_match_mask);
                                slot = &mainBucket->counters[mainSlotIndex];
                                break;
                            } else {
                                _isValidMainSlot = true;
                                mainSlotIndex += __builtin_ctz(bad_slot_mask);
                                slot = &mainBucket->counters[mainSlotIndex];
                                break;
                            }
                        }
                        if (bad_slot_mask) {
                            _isValidMainSlot = true;
                            mainSlotIndex += __builtin_ctz(bad_slot_mask);
                            slot = &mainBucket->counters[mainSlotIndex];
                            break;
                        }
                    } else {
                        _isValidMainSlot = true;
                        slot = &mainBucket->counters[mainSlotIndex];
                        //     printf("avx2主桶插槽 %lu 新键, j: %d, subKey: 0x%08llx, slot->subKeyFields.subKey: 0x%08llx, value: %lu, validFlag: %lu, migVersion: %d, mainBucketVersion: %d, localDepth: %lu, segmentIndex: %lu\n",
                        //            mainSlotIndex, j, subKey, slot->subKeyFields.subKey, slot->valueFields.value, slot->subKeyFields.validFlag, slot->subKeyFields.migVersion, mainBucketVersion, localDepth, segmentIndex);
                        break;
                    }
                }

                if (!_isFindFlag) {
                    if (_isValidSharedSlot) {
                        // 优先使用共享桶的可用slot
                        slot = &sharedBucket->counters[shareSlotIndex];
                    } else if (_isValidMainSlot) {
                        // 共享桶不可用时，标记主桶可用
                        _isShareOrMainFlag = 1;
                    } else {
                        slot = nullptr;
                    }
                }
            }

            currentKeyPosition += WRERT_NODE_SUBKEY_MAX_BYTES;

            if (currentKeyPosition == WRERT_KEY_MAX_BYTES) {
                if (!_isFindFlag || slot == nullptr) {
                    // printf("情况2.1.1: 到达键的末尾，新键值对，直接插入\n");
                    currentNode->put(
                      subKey, value, segmentNode, segment, slot,
                      segmentIndex, sharedBucketOffset, fingerprint, _isShareOrMainFlag, nodeFlag = 0, &beforeAddress);
                    return OperationResults::Success;
                } else {
                    slot->valueFields.value = value;
                    clflush((char*)&slot->valueFields, sizeof(slot->valueFields));
                    // printf("情况2.1.2: 到达键的末尾，已有键值对，更新值\n");
                    return OperationResults::Success;
                }
            } else {
                if (!_isFindFlag || slot == nullptr) {
                    WRERTKeyValue* kv = _kv;
                    if (kv == nullptr) {
                        kv = NewWRERTKeyValue(key, value);
                        clflush((char*)kv, sizeof(WRERTKeyValue));
                    }
                    // printf("情况2.2.1: 还未到达键的末尾，新键值对，直接插入\n");
                    currentNode->put(
                      subKey, (uint64_t)kv, segmentNode, segment, slot,
                      segmentIndex, sharedBucketOffset, fingerprint, _isShareOrMainFlag, nodeFlag = 0, &beforeAddress);
                    return OperationResults::Success;
                } else {
                    if (slot->subKeyFields.nodeFlag == 1) {
                        currentNode = (WRERTNode*)slot->valueFields.value;
                        beforeAddress = (uint64_t)slot;
                        // printf("情况2.2.2: 还未到达键的末尾，指向下一个节点，继续递归\n");
                    } else {
                        WRERTKeyValue* kv = (WRERTKeyValue*)slot->valueFields.value;
                        if (kv->key == key) {
                            kv->value = value;
                            clflush((char*)&kv->value, sizeof(kv->value));

                            // printf("情况2.2.3: 还未到达键的末尾，已有键值对，更新值\n");

                            return OperationResults::Success;
                        } else {
                            WRERTNode* newNode = NewWRERTNode();

                            newNode->directoryNode.header.initialize(
                              &currentNode->directoryNode.header, 0, currentKeyPosition / WRERT_NODE_SUBKEY_MAX_BYTES);
                            //   &currentNode->directoryNode.header, 0, currentKeyPosition >> 2);

                            // printf("情况2.2.4: 还未到达键的末尾，键不同，创建新节点\n");

                            // TUDO: 待优化

                            insert(kv->key, kv->value, newNode, currentKeyPosition, kv);
                            insert(key, value, newNode, currentKeyPosition, _kv);

                            clflush((char*)newNode, sizeof(WRERTNode));

                            slot->subKeyFields.nodeFlag = 1;
                            slot->valueFields.value = (uint64_t)newNode;

                            clflush((char*)slot, sizeof(WRERTSlotKeyValue));

                            return OperationResults::Success;
                        }
                    }
                }
            }
        } else if (currentNode->directoryNode.header.prefixLength != 0
                   && prefixMatchLength < currentNode->directoryNode.header.prefixLength) {
            // printf("情况3: 前缀不匹配，需要节点路径压缩的分裂\n");
            // 步骤1: 创建新的节点
            WRERTNode* newNode = NewWRERTNode();
            // 步骤2: 初始化新节点的头部信息
            newNode->directoryNode.header.initialize(
              &currentNode->directoryNode.header, prefixMatchLength, currentNode->directoryNode.header.nodeHeaderDepth);
            // 步骤3: 获取全局段指针
            if (currentNode->directoryNode.directory.globalSegmentPtr != 0) {
                WRERTKeyValue* newGlobalSegment = reinterpret_cast<WRERTKeyValue*>(newNode->directoryNode.directory.globalSegmentPtr);
                WRERTKeyValue* globalSegment = reinterpret_cast<WRERTKeyValue*>(currentNode->directoryNode.directory.globalSegmentPtr);
                for (int j = currentNode->directoryNode.header.prefixLength - prefixMatchLength; j <= currentNode->directoryNode.header.prefixLength; j++) {
                    newGlobalSegment[j - currentNode->directoryNode.header.prefixLength + prefixMatchLength] = globalSegment[j];
                }
            }
            // 步骤4: 计算子键：从当前键位置提取 WRERT_NODE_SUBKEY_LENGTH 位的子键
            uint64_t subKey = WRERT_GET_SUBKEY(key, (currentKeyPosition + prefixMatchLength) * WRERT_SIZE_OF_CHAR, WRERT_NODE_SPAN);
            // uint64_t subKey = WRERT_GET_SUBKEY(key, ((currentKeyPosition + prefixMatchLength) << 3), WRERT_NODE_SPAN);
            // 步骤5: 创建新的键值对并持久化
            WRERTKeyValue* kv = NewWRERTKeyValue(key, value);
            clflush((char*)kv, sizeof(WRERTKeyValue));
            // 步骤6: 将新键值对插入新节点 (可以直接插入，待优化)
            newNode->putSegNode(subKey, (uint64_t)kv, fingerprint, &beforeAddress, nodeFlag = 0);
            // 步骤7: 将当前节点插入新节点
            newNode->putSegNode(WRERT_GET_PREFIX_ARRAY_BITS(currentNode->directoryNode.header.prefixArray,
                                                            prefixMatchLength * WRERT_NODE_SUBKEY_MAX_BYTES,
                                                            WRERT_NODE_SUBKEY_MAX_BYTES),
                                (uint64_t)currentNode, fingerprint, &beforeAddress, nodeFlag = 1);

            // 步骤8: 更新当前节点的头部信息, 更新当前节点的前缀长度和节点头深度
            currentNode->directoryNode.header.prefixLength -=
              //   (prefixMatchLength + newNode->directoryNode.header.nodeHeaderDepth);
              (prefixMatchLength + newNode->directoryNode.header.nodeHeaderDepth + 1);
            currentNode->directoryNode.header.nodeHeaderDepth =
              //   newNode->directoryNode.header.prefixLength + newNode->directoryNode.header.nodeHeaderDepth;
              newNode->directoryNode.header.prefixLength + newNode->directoryNode.header.nodeHeaderDepth + 1;

            for (size_t i = 0; i <= prefixMatchLength; i++) {
                for (size_t j = i * WRERT_NODE_SUBKEY_MAX_BYTES; j < (i + 1) * WRERT_NODE_SUBKEY_MAX_BYTES; j++) {
                    if (j + WRERT_NODE_SUBKEY_MAX_BYTES < WRERT_NODE_PREFIX_MAX_BYTES) {
                        currentNode->directoryNode.header.prefixArray[j] = currentNode->directoryNode.header.prefixArray[j + WRERT_NODE_SUBKEY_MAX_BYTES];
                    } else {
                        currentNode->directoryNode.header.prefixArray[j] = 0;
                    }
                }
            }

            // 步骤9: 持久化当前节点头部信息
            clflush((char*)newNode, sizeof(WRERTNode));
            clflush((char*)&(currentNode->directoryNode.header), sizeof(WRERTHeader));

            if (newNode->directoryNode.header.nodeHeaderDepth != 0) {
                ((WRERTSlotKeyValue*)beforeAddress)->valueFields.value = (uint64_t)newNode;
                clflush((char*)(WRERTSlotKeyValue*)beforeAddress, sizeof(WRERTSlotKeyValue));
            } else {
                *(WRERTNode**)beforeAddress = newNode;
                clflush((char*)beforeAddress, sizeof(WRERTNode*));
            }

            return OperationResults::Success;
        }
    }

    return OperationResults::Failed;
}

uint64_t WRERT::search(uint64_t key, WRERTNode* node, int keyLength) {
    WRERTNode* currentNode = node;
    if (WRERT_UNLIKELY(node == nullptr)) {
        currentNode = root;
    }
    if (WRERT_UNLIKELY(currentNode == nullptr)) {
        return (uint64_t)OperationResults::NotFound;
    }

    uint32_t currentKeyPosition = keyLength;
    uint32_t arrayMatchLength;

    int prefixMatchLength;
    while (currentKeyPosition < WRERT_KEY_MAX_BYTES) {
        if (WRERT_UNLIKELY(currentNode->directoryNode.header.prefixLength > 0)) {
            prefixMatchLength = currentNode->directoryNode.header.computePrefix(key, currentKeyPosition * WRERT_SIZE_OF_CHAR, &arrayMatchLength);
            if (arrayMatchLength != 0 && currentKeyPosition + arrayMatchLength <= currentNode->directoryNode.header.prefixLength * WRERT_NODE_SUBKEY_MAX_BYTES) {
                WRERTKeyValue* globalSegment = reinterpret_cast<WRERTKeyValue*>(currentNode->directoryNode.directory.globalSegmentPtr);
                return currentNode->getDirNode(globalSegment, key);
            } else if (currentNode->directoryNode.header.prefixLength == prefixMatchLength) {
                currentKeyPosition += currentNode->directoryNode.header.prefixLength * WRERT_NODE_SUBKEY_MAX_BYTES;
            } else {
                return (uint64_t)OperationResults::Failed;
            }
        }

        // uint64_t subKey = WRERT_GET_SUBKEY(key, currentKeyPosition * WRERT_SIZE_OF_CHAR, WRERT_NODE_SPAN);
        uint64_t subKey = WRERT_GET_SUBKEY(key, (currentKeyPosition << 3), WRERT_NODE_SPAN);

        WRERTSlotKeyValue* slot = nullptr;
        slot = currentNode->getSegNode(subKey);

        currentKeyPosition +=
          currentNode->directoryNode.header.prefixLength == 0 ? WRERT_NODE_SUBKEY_MAX_BYTES : currentNode->directoryNode.header.prefixLength << 2;
        //   currentNode->directoryNode.header.prefixLength == 0 ? WRERT_NODE_SUBKEY_MAX_BYTES : currentNode->directoryNode.header.prefixLength * WRERT_NODE_SUBKEY_MAX_BYTES;

        if (slot == nullptr) {
            return (uint64_t)OperationResults::Failed;
        }

        if (slot->subKeyFields.nodeFlag == 0) {
            if (currentKeyPosition == WRERT_KEY_MAX_BYTES) {
                return slot->valueFields.value;
            } else {
                // if (slot->subKeyFields.fingerprint == currentNode->fingerprint(key)) {
                if (key == (reinterpret_cast<WRERTKeyValue*>(slot->valueFields.value))->key) {
                    return (reinterpret_cast<WRERTKeyValue*>(slot->valueFields.value))->value;
                } else {
                    return (uint64_t)OperationResults::Failed;
                }
            }
        } else {
            currentNode = reinterpret_cast<WRERTNode*>(slot->valueFields.value);
        }
    }

    return (uint64_t)OperationResults::NotFound;
}

std::vector<WRERTKeyValue> WRERT::lookupRange(uint64_t leftKey, uint64_t rightKey) {
    std::vector<WRERTKeyValue> results;

    // printf("=======================================\n");
    // printf("lookupRange: leftKey = %llu, 0x%016llx, rightKey = %llu, 0x%016llx\n", leftKey, leftKey, rightKey, rightKey);
    // printf("=======================================\n");

    // auto start_time = std::chrono::high_resolution_clock::now();
    nodeScan(root, leftKey, rightKey, results, 0, 0);
    // auto end_time = std::chrono::high_resolution_clock::now();
    // printf("lookupRange: %llu, %llu, latency = %llu\n", leftKey, rightKey, std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count());

    return results;
}

static int num = 0;

void WRERT::nodeScan(WRERTNode* node, uint64_t leftKey, uint64_t rightKey,
                     std::vector<WRERTKeyValue>& results, int currentKeyPosition, uint64_t prefix) {
    WRERTNode* currentNode = node;
    if (WRERT_UNLIKELY(node == nullptr)) {
        currentNode = root;
    }

    if (WRERT_UNLIKELY(currentNode == nullptr)) {
        return;
    }

    uint32_t arrayMatchLength = 0;

    uint64_t leftSegmentIndex = UINT64_MAX, rightSegmentIndex = UINT64_MAX;

    if (WRERT_UNLIKELY(leftKey == rightKey)) {
        // printf("情况1：如果左键等于右键相当于是点查询单个键\n");
        uint64_t result = search(leftKey);
        WRERTKeyValue keyValue;
        if (result != (uint64_t)OperationResults::Failed) {
            keyValue.key = leftKey;
            keyValue.value = result;
            results.push_back(keyValue);
        }

        return;
    }

    if (WRERT_UNLIKELY(leftKey > rightKey)) {
        // printf("情况2：如果左键大于右键，说明范围为空，直接返回\n");
        return;
    }

    // 情况3：正常范围查询
    if (WRERT_UNLIKELY(currentNode->directoryNode.header.prefixLength > 0)) {
        uint64_t subKey;

        for (size_t i = 0; i < currentNode->directoryNode.header.prefixLength * WRERT_NODE_SUBKEY_MAX_BYTES; i++) {
            subKey = WRERT_GET_SUBKEY(leftKey, (currentKeyPosition + i) * WRERT_SIZE_OF_CHAR, WRERT_SIZE_OF_CHAR);
            if (subKey == currentNode->directoryNode.header.prefixArray[i]) {
                continue;
            } else if (subKey > currentNode->directoryNode.header.prefixArray[i]) {
                return;
            } else {
                leftSegmentIndex = 0;
                break;
            }
        }

        for (size_t i = 0; i < currentNode->directoryNode.header.prefixLength * WRERT_NODE_SUBKEY_MAX_BYTES; i++) {
            subKey = WRERT_GET_SUBKEY(rightKey, (currentKeyPosition + i) * WRERT_SIZE_OF_CHAR, WRERT_SIZE_OF_CHAR);
            if (subKey == currentNode->directoryNode.header.prefixArray[i]) {
                continue;
            } else if (subKey > currentNode->directoryNode.header.prefixArray[i]) {
                rightSegmentIndex = currentNode->directoryNode.capacity - 1;
                break;
            } else {
                return;
            }
        }

        prefix = (prefix << currentNode->directoryNode.header.prefixLength * WRERT_NODE_SUBKEY_MAX_BYTES * WRERT_SIZE_OF_CHAR);

        for (size_t i = 0; i < currentNode->directoryNode.header.prefixLength * WRERT_NODE_SUBKEY_MAX_BYTES; ++i) {
            prefix += currentNode->directoryNode.header.prefixArray[i] << (currentNode->directoryNode.header.prefixLength * WRERT_NODE_SUBKEY_MAX_BYTES - i);
        }

        currentKeyPosition += currentNode->directoryNode.header.prefixLength * WRERT_NODE_SUBKEY_MAX_BYTES;
    }

    const uint8_t globalDepth = currentNode->directoryNode.directory.globalDepth;

    uint64_t leftSubKey = UINT64_MAX, rightSubKey = UINT64_MAX;
    if (WRERT_LIKELY(leftSegmentIndex == UINT64_MAX)) {
        // leftSubKey = WRERT_GET_SUBKEY(leftKey, currentKeyPosition * WRERT_SIZE_OF_CHAR, WRERT_NODE_SPAN);
        leftSubKey = WRERT_GET_SUBKEY(leftKey, (currentKeyPosition << 3), WRERT_NODE_SPAN);
        leftSegmentIndex = WRERT_GET_SEGMENT_NUMBER(leftSubKey, WRERT_NODE_SPAN, globalDepth);
    }

    if (WRERT_LIKELY(rightSegmentIndex == UINT64_MAX)) {
        // rightSubKey = WRERT_GET_SUBKEY(rightKey, currentKeyPosition * WRERT_SIZE_OF_CHAR, WRERT_NODE_SPAN);
        rightSubKey = WRERT_GET_SUBKEY(rightKey, (currentKeyPosition << 3), WRERT_NODE_SPAN);
        rightSegmentIndex = WRERT_GET_SEGMENT_NUMBER(rightSubKey, WRERT_NODE_SPAN, globalDepth);
    }

    prefix = (prefix << WRERT_NODE_SPAN);
    currentKeyPosition += WRERT_NODE_SUBKEY_MAX_BYTES;

    if (leftSegmentIndex == rightSegmentIndex) {
        // printf("情况3.1：如果左右段索引一致，则说明查询范围在一个段内. 段索引 = %llu, 左边界子键 = 0x%08llx, 右边界子键 = 0x%08llx\n",
        //        leftSegmentIndex, leftSubKey, rightSubKey);
        WRERTSegmentNode* segmentNode = reinterpret_cast<WRERTSegmentNode*>(
          WRERT_GET_SEGMENT_POSITION(&currentNode->directoryNode, leftSegmentIndex));
        WRERTSegment* segment = reinterpret_cast<WRERTSegment*>(segmentNode->segmentPtr);
        const uint64_t sharedBucketOffset = segmentNode->sharedBucketOffset == 0 ? 0 : segmentNode->sharedBucketOffset + 1;
        const uint8_t localDepth = segmentNode->localDepth;

        const uint64_t leftSharedBucketIndex = WRERT_GET_SHARED_BUCKET_INDEX(leftSubKey, localDepth, WRERT_MAIN_BUCKETS_MAX_BITS);
        const uint64_t rightSharedBucketIndex = WRERT_GET_SHARED_BUCKET_INDEX(rightSubKey, localDepth, WRERT_MAIN_BUCKETS_MAX_BITS);

        const uint64_t leftSharedBucketPosition = sharedBucketOffset + (leftSharedBucketIndex & WRERT_SHARED_BUCKETS_PER_SEGMENT_MASK);
        const uint64_t rightSharedBucketPosition = sharedBucketOffset + (rightSharedBucketIndex & WRERT_SHARED_BUCKETS_PER_SEGMENT_MASK);

        const uint64_t leftMainBucketPosition_1 =
          ((sharedBucketOffset + WRERT_SHARED_BUCKETS_PER_SEGMENT) % WRERT_SEGMENT_SIZE + (leftSharedBucketPosition % WRERT_SHARED_BUCKETS_PER_SEGMENT) * 2) % WRERT_SEGMENT_SIZE;
        const uint64_t rightMainBucketPosition_2 =
          ((sharedBucketOffset + WRERT_SHARED_BUCKETS_PER_SEGMENT) % WRERT_SEGMENT_SIZE + (rightSharedBucketPosition % WRERT_SHARED_BUCKETS_PER_SEGMENT) * 2 + 1) % WRERT_SEGMENT_SIZE;

        auto bucketRangeIterate = [&](uint64_t start, uint64_t end, int64_t _migVersion) {
            for (uint64_t i = start; i <= end; i++) {
                for (uint64_t j = 0; j < WRERT_SLOTS_PER_BUCKET; j++) {
                    const auto* slot = &segment->buckets[i].counters[j];
                    // printf("情况3.1：遍历桶 %llu 中的所有 slot %llu:  curSubKey = 0x%08llx, value = %llu, migVersion = %d, _migVersion = %d, validFlag = %llu, globalDepth = %llu, localDepth = %d, leftSegmentIndex = %d\n",
                    //        i, j, slot->subKeyFields.subKey, slot->valueFields.value, slot->subKeyFields.migVersion, _migVersion, slot->subKeyFields.validFlag, globalDepth, localDepth, leftSegmentIndex);
                    if (slot->subKeyFields.validFlag == 0 || slot->subKeyFields.migVersion != _migVersion) {
                        break;
                    }
                    uint64_t curSubKey = slot->subKeyFields.subKey;
                    if (WRERT_GET_SEGMENT_NUMBER(curSubKey, WRERT_NODE_SPAN, globalDepth) == leftSegmentIndex) {
                        uint64_t value = slot->valueFields.value;
                        // printf("情况3.1：遍历桶 %llu 中的所有 slot %llu: leftSegmentIndex = %llu, curSubKey = 0x%08llx, value = %llu, migVersion = %llu, _migVersion = %llu, globalDepth = %d, localDepth = %d\n",
                        //        i, j, leftSegmentIndex, curSubKey, value, slot->subKeyFields.migVersion, _migVersion, globalDepth, segmentNode->localDepth);
                        // 如果当前 slot 存储的子键在查询范围中
                        // if ((leftSubKey == UINT64_MAX || curSubKey > leftSubKey)
                        //     && (rightSubKey == UINT64_MAX || curSubKey < rightSubKey)) {
                        if (curSubKey > leftSubKey && curSubKey < rightSubKey) {
                            if (currentKeyPosition == WRERT_KEY_MAX_BYTES) {     // 如果到键的末尾
                                results.emplace_back(curSubKey + prefix, value); // 记录匹配的键值对
                                // printf("有效kv - 情况3.1：遍历桶 %llu 中的所有 slot %llu: leftSegmentIndex = %llu, num = %llu, curSubKey = 0x%08llx, value = %llu, migVersion = %llu, _migVersion = %llu, globalDepth = %d, localDepth = %d\n",
                                //        i, j, leftSegmentIndex, ++num, curSubKey, value, slot->subKeyFields.migVersion, _migVersion, globalDepth, segmentNode->localDepth);
                            } else {
                                if (slot->subKeyFields.nodeFlag == 1) { // 如果 slot 指向的下一个节点，递归处理
                                    // auto temp_start_time = std::chrono::high_resolution_clock::now();

                                    scanAllNodes((WRERTNode*)value, results, currentKeyPosition, curSubKey + prefix); // 如果是节点指针，获取该节点下的所有键值对
                                    // auto temp_end_time = std::chrono::high_resolution_clock::now();
                                    // temp_total_time += std::chrono::duration_cast<std::chrono::nanoseconds>(temp_end_time - temp_start_time).count();

                                } else {                                       // 否则，说明是键值对指针
                                    results.push_back(*(WRERTKeyValue*)value); // 如果是键值对指针，记录匹配的键值对
                                    // printf("有效kv - 情况3.1：遍历桶 %llu 中的所有 slot %llu: leftSegmentIndex = %llu, num = %llu, curSubKey = 0x%08llx, value = %llu, migVersion = %llu, _migVersion = %llu, globalDepth = %d, localDepth = %d\n",
                                    //        i, j, leftSegmentIndex, ++num, curSubKey, (reinterpret_cast<WRERTKeyValue*>(slot->valueFields.value))->value, slot->subKeyFields.migVersion, _migVersion, globalDepth, segmentNode->localDepth);
                                }
                            }
                        } else if (curSubKey == leftSubKey || curSubKey == rightSubKey) { // 如果当前子键等于查询范围的边界子键
                            if (currentKeyPosition == WRERT_KEY_MAX_BYTES) {              // 如果到键的末尾
                                results.emplace_back(curSubKey + prefix, value);          // 记录匹配的键值对
                                // printf("有效kv - 情况3.1：遍历桶 %llu 中的所有 slot %llu: leftSegmentIndex = %llu, num = %llu, curSubKey = 0x%08llx, value = %llu, migVersion = %llu, _migVersion = %llu, globalDepth = %d, localDepth = %d\n",
                                //        i, j, leftSegmentIndex, ++num, curSubKey, value, slot->subKeyFields.migVersion, _migVersion, globalDepth, segmentNode->localDepth);

                            } else {
                                if (slot->subKeyFields.nodeFlag == 1) { // 如果 slot 指向的下一个节点，递归处理
                                    // auto temp_start_time = std::chrono::high_resolution_clock::now();

                                    nodeScan((WRERTNode*)value, leftKey, rightKey, results, currentKeyPosition, curSubKey + prefix); // 如果是节点指针，递归扫描子节点
                                    // auto temp_end_time = std::chrono::high_resolution_clock::now();
                                    // temp_total_time += std::chrono::duration_cast<std::chrono::nanoseconds>(temp_end_time - temp_start_time).count();

                                } else {                                       // 否则，说明是键值对指针
                                    results.push_back(*(WRERTKeyValue*)value); // 如果是键值对指针，记录匹配的键值对
                                    // printf("有效kv - 情况3.1：遍历桶 %llu 中的所有 slot %llu: leftSegmentIndex = %llu, num = %llu, curSubKey = 0x%08llx, value = %llu, migVersion = %llu, _migVersion = %llu, globalDepth = %d, localDepth = %d\n",
                                    //        i, j, leftSegmentIndex, ++num, curSubKey, (reinterpret_cast<WRERTKeyValue*>(slot->valueFields.value))->value, slot->subKeyFields.migVersion, _migVersion, globalDepth, segmentNode->localDepth);
                                }
                            }
                        }
                    } else {
                        break;
                    }
                }
            }
        };
        if (WRERT_SEGMENT_SIZE / 2 > leftSharedBucketPosition && WRERT_SEGMENT_SIZE / 2 < rightSharedBucketPosition) {
            bucketRangeIterate(0, rightMainBucketPosition_2, localDepth);
            bucketRangeIterate(leftSharedBucketPosition, rightSharedBucketPosition, localDepth - 1);
            bucketRangeIterate(leftMainBucketPosition_1, WRERT_SEGMENT_SIZE - 1, localDepth);
        } else {
            bucketRangeIterate(leftSharedBucketPosition, rightSharedBucketPosition, localDepth - 1);
            bucketRangeIterate(leftMainBucketPosition_1, rightMainBucketPosition_2, localDepth);
        }
        return;
    } else {
        // printf("情况3.2：如果左右段索引不一致，则说明查询范围跨多个段. 段索引 = %llu - %llu, 左边界子键 = 0x%08llx, 右边界子键 = 0x%08llx\n",
        //        leftSegmentIndex, rightSegmentIndex, leftSubKey, rightSubKey);
        WRERTSegment* segment;
        WRERTSegmentNode* segmentNode;
        WRERTSegmentNode* leftSegmentNode = reinterpret_cast<WRERTSegmentNode*>(
          WRERT_GET_SEGMENT_POSITION(&currentNode->directoryNode, leftSegmentIndex));
        WRERTSegmentNode* rightSegmentNode = reinterpret_cast<WRERTSegmentNode*>(
          WRERT_GET_SEGMENT_POSITION(&currentNode->directoryNode, rightSegmentIndex));

        const uint64_t leftSharedBucketIndex = WRERT_GET_SHARED_BUCKET_INDEX(leftSubKey, leftSegmentNode->localDepth, WRERT_MAIN_BUCKETS_MAX_BITS);
        const uint64_t rightSharedBucketIndex = WRERT_GET_SHARED_BUCKET_INDEX(rightSubKey, rightSegmentNode->localDepth, WRERT_MAIN_BUCKETS_MAX_BITS);

        uint64_t leftSharedBucketPosition, rightSharedBucketPosition;
        uint64_t leftMainBucketPosition_1, rightMainBucketPosition_2;
        uint64_t sharedBucketOffset, segmentNodeNum, segmentNodeIndexStart;

        for (uint64_t _segmentIndex = leftSegmentIndex, sameSegmentIndex = leftSegmentIndex; _segmentIndex <= rightSegmentIndex;) {
            // printf("当前段索引 = %llu\n", _segmentIndex);
            segmentNode = reinterpret_cast<WRERTSegmentNode*>(
              WRERT_GET_SEGMENT_POSITION(&currentNode->directoryNode, _segmentIndex));
            sharedBucketOffset = segmentNode->sharedBucketOffset == 0 ? 0 : segmentNode->sharedBucketOffset + 1;
            uint8_t localDepth = segmentNode->localDepth;
            segmentNodeNum = 1ULL << (globalDepth - localDepth);
            segmentNodeIndexStart = _segmentIndex & (~(segmentNodeNum - 1));
            if (_segmentIndex == leftSegmentIndex) {
                if (segmentNode->segmentPtr == rightSegmentNode->segmentPtr) {
                    // printf("左边界，当前段节点与右节点指向同一个段\n");
                    leftSharedBucketPosition = sharedBucketOffset + (leftSharedBucketIndex & WRERT_SHARED_BUCKETS_PER_SEGMENT_MASK);
                    rightSharedBucketPosition = sharedBucketOffset + (rightSharedBucketIndex & WRERT_SHARED_BUCKETS_PER_SEGMENT_MASK);
                    sameSegmentIndex = rightSegmentIndex;
                } else {
                    // printf("左边界，当前段节点与右节点指向不同的段\n");
                    leftSharedBucketPosition = sharedBucketOffset + (leftSharedBucketIndex & WRERT_SHARED_BUCKETS_PER_SEGMENT_MASK);
                    rightSharedBucketPosition = sharedBucketOffset + WRERT_SHARED_BUCKETS_PER_SEGMENT_MASK;
                    sameSegmentIndex = segmentNodeNum + segmentNodeIndexStart - 1;
                }
            } else if (_segmentIndex == rightSegmentIndex) {
                // printf("右边界\n");
                leftSharedBucketPosition = sharedBucketOffset;
                rightSharedBucketPosition = sharedBucketOffset + (rightSharedBucketIndex & WRERT_SHARED_BUCKETS_PER_SEGMENT_MASK);
                sameSegmentIndex = rightSegmentIndex;
            } else {
                if (segmentNode->segmentPtr == rightSegmentNode->segmentPtr) {
                    // printf("中间段，当前段节点与右节点指向同一个段\n");
                    leftSharedBucketPosition = sharedBucketOffset;
                    rightSharedBucketPosition = sharedBucketOffset + (rightSharedBucketIndex & WRERT_SHARED_BUCKETS_PER_SEGMENT_MASK);
                    sameSegmentIndex = rightSegmentIndex;
                } else {
                    // printf("中间段，当前段节点与右节点指向不同的段\n");
                    leftSharedBucketPosition = sharedBucketOffset;
                    rightSharedBucketPosition = sharedBucketOffset + WRERT_SHARED_BUCKETS_PER_SEGMENT_MASK;
                    sameSegmentIndex = segmentNodeNum + segmentNodeIndexStart - 1;
                }
            }
            // printf("相同段索引: %llu\n", sameSegmentIndex);
            segment = reinterpret_cast<WRERTSegment*>(segmentNode->segmentPtr);
            leftMainBucketPosition_1 =
              ((sharedBucketOffset + WRERT_SHARED_BUCKETS_PER_SEGMENT) % WRERT_SEGMENT_SIZE + (leftSharedBucketPosition % WRERT_SHARED_BUCKETS_PER_SEGMENT) * 2) % WRERT_SEGMENT_SIZE;
            rightMainBucketPosition_2 =
              ((sharedBucketOffset + WRERT_SHARED_BUCKETS_PER_SEGMENT) % WRERT_SEGMENT_SIZE + (rightSharedBucketPosition % WRERT_SHARED_BUCKETS_PER_SEGMENT) * 2 + 1) % WRERT_SEGMENT_SIZE;
            // printf("左共享桶位置: %llu, 右共享桶位置: %llu, 开始主桶位置: %llu, 结束主桶位置: %llu\n", leftSharedBucketPosition, rightSharedBucketPosition, leftMainBucketPosition_1, rightMainBucketPosition_2);

            auto bucketRangeIterate = [&](uint64_t start, uint64_t end, int64_t _migVersion) {
                for (uint64_t i = start; i <= end; i++) {
                    for (uint64_t j = 0; j < WRERT_SLOTS_PER_BUCKET; j++) {
                        const auto* slot = &segment->buckets[i].counters[j];
                        // printf("情况3.2：遍历桶 %llu 中的所有 slot %llu:  curSubKey = 0x%08llx, value = %llu, migVersion = %d, _migVersion = %d, validFlag = %llu, globalDepth = %llu, localDepth = %d, leftSegmentIndex = %d\n",
                        //        i, j, slot->subKeyFields.subKey, slot->valueFields.value, slot->subKeyFields.migVersion, _migVersion, slot->subKeyFields.validFlag, globalDepth, localDepth, leftSegmentIndex);
                        if (slot->subKeyFields.validFlag == 0 || slot->subKeyFields.migVersion != _migVersion) {
                            break;
                        }
                        uint64_t curSubKey = slot->subKeyFields.subKey;
                        uint64_t subKeySegmentIndex = WRERT_GET_SEGMENT_NUMBER(curSubKey, WRERT_NODE_SPAN, globalDepth);
                        if (subKeySegmentIndex >= _segmentIndex && subKeySegmentIndex <= sameSegmentIndex) {
                            uint64_t value = slot->valueFields.value;
                            // printf("情况3.2：遍历桶中的所有slot: segmentIndex = %llu, curSubKey = 0x%08llx, value = %llu, migVersion = %llu\n",
                            //        _segmentIndex, curSubKey, value, slot->subKeyFields.migVersion);
                            if (curSubKey > leftSubKey && curSubKey < rightSubKey) {
                                if (currentKeyPosition == WRERT_KEY_MAX_BYTES) {
                                    results.emplace_back(curSubKey + prefix, value);
                                    // printf("有效kv - 情况3.2：遍历桶 %llu 中的所有 slot %llu: leftSegmentIndex = %llu, num = %llu, curSubKey = 0x%08llx, value = %llu, migVersion = %llu, _migVersion = %llu, globalDepth = %d, localDepth = %d\n",
                                    //        i, j, leftSegmentIndex, ++num, curSubKey, value, slot->subKeyFields.migVersion, _migVersion, globalDepth, segmentNode->localDepth);
                                } else {
                                    if (slot->subKeyFields.nodeFlag == 1) {
                                        scanAllNodes((WRERTNode*)value, results, currentKeyPosition, curSubKey + prefix);
                                    } else {
                                        results.push_back(*(WRERTKeyValue*)value);
                                        // printf("有效kv - 情况3.2：遍历桶 %llu 中的所有 slot %llu: leftSegmentIndex = %llu, num = %llu, curSubKey = 0x%08llx, value = %llu, migVersion = %llu, _migVersion = %llu, globalDepth = %d, localDepth = %d\n",
                                        //        i, j, leftSegmentIndex, ++num, curSubKey, (reinterpret_cast<WRERTKeyValue*>(slot->valueFields.value))->value, slot->subKeyFields.migVersion, _migVersion, globalDepth, segmentNode->localDepth);
                                    }
                                }
                            } else if (curSubKey == leftSubKey || curSubKey == rightSubKey) {
                                if (currentKeyPosition == WRERT_KEY_MAX_BYTES) {
                                    results.emplace_back(curSubKey + prefix, value);
                                    // printf("有效kv - 情况3.2：遍历桶 %llu 中的所有 slot %llu: leftSegmentIndex = %llu, num = %llu, curSubKey = 0x%08llx, value = %llu, migVersion = %llu, _migVersion = %llu, globalDepth = %d, localDepth = %d\n",
                                    //        i, j, leftSegmentIndex, ++num, curSubKey, value, slot->subKeyFields.migVersion, _migVersion, globalDepth, segmentNode->localDepth);
                                } else {
                                    if (slot->subKeyFields.nodeFlag == 1) {
                                        nodeScan((WRERTNode*)value, leftKey, rightKey, results, currentKeyPosition, curSubKey + prefix);
                                    } else {
                                        results.push_back(*(WRERTKeyValue*)value);
                                        // printf("有效kv - 情况3.2：遍历桶 %llu 中的所有 slot %llu: leftSegmentIndex = %llu, num = %llu, curSubKey = 0x%08llx, value = %llu, migVersion = %llu, _migVersion = %llu, globalDepth = %d, localDepth = %d\n",
                                        //        i, j, leftSegmentIndex, ++num, curSubKey, (reinterpret_cast<WRERTKeyValue*>(slot->valueFields.value))->value, slot->subKeyFields.migVersion, _migVersion, globalDepth, segmentNode->localDepth);
                                    }
                                }
                            }
                        } else {
                            break;
                        }
                    }
                }
            };
            // auto start_time = std::chrono::high_resolution_clock::now();
            if (WRERT_SEGMENT_SIZE / 2 > leftSharedBucketPosition && WRERT_SEGMENT_SIZE / 2 < rightSharedBucketPosition) {
                // printf("情况3.2：主桶区间1: [%llu, %llu], 共享桶区间: [%llu, %llu], 主桶区间2: [%llu, %llu]\n",
                //        0, rightMainBucketPosition_2, leftSharedBucketPosition, rightSharedBucketPosition, leftMainBucketPosition_1, WRERT_SEGMENT_SIZE - 1);
                // 情况2: 三段区间 [startMainBucketPosition, sharedBucketOffset-1], [sharedBucketOffset + sharedBucketIndexStart, sharedBucketOffset + sharedBucketIndexEnd - 1], [sharedBucketOffset + WRERT_SHARED_BUCKETS_PER_SEGMENT, endMainBucketPosition]
                bucketRangeIterate(0, rightMainBucketPosition_2, localDepth);
                bucketRangeIterate(leftSharedBucketPosition, rightSharedBucketPosition, localDepth - 1);
                bucketRangeIterate(leftMainBucketPosition_1, WRERT_SEGMENT_SIZE - 1, localDepth);
            } else {
                // printf("情况3.2：主桶区间2: [%llu, %llu], 共享桶区间: [%llu, %llu]\n", leftMainBucketPosition_1, rightMainBucketPosition_2, leftSharedBucketPosition, rightSharedBucketPosition);
                // 情况1: 两段区间 [startMainBucketPosition, endMainBucketPosition], [sharedBucketOffset + sharedBucketIndexStart, sharedBucketOffset + sharedBucketIndexEnd - 1]
                bucketRangeIterate(leftMainBucketPosition_1, rightMainBucketPosition_2, localDepth);
                bucketRangeIterate(leftSharedBucketPosition, rightSharedBucketPosition, localDepth - 1);
            }
            // auto end_time = std::chrono::high_resolution_clock::now();
            // range_segment_scan_latency_.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count() - temp_total_time);

            _segmentIndex = segmentNodeIndexStart + segmentNodeNum;
        }
    }
    return;
}

void WRERT::scanAllNodes(WRERTNode* node, std::vector<WRERTKeyValue>& results, int currentKeyPosition, uint64_t prefix) {
    // printf("ScanAllNodes called. currentKeyPosition: %d, prefix: %016llx\n", currentKeyPosition, prefix);
    WRERTNode* currentNode = node;
    if (WRERT_UNLIKELY(currentNode == nullptr)) {
        return;
    }
    if (WRERT_UNLIKELY(currentNode->directoryNode.header.prefixLength > 0)) {
        prefix = (prefix << currentNode->directoryNode.header.prefixLength * WRERT_NODE_SUBKEY_MAX_BYTES * WRERT_SIZE_OF_CHAR);
        for (size_t i = 0; i < currentNode->directoryNode.header.prefixLength * WRERT_NODE_SUBKEY_MAX_BYTES; ++i) {
            prefix += currentNode->directoryNode.header.prefixArray[i] << (currentNode->directoryNode.header.prefixLength * WRERT_NODE_SUBKEY_MAX_BYTES - i);
        }
        currentKeyPosition += currentNode->directoryNode.header.prefixLength * WRERT_NODE_SUBKEY_MAX_BYTES;
    }

    const uint8_t globalDepth = currentNode->directoryNode.directory.globalDepth;
    prefix = (prefix << WRERT_NODE_SPAN);
    currentKeyPosition += WRERT_NODE_SUBKEY_MAX_BYTES;

    WRERTSegment* segment;
    WRERTSegmentNode* segmentNode;

    WRERTSegmentNode* lastSegmentNode = reinterpret_cast<WRERTSegmentNode*>(
      WRERT_GET_SEGMENT_POSITION(&currentNode->directoryNode, currentNode->directoryNode.capacity - 1));
    uint64_t sharedBucketOffset, segmentNodeNum, segmentNodeIndexStart;
    uint64_t lastSegmentIndex = currentNode->directoryNode.capacity - 1;
    for (uint64_t _segmentIndex = 0, sameSegmentIndex = 0; _segmentIndex <= lastSegmentIndex;) {
        // printf("当前段索引 = %llu\n", _segmentIndex);
        segmentNode = reinterpret_cast<WRERTSegmentNode*>(
          WRERT_GET_SEGMENT_POSITION(&currentNode->directoryNode, _segmentIndex));
        sharedBucketOffset = segmentNode->sharedBucketOffset == 0 ? 0 : segmentNode->sharedBucketOffset + 1;
        uint8_t localDepth = segmentNode->localDepth;
        segmentNodeNum = 1ULL << (globalDepth - localDepth);
        segmentNodeIndexStart = _segmentIndex & (~(segmentNodeNum - 1));
        if (_segmentIndex == 0) {
            if (segmentNode->segmentPtr == lastSegmentNode->segmentPtr) {
                // printf("左边界，当前段节点与最后一个段节点指向同一个段\n");
                sameSegmentIndex = lastSegmentIndex;
            } else {
                // printf("左边界，当前段节点与最后一个段节点指向不同的段\n");
                sameSegmentIndex = segmentNodeNum + segmentNodeIndexStart - 1;
            }
        } else if (_segmentIndex == currentNode->directoryNode.capacity - 1) {
            // printf("右边界\n");
            sameSegmentIndex = lastSegmentIndex;
        } else {
            if (segmentNode->segmentPtr == lastSegmentNode->segmentPtr) {
                // printf("中间段，当前段节点与最后一个段节点指向同一个段\n");
                sameSegmentIndex = lastSegmentIndex;
            } else {
                // printf("中间段，当前段节点与最后一个段节点指向不同的段\n");
                sameSegmentIndex = segmentNodeNum + segmentNodeIndexStart - 1;
            }
        }
        // printf("相同段索引: %llu\n", sameSegmentIndex);
        segment = reinterpret_cast<WRERTSegment*>(segmentNode->segmentPtr);

        auto bucketRangeIterate = [&](uint64_t start, uint64_t end, int64_t _migVersion) {
            for (uint64_t _bucketIndex = start; _bucketIndex <= end; _bucketIndex++) {
                for (uint64_t _slotIndex = 0; _slotIndex < WRERT_SLOTS_PER_BUCKET; _slotIndex++) {
                    const auto* slot = &segment->buckets[_bucketIndex].counters[_slotIndex];
                    if (slot->subKeyFields.validFlag == 0 || slot->subKeyFields.migVersion != _migVersion) {
                        break;
                    }
                    uint64_t curSubKey = slot->subKeyFields.subKey;
                    uint64_t subKeySegmentIndex = WRERT_GET_SEGMENT_NUMBER(curSubKey, WRERT_NODE_SPAN, globalDepth);
                    if (subKeySegmentIndex >= _segmentIndex && subKeySegmentIndex <= sameSegmentIndex) {
                        uint64_t value = slot->valueFields.value;
                        // printf("情况3.2：遍历桶中的所有slot: segmentIndex = %llu, curSubKey = 0x%08llx, value = %llu, migVersion = %llu\n", _segmentIndex, curSubKey, value, bucket->counters[j].subKeyFields.migVersion);
                        if (currentKeyPosition == WRERT_KEY_MAX_BYTES) {
                            results.emplace_back(curSubKey + prefix, value);
                        } else {
                            if (slot->subKeyFields.nodeFlag == 1) {
                                scanAllNodes((WRERTNode*)value, results, currentKeyPosition, curSubKey + prefix);
                            } else {
                                results.push_back(*(WRERTKeyValue*)value);
                            }
                        }
                    } else {
                        break;
                    }
                }
            }
        };

        if (sharedBucketOffset == WRERT_SHARED_BUCKETS_PER_SEGMENT) {
            bucketRangeIterate(0, sharedBucketOffset - 1, localDepth);
            bucketRangeIterate(sharedBucketOffset, sharedBucketOffset + WRERT_SHARED_BUCKETS_PER_SEGMENT - 1, localDepth - 1);
            bucketRangeIterate(sharedBucketOffset + WRERT_SHARED_BUCKETS_PER_SEGMENT, WRERT_SEGMENT_SIZE - 1, localDepth);
        } else if (sharedBucketOffset == 0) {
            bucketRangeIterate(0, sharedBucketOffset + WRERT_SHARED_BUCKETS_PER_SEGMENT - 1, localDepth - 1);
            bucketRangeIterate(sharedBucketOffset + WRERT_SHARED_BUCKETS_PER_SEGMENT, WRERT_SEGMENT_SIZE - 1, localDepth);
        } else {
            bucketRangeIterate(0, sharedBucketOffset - 1, localDepth);
            bucketRangeIterate(sharedBucketOffset, WRERT_SEGMENT_SIZE - 1, localDepth - 1);
        }

        _segmentIndex = segmentNodeIndexStart + segmentNodeNum;
    }
}

uint64_t WRERT::memory_profile(WRERTNode* node, int position) {
    // 如果未指定节点，使用根节点
    if (node == nullptr) {
        node = root;
    }

    // 计算当前节点的内存占用：节点本身大小 + 目录节点大小 + 段节点数组大小
    uint64_t totalMemory = sizeof(WRERTNode) + sizeof(WRERTSegmentNode) * node->directoryNode.capacity;

    // 计算全局段的大小（如果存在）
    if (node->directoryNode.directory.globalSegmentPtr != 0) {
        totalMemory += sizeof(WRERTKeyValue) * WRERT_BUCKET_SIZE;
    }

    position += WRERT_NODE_SUBKEY_LENGTH;
    WRERTSegment* lastSegment = nullptr;

    // printf("sizeof(WRERTNode): %llu, sizeof(WRERTSegmentNode): %llu, 当前节点的内存占用 capacity: %llu,totalMemory: %llu\n",
    //        sizeof(WRERTNode), sizeof(WRERTSegmentNode), node->directoryNode.capacity, totalMemory);

    // 遍历所有段节点
    for (uint64_t i = 0; i < node->directoryNode.capacity; i++) {
        // 获取段节点指针
        WRERTSegmentNode* segmentNode = reinterpret_cast<WRERTSegmentNode*>(
          WRERT_GET_SEGMENT_POSITION(&node->directoryNode, i));
        // 获取段指针
        WRERTSegment* segment = reinterpret_cast<WRERTSegment*>(segmentNode->segmentPtr);
        // 跳过重复的段引用
        if (segment == lastSegment) {
            continue;
        } else {
            lastSegment = segment;
            // 计算段中所有桶的大小
            totalMemory += WRERT_SEGMENT_SIZE * sizeof(WRERTBucket);
            // printf("计算段中所有桶的大小 totalMemory: %llu, bucketSize: %llu\n", totalMemory, WRERT_SEGMENT_SIZE * sizeof(WRERTBucket));
        }
        uint64_t sharedBucketOffset = segmentNode->sharedBucketOffset == 0 ? 0 : segmentNode->sharedBucketOffset + 1;
        if (segment != nullptr) {
            // 遍历段中的所有桶
            for (uint32_t j = 0; j < WRERT_SEGMENT_SIZE; j++) {
                // 遍历桶中的所有slot
                for (uint32_t k = 0; k < WRERT_SLOTS_PER_BUCKET; k++) {
                    WRERTSlotKeyValue* slot = &segment->buckets[j].counters[k];
                    if (j >= sharedBucketOffset && j < sharedBucketOffset + WRERT_SHARED_BUCKETS_PER_SEGMENT) {
                        if (isInvalidSharedSlot(slot, segmentNode->localDepth)) {
                            break;
                        }
                        if (position != WRERT_KEY_LENGTH) {
                            // 如果是节点指针，递归计算子节点的内存占用
                            if (slot->subKeyFields.nodeFlag == 0) {
                                totalMemory += sizeof(WRERTKeyValue);
                            } else {
                                totalMemory += memory_profile(reinterpret_cast<WRERTNode*>(slot->valueFields.value), position);
                            }
                        }
                    } else {
                        if (isInvalidMainSlot(slot, segmentNode->localDepth)) {
                            break;
                        }
                        if (position != WRERT_KEY_LENGTH) {
                            // 如果是节点指针，递归计算子节点的内存占用
                            if (slot->subKeyFields.nodeFlag == 0) {
                                totalMemory += sizeof(WRERTKeyValue);
                            } else {
                                totalMemory += memory_profile(reinterpret_cast<WRERTNode*>(slot->valueFields.value), position);
                            }
                        }
                    }
                }
            }
        }
    }

    return totalMemory;
}

WRERT* WRERT::recoveryTree(int position, int nodeHeaderDepth) {
    char* new_base_ptr = reinterpret_cast<char*>(concurrency_myallocator->nvm[0]);
    WRERT* _wrert_ptr = reinterpret_cast<WRERT*>(new_base_ptr);
    uintptr_t old_base = reinterpret_cast<uintptr_t>(_wrert_ptr->_base_ptr);

    _wrert_ptr->_base_ptr = new_base_ptr;

    uintptr_t _offset = reinterpret_cast<uintptr_t>(_wrert_ptr->root) - old_base;
    _wrert_ptr->root = reinterpret_cast<WRERTNode*>(new_base_ptr + _offset);

    if (_wrert_ptr->root->directoryNode.header.prefixLength != 0) {
        position += _wrert_ptr->root->directoryNode.header.prefixLength * WRERT_NODE_SUBKEY_LENGTH;
    }

    position += WRERT_NODE_SUBKEY_LENGTH;
    WRERTSegment* lastSegment = nullptr;

    const uint8_t globalDepth = _wrert_ptr->root->directoryNode.directory.globalDepth;

    uint64_t leaf[2];
    uint64_t leaf_cnt = 0;

    for (uint64_t i = 0; i < _wrert_ptr->root->directoryNode.capacity;) {
        WRERTSegmentNode* segmentNode = reinterpret_cast<WRERTSegmentNode*>(
          WRERT_GET_SEGMENT_POSITION(&_wrert_ptr->root->directoryNode, i));

        uintptr_t segmentNode_offset = reinterpret_cast<uintptr_t>(segmentNode->segmentPtr) - old_base;
        segmentNode->segmentPtr = reinterpret_cast<uint64_t>(new_base_ptr + segmentNode_offset);
        WRERTSegment* segment = reinterpret_cast<WRERTSegment*>(segmentNode->segmentPtr);

        uint64_t stride = 1ULL << (globalDepth - segmentNode->localDepth);
        uint64_t buddyIndexStart = i & (~(stride - 1));
        uint64_t buddyIndex = buddyIndexStart + stride;

        for (uint64_t _buddy = buddyIndex - 1; _buddy > i; _buddy--) {
            WRERTSegmentNode* segmentNodeBuddy = reinterpret_cast<WRERTSegmentNode*>(
              WRERT_GET_SEGMENT_POSITION(&_wrert_ptr->root->directoryNode, _buddy));
            uintptr_t segmentNodeBuddy_offset = reinterpret_cast<uintptr_t>(segmentNodeBuddy->segmentPtr) - old_base;
            segmentNodeBuddy->segmentPtr = reinterpret_cast<uint64_t>(new_base_ptr + segmentNodeBuddy_offset);

            if (segmentNode->localDepth < segmentNodeBuddy->localDepth) {
                memcpy(segmentNodeBuddy, segmentNode, sizeof(WRERTSegmentNode));
            }
        }

        i = buddyIndex;

        uint64_t sharedBucketOffset = segmentNode->sharedBucketOffset == 0 ? 0 : segmentNode->sharedBucketOffset + 1;

        if (segment != nullptr) {
            for (uint32_t j = 0; j < WRERT_SEGMENT_SIZE; j++) {
                for (uint32_t k = 0; k < WRERT_SLOTS_PER_BUCKET; k++) {
                    WRERTSlotKeyValue* slot = &segment->buckets[j].counters[k];
                    if (j >= sharedBucketOffset && j < sharedBucketOffset + WRERT_SHARED_BUCKETS_PER_SEGMENT) {
                        if (isInvalidSharedSlot(slot, segmentNode->localDepth)) {
                            break;
                        }
                        if (position != WRERT_KEY_LENGTH) {
                            if (slot->subKeyFields.nodeFlag == 0) {
                                uintptr_t slot_offset = reinterpret_cast<uintptr_t>(slot->valueFields.value) - old_base;
                                slot->valueFields.value = reinterpret_cast<uint64_t>(new_base_ptr + slot_offset);

                                if (leaf_cnt != 2) {
                                    leaf[leaf_cnt++] = (reinterpret_cast<WRERTKeyValue*>(slot->valueFields.value))->key;
                                }
                            } else {
                                uintptr_t child_offset = reinterpret_cast<uintptr_t>(slot->valueFields.value) - old_base;
                                slot->valueFields.value = reinterpret_cast<uint64_t>(new_base_ptr + child_offset);
                                recoveryAllNodes(reinterpret_cast<WRERTNode*>(slot->valueFields.value),
                                                 new_base_ptr, old_base, position, nodeHeaderDepth + _wrert_ptr->root->directoryNode.header.prefixLength + 1);
                            }
                        }
                    } else {
                        if (isInvalidMainSlot(slot, segmentNode->localDepth)) {
                            break;
                        }
                        if (position != WRERT_KEY_LENGTH) {
                            if (slot->subKeyFields.nodeFlag == 0) {
                                uintptr_t slot_offset = reinterpret_cast<uintptr_t>(slot->valueFields.value) - old_base;
                                slot->valueFields.value = reinterpret_cast<uint64_t>(new_base_ptr + slot_offset);
                                if (leaf_cnt != 2) {
                                    leaf[leaf_cnt++] = (reinterpret_cast<WRERTKeyValue*>(slot->valueFields.value))->key;
                                }
                            } else {
                                uintptr_t child_offset = reinterpret_cast<uintptr_t>(slot->valueFields.value) - old_base;
                                slot->valueFields.value = reinterpret_cast<uint64_t>(new_base_ptr + child_offset);
                                recoveryAllNodes(reinterpret_cast<WRERTNode*>(slot->valueFields.value),
                                                 new_base_ptr, old_base, position, nodeHeaderDepth + _wrert_ptr->root->directoryNode.header.prefixLength + 1);
                            }
                        }
                    }
                }
            }
        }
    }

    if (_wrert_ptr->root->directoryNode.header.nodeHeaderDepth != nodeHeaderDepth && leaf_cnt == 2) {
        int prefixCommonLength = _wrert_ptr->root->directoryNode.header.computeCommonPrefix(leaf[0], leaf[1], nodeHeaderDepth * WRERT_NODE_SUBKEY_MAX_BYTES);
        _wrert_ptr->root->directoryNode.header.nodeHeaderDepth = nodeHeaderDepth;
        _wrert_ptr->root->directoryNode.header.prefixLength = prefixCommonLength / WRERT_NODE_SUBKEY_MAX_BYTES;
        if (_wrert_ptr->root->directoryNode.header.prefixLength > 0) {
            _wrert_ptr->root->directoryNode.header.assignPrefix(leaf[1], nodeHeaderDepth * WRERT_NODE_SUBKEY_MAX_BYTES * WRERT_SIZE_OF_CHAR);
        }
        clflush((char*)&_wrert_ptr->root->directoryNode.header, sizeof(WRERTHeader));
    }

    return _wrert_ptr;
}

void WRERT::recoveryAllNodes(WRERTNode* node, char* new_base_ptr, uintptr_t old_base, int position, int nodeHeaderDepth) {
    if (node->directoryNode.header.prefixLength != 0) {
        position += node->directoryNode.header.prefixLength * WRERT_NODE_SUBKEY_LENGTH;
    }

    position += WRERT_NODE_SUBKEY_LENGTH;
    WRERTSegment* lastSegment = nullptr;

    const uint8_t globalDepth = node->directoryNode.directory.globalDepth;

    uint64_t leaf[2];
    uint64_t leaf_cnt = 0;

    for (uint64_t i = 0; i < node->directoryNode.capacity; i++) {
        WRERTSegmentNode* segmentNode = reinterpret_cast<WRERTSegmentNode*>(
          WRERT_GET_SEGMENT_POSITION(&node->directoryNode, i));
        uintptr_t segmentNode_offset = reinterpret_cast<uintptr_t>(segmentNode->segmentPtr) - old_base;

        segmentNode->segmentPtr = reinterpret_cast<uint64_t>(new_base_ptr + segmentNode_offset);
        WRERTSegment* segment = reinterpret_cast<WRERTSegment*>(segmentNode->segmentPtr);

        uint64_t stride = 1ULL << (globalDepth - segmentNode->localDepth);
        uint64_t buddyIndexStart = i & (~(stride - 1));
        uint64_t buddyIndex = buddyIndexStart + stride;

        for (uint64_t _buddy = buddyIndex - 1; _buddy > i; _buddy--) {
            WRERTSegmentNode* segmentNodeBuddy = reinterpret_cast<WRERTSegmentNode*>(
              WRERT_GET_SEGMENT_POSITION(&node->directoryNode, _buddy));
            uintptr_t segmentNodeBuddy_offset = reinterpret_cast<uintptr_t>(segmentNodeBuddy->segmentPtr) - old_base;
            segmentNodeBuddy->segmentPtr = reinterpret_cast<uint64_t>(new_base_ptr + segmentNodeBuddy_offset);
            if (segmentNode->localDepth < segmentNodeBuddy->localDepth) {
                memcpy(segmentNodeBuddy, segmentNode, sizeof(WRERTSegmentNode));
            }
        }

        i = buddyIndex;

        uint64_t sharedBucketOffset = segmentNode->sharedBucketOffset == 0 ? 0 : segmentNode->sharedBucketOffset + 1;

        if (segment != nullptr) {
            for (uint32_t j = 0; j < WRERT_SEGMENT_SIZE; j++) {
                for (uint32_t k = 0; k < WRERT_SLOTS_PER_BUCKET; k++) {
                    WRERTSlotKeyValue* slot = &segment->buckets[j].counters[k];

                    if (j >= sharedBucketOffset && j < sharedBucketOffset + WRERT_SHARED_BUCKETS_PER_SEGMENT) {
                        if (isInvalidSharedSlot(slot, segmentNode->localDepth)) {
                            break;
                        }
                        if (position != WRERT_KEY_LENGTH) {
                            if (slot->subKeyFields.nodeFlag == 0) {
                                uintptr_t slot_offset = reinterpret_cast<uintptr_t>(slot->valueFields.value) - old_base;
                                slot->valueFields.value = reinterpret_cast<uint64_t>(new_base_ptr + slot_offset);
                                if (leaf_cnt != 2) {
                                    leaf[leaf_cnt++] = (reinterpret_cast<WRERTKeyValue*>(slot->valueFields.value))->key;
                                }
                            } else {
                                uintptr_t child_offset = reinterpret_cast<uintptr_t>(slot->valueFields.value) - old_base;
                                slot->valueFields.value = reinterpret_cast<uint64_t>(new_base_ptr + child_offset);
                                recoveryAllNodes(reinterpret_cast<WRERTNode*>(slot->valueFields.value),
                                                 new_base_ptr, old_base, position, nodeHeaderDepth + node->directoryNode.header.prefixLength + 1);
                            }
                        }
                    } else {
                        if (isInvalidMainSlot(slot, segmentNode->localDepth)) {
                            break;
                        }
                        if (position != WRERT_KEY_LENGTH) {
                            if (slot->subKeyFields.nodeFlag == 0) {
                                uintptr_t slot_offset = reinterpret_cast<uintptr_t>(slot->valueFields.value) - old_base;
                                slot->valueFields.value = reinterpret_cast<uint64_t>(new_base_ptr + slot_offset);
                                if (leaf_cnt != 2) {
                                    leaf[leaf_cnt++] = (reinterpret_cast<WRERTKeyValue*>(slot->valueFields.value))->key;
                                }
                            } else {
                                uintptr_t child_offset = reinterpret_cast<uintptr_t>(slot->valueFields.value) - old_base;
                                slot->valueFields.value = reinterpret_cast<uint64_t>(new_base_ptr + child_offset);
                                recoveryAllNodes(reinterpret_cast<WRERTNode*>(slot->valueFields.value),
                                                 new_base_ptr, old_base, position, nodeHeaderDepth + node->directoryNode.header.prefixLength + 1);
                            }
                        }
                    }
                }
            }
        }
    }

    if (node->directoryNode.header.nodeHeaderDepth != nodeHeaderDepth && leaf_cnt == 2) {
        int prefixCommonLength = node->directoryNode.header.computeCommonPrefix(leaf[0], leaf[1], nodeHeaderDepth * WRERT_NODE_SUBKEY_MAX_BYTES);
        node->directoryNode.header.nodeHeaderDepth = nodeHeaderDepth;
        node->directoryNode.header.prefixLength = prefixCommonLength / WRERT_NODE_SUBKEY_MAX_BYTES;
        if (node->directoryNode.header.prefixLength > 0) {
            node->directoryNode.header.assignPrefix(leaf[1], nodeHeaderDepth * WRERT_NODE_SUBKEY_MAX_BYTES * WRERT_SIZE_OF_CHAR);
        }
        clflush((char*)&node->directoryNode.header, sizeof(WRERTHeader));
    }
}

WRERT::WRERT() {
    root = NewWRERTNode();

    // 持久化 WRERTNode* root 根节点指针;
    // clflush((char*)root, sizeof(WRERTNode*));
}

WRERT::WRERT(uint32_t globalDepthTree) : globalDepthTree(globalDepthTree) {
    WRERT::initialize();
}

WRERT::~WRERT() {
    delete root;
}

void WRERT::initialize() {
    // 创建根节点
    root = NewWRERTNode();
}

WRERT* NewWRERT() {
    WRERT* _new_wrert = static_cast<WRERT*>(concurrency_fast_alloc(sizeof(WRERT)));
    // printf("sizeof(WRERT): %llu\n", sizeof(WRERT));
    _new_wrert->initialize();
    _new_wrert->_base_ptr = reinterpret_cast<char*>(_new_wrert);

    // _new_wrert->globalDepthTree = 1;

    return _new_wrert;
}

} // namespace wrert
