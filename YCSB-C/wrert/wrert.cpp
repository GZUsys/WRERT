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
restart:
    WRERTNode* currentNode = node ? node : root;
    locks().directory(currentNode).read_lock();

    if (node == nullptr && currentNode != root) {
        locks().directory(currentNode).read_unlock();
        goto restart;
    }

    uint32_t currentKeyPosition = keyLength;
    uint64_t beforeAddress = (uint64_t)&root;
    uint8_t nodeHeaderDepth = currentNode->directoryNode.header.nodeHeaderDepth;
    uint32_t arrayMatchLength = 0;
    uint8_t nodeFlag;
    uint16_t fingerprint = 0;
    int prefixMatchLength;

    while (currentKeyPosition < WRERT_KEY_MAX_BYTES) {
        prefixMatchLength = currentNode->directoryNode.header.computePrefix(key, currentKeyPosition << 2, &arrayMatchLength);
        if (prefixMatchLength == -1) {
            locks().directory(currentNode).read_unlock();
            locks().directory(currentNode).write_lock();
            if (currentNode->directoryNode.header.prefixLength != WRERT_NODE_PREFIX_MAX_BYTES) {
                locks().directory(currentNode).write_unlock();
                goto restart;
            }
            currentNode->directoryNode.header.prefixLength = WRERT_NODE_PREFIX_MAX_BYTES / WRERT_NODE_SUBKEY_MAX_BYTES;
            currentNode->directoryNode.header.assignPrefix(key, currentKeyPosition * WRERT_SIZE_OF_CHAR);
            locks().directory(currentNode).write_unlock();
            locks().directory(currentNode).read_lock();
            continue;
        } else if (currentKeyPosition + arrayMatchLength == WRERT_KEY_MAX_BYTES) {
            WRERTKeyValue* globalSegment = reinterpret_cast<WRERTKeyValue*>(currentNode->directoryNode.directory.globalSegmentPtr);
            locks().directory(currentNode).read_unlock();
            locks().directory(currentNode).write_lock();
            currentNode->putDirNode(globalSegment, key, value);
            locks().directory(currentNode).write_unlock();
            return OperationResults::Success;
        } else if (currentNode->directoryNode.header.prefixLength == prefixMatchLength) {
            currentKeyPosition += currentNode->directoryNode.header.prefixLength << 2;
            const uint64_t subKey = WRERT_GET_SUBKEY(key, (currentKeyPosition << 3), WRERT_NODE_SPAN);
            const uint8_t globalDepth = currentNode->directoryNode.directory.globalDepth;
            const uint64_t segmentIndex = WRERT_GET_SEGMENT_NUMBER(subKey, WRERT_NODE_SPAN, globalDepth);
            WRERTSegmentNode* segmentNode = reinterpret_cast<WRERTSegmentNode*>(WRERT_GET_SEGMENT_POSITION(&currentNode->directoryNode, segmentIndex));
            WRERTSegment* segment = reinterpret_cast<WRERTSegment*>(segmentNode->segmentPtr);

            locks().segment(segment).read_lock();

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

            locks().bucket(sharedBucket).read_lock();

            uint64_t shareSlotIndex, mainSlotIndex;
            // 0 shared bucket，1 main bucket，-1 not found
            int8_t _isShareOrMainFlag = 0;
            WRERTSlotKeyValue* slot;
            uint64_t _value = 0;
            WRERTBucket* targetBucket = nullptr;
            bool _isValidSharedSlot = false, _isValidMainSlot = false, _isFindFlag = false, needSMO = false;

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
                            _value = slot->value;
                            break;
                        } else {
                            _isValidSharedSlot = true;
                            shareSlotIndex += __builtin_ctz(bad_slot_mask);
                            slot = &sharedBucket->counters[shareSlotIndex];
                            _value = slot->value;
                            break;
                        }
                    }
                    if (bad_slot_mask) {
                        _isValidSharedSlot = true;
                        shareSlotIndex += __builtin_ctz(bad_slot_mask);
                        slot = &sharedBucket->counters[shareSlotIndex];
                        _value = slot->value;
                        break;
                    }
                } else {
                    _isValidSharedSlot = true;
                    slot = &sharedBucket->counters[shareSlotIndex];
                    _value = slot->value;
                    break;
                }
            }

            locks().bucket(sharedBucket).read_unlock();

            if (!_isFindFlag) {
                locks().bucket(mainBucket).read_lock();

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
                                _value = slot->value;
                                break;
                            } else {
                                _isValidMainSlot = true;
                                mainSlotIndex += __builtin_ctz(bad_slot_mask);
                                slot = &mainBucket->counters[mainSlotIndex];
                                _value = slot->value;
                                break;
                            }
                        }
                        if (bad_slot_mask) {
                            _isValidMainSlot = true;
                            mainSlotIndex += __builtin_ctz(bad_slot_mask);
                            slot = &mainBucket->counters[mainSlotIndex];
                            _value = slot->value;
                            break;
                        }
                    } else {
                        _isValidMainSlot = true;
                        slot = &mainBucket->counters[mainSlotIndex];
                        _value = slot->value;
                        break;
                    }
                }

                locks().bucket(mainBucket).read_unlock();

                if (!_isFindFlag) {
                    if (_isValidSharedSlot) {
                        slot = &sharedBucket->counters[shareSlotIndex];
                        _value = slot->value;
                    } else if (_isValidMainSlot) {
                        _isShareOrMainFlag = 1;
                    } else {
                        slot = nullptr;
                        _isShareOrMainFlag = -1;
                    }
                }
            }

            currentKeyPosition += WRERT_NODE_SUBKEY_MAX_BYTES;

            if (_isShareOrMainFlag != -1) {
                targetBucket = _isShareOrMainFlag ? mainBucket : sharedBucket;
            }

            if (currentKeyPosition == WRERT_KEY_MAX_BYTES) {
                if (!_isFindFlag || slot == nullptr) {
                    // printf("情况2.1.1: 到达键的末尾，新键值对，直接插入\n");
                    if (slot != nullptr) {
                        locks().bucket(targetBucket).write_lock();
                        needSMO = false;
                        if (slot->subKeyFields.validFlag == 1
                            && slot->subKeyFields.migVersion == localDepth - !_isShareOrMainFlag
                            && slot->subKeyFields.subKey != subKey) {
                            locks().bucket(targetBucket).write_unlock();
                            locks().segment(segment).read_unlock();
                            locks().directory(currentNode).read_unlock();
                            goto restart;
                        }
                    } else {
                        locks().segment(segment).read_unlock();
                        locks().directory(currentNode).read_unlock();
                        locks().directory(currentNode).write_lock();
                        locks().segment(segment).write_lock();
                        needSMO = true;
                        if (localDepth != segmentNode->localDepth
                                || nodeHeaderDepth != currentNode->directoryNode.header.nodeHeaderDepth
                                || nodeHeaderDepth != 0 ?
                              currentNode != reinterpret_cast<WRERTNode*>(reinterpret_cast<WRERTSlotKeyValue*>(beforeAddress)->valueFields.value) :
                              currentNode != *reinterpret_cast<WRERTNode**>(beforeAddress)) {
                            locks().segment(segment).write_unlock();
                            locks().directory(currentNode).write_unlock();
                            goto restart;
                        }
                    }

                    currentNode->put(
                      subKey, value, segmentNode, segment, slot,
                      segmentIndex, sharedBucketOffset, fingerprint, _isShareOrMainFlag, nodeFlag = 0, &beforeAddress);

                    if (needSMO) {
                        locks().segment(segment).write_unlock();
                        locks().directory(currentNode).write_unlock();
                    } else {
                        locks().bucket(targetBucket).write_unlock();
                        locks().segment(segment).read_unlock();
                        locks().directory(currentNode).read_unlock();
                    }

                    return OperationResults::Success;
                } else {
                    // printf("情况2.1.2: 到达键的末尾，已有键值对，更新值\n");
                    // locks().bucket(targetBucket).write_lock();
                    // if (slot->subKeyFields.validFlag == 1
                    //     && slot->subKeyFields.migVersion == localDepth - !_isShareOrMainFlag
                    //     && slot->subKeyFields.subKey != subKey
                    //     && slot->value != _value) {
                    //     locks().bucket(targetBucket).write_unlock();
                    //     locks().segment(segment).read_unlock();
                    //     locks().directory(currentNode).read_unlock();
                    //     global_cas_retry_count1.fetch_add(1, std::memory_order_relaxed);
                    //     goto restart;
                    // }

                    if (!__atomic_compare_exchange_n(&slot->value, &_value, value, false,
                                                     __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
                        locks().segment(segment).read_unlock();
                        locks().directory(currentNode).read_unlock();
                        // global_cas_retry_count1.fetch_add(1, std::memory_order_relaxed);
                        goto restart;
                    }

                    // slot->valueFields.value = value;
                    clflush((char*)&slot->valueFields, sizeof(slot->valueFields));
                    // locks().bucket(targetBucket).write_unlock();
                    locks().segment(segment).read_unlock();
                    locks().directory(currentNode).read_unlock();
                    return OperationResults::Success;
                }
            } else {
                if (!_isFindFlag || slot == nullptr) {
                    // printf("情况2.2.1: 还未到达键的末尾，新键值对，直接插入\n");
                    if (slot != nullptr) {
                        locks().bucket(targetBucket).write_lock();
                        needSMO = false;
                        if (slot->subKeyFields.validFlag == 1
                            && slot->subKeyFields.migVersion == localDepth - !_isShareOrMainFlag
                            && slot->subKeyFields.subKey != subKey) {
                            // printf("情况2.2.1: 重试直接插入\n");
                            locks().bucket(targetBucket).write_unlock();
                            locks().segment(segment).read_unlock();
                            locks().directory(currentNode).read_unlock();
                            goto restart;
                        }
                    } else {
                        locks().segment(segment).read_unlock();
                        locks().directory(currentNode).read_unlock();
                        locks().directory(currentNode).write_lock();
                        locks().segment(segment).write_lock();
                        needSMO = true;
                        if (segmentNode->localDepth != localDepth
                                || nodeHeaderDepth != currentNode->directoryNode.header.nodeHeaderDepth
                                || nodeHeaderDepth != 0 ?
                              currentNode != reinterpret_cast<WRERTNode*>(reinterpret_cast<WRERTSlotKeyValue*>(beforeAddress)->valueFields.value) :
                              currentNode != *reinterpret_cast<WRERTNode**>(beforeAddress)) {
                            locks().segment(segment).write_unlock();
                            locks().directory(currentNode).write_unlock();
                            goto restart;
                        }
                    }

                    WRERTKeyValue* kv = _kv;
                    if (kv == nullptr) {
                        kv = NewWRERTKeyValue(key, value);
                        clflush((char*)kv, sizeof(WRERTKeyValue));
                    }
                    currentNode->put(
                      subKey, (uint64_t)kv, segmentNode, segment, slot,
                      segmentIndex, sharedBucketOffset, fingerprint, _isShareOrMainFlag, nodeFlag = 0, &beforeAddress);

                    if (needSMO) {
                        locks().segment(segment).write_unlock();
                        locks().directory(currentNode).write_unlock();
                    } else {
                        locks().bucket(targetBucket).write_unlock();
                        locks().segment(segment).read_unlock();
                        locks().directory(currentNode).read_unlock();
                    }

                    return OperationResults::Success;
                } else {
                    if (slot->subKeyFields.nodeFlag == 1) {
                        // printf("情况2.2.2: 还未到达键的末尾，指向下一个节点，继续递归\n");
                        WRERTNode* subNode = (WRERTNode*)slot->valueFields.value;

                        locks().directory(subNode).read_lock();
                        if (subNode != (WRERTNode*)slot->valueFields.value) {
                            locks().directory(subNode).read_unlock();
                            locks().segment(segment).read_unlock();
                            locks().directory(currentNode).read_unlock();
                            goto restart;
                        }

                        locks().segment(segment).read_unlock();
                        locks().directory(currentNode).read_unlock();

                        currentNode = subNode;
                        nodeHeaderDepth = currentNode->directoryNode.header.nodeHeaderDepth;
                        beforeAddress = (uint64_t)slot;
                    } else {
                        WRERTKeyValue* kv = (WRERTKeyValue*)slot->valueFields.value;
                        if (kv->key == key) {
                            // printf("情况2.2.3: 还未到达键的末尾，已有键值对，更新值\n");
                            // locks().bucket(targetBucket).write_lock();
                            // if (slot->subKeyFields.validFlag == 1
                            //     && slot->subKeyFields.migVersion == localDepth - !_isShareOrMainFlag
                            //     && slot->subKeyFields.subKey != subKey
                            //     && slot->value != _value) {
                            //     locks().bucket(targetBucket).write_unlock();
                            //     locks().segment(segment).read_unlock();
                            //     locks().directory(currentNode).read_unlock();
                            //     global_cas_retry_count2.fetch_add(1, std::memory_order_relaxed);
                            //     goto restart;
                            // }
                            __atomic_store_n(&kv->value, value, __ATOMIC_RELAXED);
                            // kv->value = value;
                            clflush((char*)&kv->value, sizeof(kv->value));
                            // locks().bucket(targetBucket).write_unlock();
                            locks().segment(segment).read_unlock();
                            locks().directory(currentNode).read_unlock();
                            return OperationResults::Success;
                        } else {
                            // printf("情况2.2.4: 还未到达键的末尾，键不同，创建新节点\n");
                            locks().bucket(targetBucket).write_lock();
                            needSMO = true;
                            if (slot->subKeyFields.validFlag == 1
                                && slot->subKeyFields.migVersion == localDepth - !_isShareOrMainFlag
                                && slot->subKeyFields.subKey != subKey) {
                                locks().bucket(targetBucket).write_unlock();
                                locks().segment(segment).read_unlock();
                                locks().directory(currentNode).read_unlock();
                                goto restart;
                            }

                            WRERTNode* newNode = NewWRERTNode();
                            newNode->directoryNode.header.initialize(
                              &currentNode->directoryNode.header, 0, currentKeyPosition / WRERT_NODE_SUBKEY_MAX_BYTES);

                            // insert(kv->key, kv->value, newNode, currentKeyPosition, kv);
                            // insert(key, value, newNode, currentKeyPosition, _kv);
                            insertNewNode(kv->key, kv->value, newNode, currentKeyPosition, kv);
                            insertNewNode(key, value, newNode, currentKeyPosition, _kv);

                            clflush((char*)newNode, sizeof(WRERTNode));

                            slot->subKeyFields.nodeFlag = 1;
                            slot->valueFields.value = (uint64_t)newNode;

                            clflush((char*)slot, sizeof(WRERTSlotKeyValue));

                            locks().bucket(targetBucket).write_unlock();
                            locks().segment(segment).read_unlock();
                            locks().directory(currentNode).read_unlock();
                            return OperationResults::Success;
                        }
                    }
                }
            }
        } else if (currentNode->directoryNode.header.prefixLength != 0
                   && prefixMatchLength < currentNode->directoryNode.header.prefixLength) {
            locks().directory(currentNode).read_unlock();
            locks().directory(currentNode).write_lock();
            if (prefixMatchLength >= currentNode->directoryNode.header.prefixLength) {
                locks().directory(currentNode).write_unlock();
                goto restart;
            }

            WRERTNode* newNode = NewWRERTNode();
            newNode->directoryNode.header.initialize(
              &currentNode->directoryNode.header, prefixMatchLength, currentNode->directoryNode.header.nodeHeaderDepth);
            if (currentNode->directoryNode.directory.globalSegmentPtr != 0) {
                WRERTKeyValue* newGlobalSegment = reinterpret_cast<WRERTKeyValue*>(newNode->directoryNode.directory.globalSegmentPtr);
                WRERTKeyValue* globalSegment = reinterpret_cast<WRERTKeyValue*>(currentNode->directoryNode.directory.globalSegmentPtr);
                for (int j = currentNode->directoryNode.header.prefixLength - prefixMatchLength; j <= currentNode->directoryNode.header.prefixLength; j++) {
                    newGlobalSegment[j - currentNode->directoryNode.header.prefixLength + prefixMatchLength] = globalSegment[j];
                }
            }

            uint64_t subKey = WRERT_GET_SUBKEY(key, (currentKeyPosition + prefixMatchLength) * WRERT_SIZE_OF_CHAR, WRERT_NODE_SPAN);
            WRERTKeyValue* kv = NewWRERTKeyValue(key, value);

            clflush((char*)kv, sizeof(WRERTKeyValue));

            newNode->putSegNode(subKey, (uint64_t)kv, fingerprint, &beforeAddress, nodeFlag = 0);
            newNode->putSegNode(WRERT_GET_PREFIX_ARRAY_BITS(currentNode->directoryNode.header.prefixArray,
                                                            prefixMatchLength * WRERT_NODE_SUBKEY_MAX_BYTES,
                                                            WRERT_NODE_SUBKEY_MAX_BYTES),
                                (uint64_t)currentNode, fingerprint, &beforeAddress, nodeFlag = 1);

            currentNode->directoryNode.header.prefixLength -=
              (prefixMatchLength + newNode->directoryNode.header.nodeHeaderDepth + 1);
            currentNode->directoryNode.header.nodeHeaderDepth =
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

            clflush((char*)newNode, sizeof(WRERTNode));
            clflush((char*)&(currentNode->directoryNode.header), sizeof(WRERTHeader));

            if (newNode->directoryNode.header.nodeHeaderDepth != 0) {
                ((WRERTSlotKeyValue*)beforeAddress)->valueFields.value = (uint64_t)newNode;
                clflush((char*)(WRERTSlotKeyValue*)beforeAddress, sizeof(WRERTSlotKeyValue));
            } else {
                *(WRERTNode**)beforeAddress = newNode;
                clflush((char*)beforeAddress, sizeof(WRERTNode*));
            }

            locks().directory(currentNode).write_unlock();
            return OperationResults::Success;
        }
    }
    locks().directory(currentNode).read_unlock();
    return OperationResults::Failed;
}

OperationResults WRERT::insertNewNode(uint64_t key, uint64_t value, WRERTNode* node, int keyLength, WRERTKeyValue* _kv) {
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
        prefixMatchLength = currentNode->directoryNode.header.computePrefix(key, currentKeyPosition << 2, &arrayMatchLength);
        if (prefixMatchLength == -1) {
            currentNode->directoryNode.header.prefixLength = WRERT_NODE_PREFIX_MAX_BYTES / WRERT_NODE_SUBKEY_MAX_BYTES;
            currentNode->directoryNode.header.assignPrefix(key, currentKeyPosition * WRERT_SIZE_OF_CHAR);
            continue;
        } else if (currentKeyPosition + arrayMatchLength == WRERT_KEY_MAX_BYTES) {
            WRERTKeyValue* globalSegment = reinterpret_cast<WRERTKeyValue*>(currentNode->directoryNode.directory.globalSegmentPtr);
            currentNode->putDirNode(globalSegment, key, value);
            return OperationResults::Success;
        } else if (currentNode->directoryNode.header.prefixLength == prefixMatchLength) {
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
            _mm_prefetch((const char*)mainBucket, _MM_HINT_T0);
            _mm_prefetch((const char*)sharedBucket, _MM_HINT_T0);

            uint64_t shareSlotIndex, mainSlotIndex;
            // 0 shared bucket，1 main bucket
            int8_t _isShareOrMainFlag = 0;
            WRERTSlotKeyValue* slot;
            bool _isValidSharedSlot = false, _isValidMainSlot = false, _isFindFlag = false;

            const __m512i idx = _mm512_set_epi64(14, 12, 10, 8, 6, 4, 2, 0);
            const __m512i v_subKey_target = _mm512_set1_epi64(subKey);
            const __m512i v_shared_validFlag_migVersion_target = _mm512_set1_epi64((1ULL << 41) | ((uint64_t)(sharedBucketVersion & WRERT_MIGVERSION_BITS) << 32));
            const __m512i v_validFlag_migVersion_mask = _mm512_set1_epi64(WRERT_VALIDFLAG_MIGVERSION_MASK);
            const __m512i v_subKey_mask = _mm512_set1_epi64(WRERT_SUBKEY_MASK);
            for (shareSlotIndex = 0; shareSlotIndex < WRERT_SLOTS_PER_BUCKET; shareSlotIndex += 8) {
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
                const __m512i v_main_validFlag_migVersion_target = _mm512_set1_epi64((1ULL << 41) | ((uint64_t)localDepth << 32));
                for (mainSlotIndex = 0; mainSlotIndex < WRERT_SLOTS_PER_BUCKET; mainSlotIndex += 8) {
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
                        break;
                    }
                }

                if (!_isFindFlag) {
                    if (_isValidSharedSlot) {
                        slot = &sharedBucket->counters[shareSlotIndex];
                    } else if (_isValidMainSlot) {
                        _isShareOrMainFlag = 1;
                    } else {
                        slot = nullptr;
                    }
                }
            }

            currentKeyPosition += WRERT_NODE_SUBKEY_MAX_BYTES;

            if (currentKeyPosition == WRERT_KEY_MAX_BYTES) {
                if (!_isFindFlag || slot == nullptr) {
                    currentNode->put(
                      subKey, value, segmentNode, segment, slot,
                      segmentIndex, sharedBucketOffset, fingerprint, _isShareOrMainFlag, nodeFlag = 0, &beforeAddress);

                    return OperationResults::Success;
                } else {
                    slot->valueFields.value = value;
                    clflush((char*)&slot->valueFields, sizeof(slot->valueFields));
                    return OperationResults::Success;
                }
            } else {
                if (!_isFindFlag || slot == nullptr) {
                    WRERTKeyValue* kv = _kv;
                    if (kv == nullptr) {
                        kv = NewWRERTKeyValue(key, value);
                        clflush((char*)kv, sizeof(WRERTKeyValue));
                    }

                    currentNode->put(
                      subKey, (uint64_t)kv, segmentNode, segment, slot,
                      segmentIndex, sharedBucketOffset, fingerprint, _isShareOrMainFlag, nodeFlag = 0, &beforeAddress);

                    return OperationResults::Success;
                } else {
                    if (slot->subKeyFields.nodeFlag == 1) {
                        currentNode = (WRERTNode*)slot->valueFields.value;
                        beforeAddress = (uint64_t)slot;
                    } else {
                        WRERTKeyValue* kv = (WRERTKeyValue*)slot->valueFields.value;
                        if (kv->key == key) {
                            kv->value = value;
                            clflush((char*)&kv->value, sizeof(kv->value));
                            return OperationResults::Success;
                        } else {
                            WRERTNode* newNode = NewWRERTNode();

                            newNode->directoryNode.header.initialize(
                              &currentNode->directoryNode.header, 0, currentKeyPosition / WRERT_NODE_SUBKEY_MAX_BYTES);

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
            WRERTNode* newNode = NewWRERTNode();
            newNode->directoryNode.header.initialize(
              &currentNode->directoryNode.header, prefixMatchLength, currentNode->directoryNode.header.nodeHeaderDepth);
            if (currentNode->directoryNode.directory.globalSegmentPtr != 0) {
                WRERTKeyValue* newGlobalSegment = reinterpret_cast<WRERTKeyValue*>(newNode->directoryNode.directory.globalSegmentPtr);
                WRERTKeyValue* globalSegment = reinterpret_cast<WRERTKeyValue*>(currentNode->directoryNode.directory.globalSegmentPtr);
                for (int j = currentNode->directoryNode.header.prefixLength - prefixMatchLength; j <= currentNode->directoryNode.header.prefixLength; j++) {
                    newGlobalSegment[j - currentNode->directoryNode.header.prefixLength + prefixMatchLength] = globalSegment[j];
                }
            }
            uint64_t subKey = WRERT_GET_SUBKEY(key, (currentKeyPosition + prefixMatchLength) * WRERT_SIZE_OF_CHAR, WRERT_NODE_SPAN);
            WRERTKeyValue* kv = NewWRERTKeyValue(key, value);
            clflush((char*)kv, sizeof(WRERTKeyValue));
            newNode->putSegNode(subKey, (uint64_t)kv, fingerprint, &beforeAddress, nodeFlag = 0);
            newNode->putSegNode(WRERT_GET_PREFIX_ARRAY_BITS(currentNode->directoryNode.header.prefixArray,
                                                            prefixMatchLength * WRERT_NODE_SUBKEY_MAX_BYTES,
                                                            WRERT_NODE_SUBKEY_MAX_BYTES),
                                (uint64_t)currentNode, fingerprint, &beforeAddress, nodeFlag = 1);

            currentNode->directoryNode.header.prefixLength -=
              (prefixMatchLength + newNode->directoryNode.header.nodeHeaderDepth + 1);
            currentNode->directoryNode.header.nodeHeaderDepth =
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
restart:
    WRERTNode* currentNode = node ? node : root;
    locks().directory(currentNode).read_lock();
    if (WRERT_UNLIKELY(currentNode == nullptr)) {
        locks().directory(currentNode).read_unlock();
        return (uint64_t)OperationResults::NotFound;
    }
    if (node == nullptr && currentNode != root) {
        locks().directory(currentNode).read_unlock();
        goto restart;
    }
    uint32_t currentKeyPosition = keyLength;
    uint32_t arrayMatchLength;
    uint64_t result = -1;
    int prefixMatchLength;
    while (currentKeyPosition < WRERT_KEY_MAX_BYTES) {
        if (WRERT_UNLIKELY(currentNode->directoryNode.header.prefixLength > 0)) {
            prefixMatchLength = currentNode->directoryNode.header.computePrefix(key, currentKeyPosition * WRERT_SIZE_OF_CHAR, &arrayMatchLength);
            if (arrayMatchLength != 0 && currentKeyPosition + arrayMatchLength <= currentNode->directoryNode.header.prefixLength * WRERT_NODE_SUBKEY_MAX_BYTES) {
                WRERTKeyValue* globalSegment = reinterpret_cast<WRERTKeyValue*>(currentNode->directoryNode.directory.globalSegmentPtr);
                result = currentNode->getDirNode(globalSegment, key);
                locks().directory(currentNode).read_unlock();
                return result;
            } else if (currentNode->directoryNode.header.prefixLength == prefixMatchLength) {
                currentKeyPosition += currentNode->directoryNode.header.prefixLength * WRERT_NODE_SUBKEY_MAX_BYTES;
            } else {
                locks().directory(currentNode).read_unlock();
                return (uint64_t)OperationResults::Failed;
            }
        }
        uint64_t subKey = WRERT_GET_SUBKEY(key, (currentKeyPosition << 3), WRERT_NODE_SPAN);
        const uint8_t globalDepth = currentNode->directoryNode.directory.globalDepth;
        const uint64_t segmentIndex = WRERT_GET_SEGMENT_NUMBER(subKey, WRERT_NODE_SPAN, globalDepth);
        WRERTSegmentNode* segmentNode = reinterpret_cast<WRERTSegmentNode*>(
          WRERT_GET_SEGMENT_POSITION(&currentNode->directoryNode, segmentIndex));
        WRERTSegment* segment = reinterpret_cast<WRERTSegment*>(segmentNode->segmentPtr);

        locks().segment(segment).read_lock();

        const uint8_t sharedBucketOffset = segmentNode->sharedBucketOffset == 0 ? 0 : segmentNode->sharedBucketOffset + 1;
        const uint8_t localDepth = segmentNode->localDepth;
        const int8_t sharedBucketVersion = localDepth - 1;
        const uint64_t sharedBucketIndex = WRERT_GET_SHARED_BUCKET_INDEX(subKey, localDepth, WRERT_MAIN_BUCKETS_MAX_BITS);
        const uint64_t sharedBucketPosition = sharedBucketOffset + (sharedBucketIndex & WRERT_SHARED_BUCKETS_PER_SEGMENT_MASK);
        WRERTBucket* sharedBucket = &segment->buckets[sharedBucketPosition];
        const uint64_t mainBucketIndex = WRERT_GET_MAIN_BUCKET_INDEX(subKey, localDepth, WRERT_MAIN_BUCKETS_MAX_BITS);
        const uint64_t mainBucketPosition = calcBucketPosition(sharedBucketOffset, mainBucketIndex);
        WRERTBucket* mainBucket = &segment->buckets[mainBucketPosition];
        _mm_prefetch((const char*)sharedBucket, _MM_HINT_T0);
        _mm_prefetch((const char*)mainBucket, _MM_HINT_T0);

        locks().bucket(sharedBucket).read_lock();

        WRERTSlotKeyValue* slot = nullptr;
        uint8_t _nodeFlag = -1;
        bool isFindFlag = false;

        // const __m512i v_offsets = _mm512_load_si512((__m512i*)offsets);
        const __m512i idx = _mm512_set_epi64(14, 12, 10, 8, 6, 4, 2, 0);
        const __m512i v_42bits_mask = _mm512_set1_epi64(WRERT_VALIDFLAG_MIGVERSION_SUBKEY_MASK);
        const __m512i v_shared_target = _mm512_set1_epi64((1ULL << 41) | ((uint64_t)(sharedBucketVersion & WRERT_MIGVERSION_BITS) << 32) | subKey);
        for (uint64_t slotIndex = 0; slotIndex < WRERT_SLOTS_PER_BUCKET; slotIndex += 8) {
            // __m512i keys_shared = _mm512_mask_i64gather_epi64(_mm512_setzero_si512(), summary_mask, v_offsets, (void*)&sharedBucket->counters[slotIndex], 1);
            __m512i shared_data0 = _mm512_load_si512((const __m512i*)&sharedBucket->counters[slotIndex]);
            __m512i shared_data1 = _mm512_load_si512((const __m512i*)&sharedBucket->counters[slotIndex + 4]);
            __m512i keys_shared = _mm512_permutex2var_epi64(shared_data0, idx, shared_data1);
            __m512i v_keys_shared = _mm512_and_si512(keys_shared, v_42bits_mask);
            __mmask8 match_mask_shared = _mm512_cmpeq_epi64_mask(v_keys_shared, v_shared_target);
            if (match_mask_shared) {
                int j = __builtin_ctz(match_mask_shared);
                slot = &sharedBucket->counters[slotIndex + j];
                result = slot->valueFields.value;
                _nodeFlag = slot->subKeyFields.nodeFlag;
                isFindFlag = true;
                break;
            }
        }

        locks().bucket(sharedBucket).read_unlock();

        if (!isFindFlag) {
            locks().bucket(mainBucket).read_lock();

            const __m512i v_main_target = _mm512_set1_epi64((1ULL << 41) | ((uint64_t)localDepth << 32) | subKey);
            for (uint64_t slotIndex = 0; slotIndex < WRERT_SLOTS_PER_BUCKET; slotIndex += 8) {
                // __m512i keys_main = _mm512_mask_i64gather_epi64(_mm512_setzero_si512(), summary_mask, v_offsets, (void*)&mainBucket->counters[slotIndex], 1);
                __m512i main_data0 = _mm512_load_si512((const __m512i*)&mainBucket->counters[slotIndex]);
                __m512i main_data1 = _mm512_load_si512((const __m512i*)&mainBucket->counters[slotIndex + 4]);
                __m512i keys_main = _mm512_permutex2var_epi64(main_data0, idx, main_data1);
                __m512i v_keys_main = _mm512_and_si512(keys_main, v_42bits_mask);
                __mmask8 match_mask_main = _mm512_cmpeq_epi64_mask(v_keys_main, v_main_target);
                if (match_mask_main) {
                    int j = __builtin_ctz(match_mask_main);
                    slot = &mainBucket->counters[slotIndex + j];
                    result = slot->valueFields.value;
                    _nodeFlag = slot->subKeyFields.nodeFlag;
                    isFindFlag = true;
                    break;
                }
            }

            locks().bucket(mainBucket).read_unlock();
        }

        currentKeyPosition +=
          currentNode->directoryNode.header.prefixLength == 0 ? WRERT_NODE_SUBKEY_MAX_BYTES : currentNode->directoryNode.header.prefixLength << 2;

        if (slot == nullptr) {
            locks().segment(segment).read_unlock();
            locks().directory(currentNode).read_unlock();
            return (uint64_t)OperationResults::Failed;
        }
        if (_nodeFlag != slot->subKeyFields.nodeFlag) {
            locks().segment(segment).read_unlock();
            locks().directory(currentNode).read_unlock();
            goto restart;
        } else if (_nodeFlag == 0) {
            if (currentKeyPosition == WRERT_KEY_MAX_BYTES) {
                if (result != slot->valueFields.value) {
                    locks().segment(segment).read_unlock();
                    locks().directory(currentNode).read_unlock();
                    goto restart;
                }
                locks().segment(segment).read_unlock();
                locks().directory(currentNode).read_unlock();
                return result;
            } else {
                if (key == (reinterpret_cast<WRERTKeyValue*>(slot->valueFields.value))->key) {
                    if (result != slot->valueFields.value) {
                        locks().segment(segment).read_unlock();
                        locks().directory(currentNode).read_unlock();
                        goto restart;
                    }
                    result = (reinterpret_cast<WRERTKeyValue*>(slot->valueFields.value))->value;
                    locks().segment(segment).read_unlock();
                    locks().directory(currentNode).read_unlock();
                    return result;
                } else {
                    locks().segment(segment).read_unlock();
                    locks().directory(currentNode).read_unlock();
                    return (uint64_t)OperationResults::Failed;
                }
            }
        } else {
            WRERTNode* subNode = reinterpret_cast<WRERTNode*>(slot->valueFields.value);

            locks().directory(subNode).read_lock();
            if (subNode != reinterpret_cast<WRERTNode*>(slot->valueFields.value)) {
                locks().directory(subNode).read_unlock();
                locks().segment(segment).read_unlock();
                locks().directory(currentNode).read_unlock();
                goto restart;
            }

            locks().segment(segment).read_unlock();
            locks().directory(currentNode).read_unlock();

            currentNode = subNode;
        }
    }
    locks().directory(currentNode).read_unlock();
    return (uint64_t)OperationResults::Failed;
}

std::vector<WRERTKeyValue> WRERT::lookupRange(uint64_t leftKey, uint64_t rightKey) {
    std::vector<WRERTKeyValue> results;

    nodeScan(root, leftKey, rightKey, results, 0, 0);

    // std::sort(results.begin(), results.end(), [](const WRERTKeyValue& a, const WRERTKeyValue& b) {
    //     return a.value < b.value;
    // });
    // for (const auto& kv : results) {
    //     printf("key: %llu, value: %llu\n", kv.key, kv.value);
    // }
    // for (uint64_t i = 0, j = 0; i < 10000; i++) {
    //     if (results[j].value != i) {
    //         printf("value: %llu\n", i);
    //     } else {
    //         j++;
    //     }
    // }

    return results;
}

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
        return;
    }

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

        // 循环检查右边界的前缀，如果大于前缀数组，则返回失败
        for (size_t i = 0; i < currentNode->directoryNode.header.prefixLength * WRERT_NODE_SUBKEY_MAX_BYTES; i++) {
            // 提取子键：从左键中提取当前位置的子键
            subKey = WRERT_GET_SUBKEY(rightKey, (currentKeyPosition + i) * WRERT_SIZE_OF_CHAR, WRERT_SIZE_OF_CHAR);

            // 如果子键与前缀数组匹配，继续检查下一个子键
            if (subKey == currentNode->directoryNode.header.prefixArray[i]) {
                continue;
            }

            // 如果子键大于前缀数组，说明查询范围的右边包含当前节点的范围，右边界为最后一个段索引
            else if (subKey > currentNode->directoryNode.header.prefixArray[i]) {
                rightSegmentIndex = currentNode->directoryNode.capacity - 1;
                break;
            }

            // 如果子键小于前缀数组，说明查询范围的右边界不属于当前节点的范围，直接返回
            else {
                return;
            }
        }

        // 将当前前缀左移，为新的前缀字节腾出空间
        prefix = (prefix << currentNode->directoryNode.header.prefixLength * WRERT_NODE_SUBKEY_MAX_BYTES * WRERT_SIZE_OF_CHAR);

        // 将节点前缀数组的字节按大端序添加到前缀值中
        for (size_t i = 0; i < currentNode->directoryNode.header.prefixLength * WRERT_NODE_SUBKEY_MAX_BYTES; ++i) {
            prefix += currentNode->directoryNode.header.prefixArray[i] << (currentNode->directoryNode.header.prefixLength * WRERT_NODE_SUBKEY_MAX_BYTES - i);
        }

        // 更新当前位置：跳过已处理的前缀长度（字节）
        currentKeyPosition += currentNode->directoryNode.header.prefixLength * WRERT_NODE_SUBKEY_MAX_BYTES;
    }

    const uint8_t globalDepth = currentNode->directoryNode.directory.globalDepth;

    uint64_t leftSubKey = UINT64_MAX, rightSubKey = UINT64_MAX;
    if (WRERT_LIKELY(leftSegmentIndex == UINT64_MAX)) {
        leftSubKey = WRERT_GET_SUBKEY(leftKey, (currentKeyPosition << 3), WRERT_NODE_SPAN);
        leftSegmentIndex = WRERT_GET_SEGMENT_NUMBER(leftSubKey, WRERT_NODE_SPAN, globalDepth);
    }

    if (WRERT_LIKELY(rightSegmentIndex == UINT64_MAX)) {
        rightSubKey = WRERT_GET_SUBKEY(rightKey, (currentKeyPosition << 3), WRERT_NODE_SPAN);
        rightSegmentIndex = WRERT_GET_SEGMENT_NUMBER(rightSubKey, WRERT_NODE_SPAN, globalDepth);
    }

    prefix = (prefix << WRERT_NODE_SPAN);
    currentKeyPosition += WRERT_NODE_SUBKEY_MAX_BYTES;

    if (leftSegmentIndex == rightSegmentIndex) {
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
                    if (slot->subKeyFields.validFlag == 0 || slot->subKeyFields.migVersion != _migVersion) {
                        break;
                    }
                    uint64_t curSubKey = slot->subKeyFields.subKey;
                    if (WRERT_GET_SEGMENT_NUMBER(curSubKey, WRERT_NODE_SPAN, globalDepth) == leftSegmentIndex) {
                        uint64_t value = slot->valueFields.value;
                        if (curSubKey > leftSubKey && curSubKey < rightSubKey) {
                            if (currentKeyPosition == WRERT_KEY_MAX_BYTES) {
                                results.emplace_back(curSubKey + prefix, value);
                            } else {
                                if (slot->subKeyFields.nodeFlag == 1) {
                                    scanAllNodes((WRERTNode*)value, results, currentKeyPosition, curSubKey + prefix);

                                } else {
                                    results.push_back(*(WRERTKeyValue*)value);
                                }
                            }
                        } else if (curSubKey == leftSubKey || curSubKey == rightSubKey) {
                            if (currentKeyPosition == WRERT_KEY_MAX_BYTES) {
                                results.emplace_back(curSubKey + prefix, value);
                            } else {
                                if (slot->subKeyFields.nodeFlag == 1) {
                                    nodeScan((WRERTNode*)value, leftKey, rightKey, results, currentKeyPosition, curSubKey + prefix);
                                } else {
                                    results.push_back(*(WRERTKeyValue*)value);
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
            segmentNode = reinterpret_cast<WRERTSegmentNode*>(
              WRERT_GET_SEGMENT_POSITION(&currentNode->directoryNode, _segmentIndex));
            sharedBucketOffset = segmentNode->sharedBucketOffset == 0 ? 0 : segmentNode->sharedBucketOffset + 1;
            uint8_t localDepth = segmentNode->localDepth;
            segmentNodeNum = 1ULL << (globalDepth - localDepth);
            segmentNodeIndexStart = _segmentIndex & (~(segmentNodeNum - 1));
            if (_segmentIndex == leftSegmentIndex) {
                if (segmentNode->segmentPtr == rightSegmentNode->segmentPtr) {
                    leftSharedBucketPosition = sharedBucketOffset + (leftSharedBucketIndex & WRERT_SHARED_BUCKETS_PER_SEGMENT_MASK);
                    rightSharedBucketPosition = sharedBucketOffset + (rightSharedBucketIndex & WRERT_SHARED_BUCKETS_PER_SEGMENT_MASK);
                    sameSegmentIndex = rightSegmentIndex;
                } else {
                    leftSharedBucketPosition = sharedBucketOffset + (leftSharedBucketIndex & WRERT_SHARED_BUCKETS_PER_SEGMENT_MASK);
                    rightSharedBucketPosition = sharedBucketOffset + WRERT_SHARED_BUCKETS_PER_SEGMENT_MASK;
                    sameSegmentIndex = segmentNodeNum + segmentNodeIndexStart - 1;
                }
            } else if (_segmentIndex == rightSegmentIndex) {
                leftSharedBucketPosition = sharedBucketOffset;
                rightSharedBucketPosition = sharedBucketOffset + (rightSharedBucketIndex & WRERT_SHARED_BUCKETS_PER_SEGMENT_MASK);
                sameSegmentIndex = rightSegmentIndex;
            } else {
                if (segmentNode->segmentPtr == rightSegmentNode->segmentPtr) {
                    leftSharedBucketPosition = sharedBucketOffset;
                    rightSharedBucketPosition = sharedBucketOffset + (rightSharedBucketIndex & WRERT_SHARED_BUCKETS_PER_SEGMENT_MASK);
                    sameSegmentIndex = rightSegmentIndex;
                } else {
                    leftSharedBucketPosition = sharedBucketOffset;
                    rightSharedBucketPosition = sharedBucketOffset + WRERT_SHARED_BUCKETS_PER_SEGMENT_MASK;
                    sameSegmentIndex = segmentNodeNum + segmentNodeIndexStart - 1;
                }
            }
            segment = reinterpret_cast<WRERTSegment*>(segmentNode->segmentPtr);
            leftMainBucketPosition_1 =
              ((sharedBucketOffset + WRERT_SHARED_BUCKETS_PER_SEGMENT) % WRERT_SEGMENT_SIZE + (leftSharedBucketPosition % WRERT_SHARED_BUCKETS_PER_SEGMENT) * 2) % WRERT_SEGMENT_SIZE;
            rightMainBucketPosition_2 =
              ((sharedBucketOffset + WRERT_SHARED_BUCKETS_PER_SEGMENT) % WRERT_SEGMENT_SIZE + (rightSharedBucketPosition % WRERT_SHARED_BUCKETS_PER_SEGMENT) * 2 + 1) % WRERT_SEGMENT_SIZE;

            auto bucketRangeIterate = [&](uint64_t start, uint64_t end, int64_t _migVersion) {
                for (uint64_t i = start; i <= end; i++) {
                    for (uint64_t j = 0; j < WRERT_SLOTS_PER_BUCKET; j++) {
                        const auto* slot = &segment->buckets[i].counters[j];

                        if (slot->subKeyFields.validFlag == 0 || slot->subKeyFields.migVersion != _migVersion) {
                            break;
                        }
                        uint64_t curSubKey = slot->subKeyFields.subKey;
                        uint64_t subKeySegmentIndex = WRERT_GET_SEGMENT_NUMBER(curSubKey, WRERT_NODE_SPAN, globalDepth);
                        if (subKeySegmentIndex >= _segmentIndex && subKeySegmentIndex <= sameSegmentIndex) {
                            // 获取当前值
                            uint64_t value = slot->valueFields.value;
                            if (curSubKey > leftSubKey && curSubKey < rightSubKey) {
                                if (currentKeyPosition == WRERT_KEY_MAX_BYTES) {
                                    results.emplace_back(curSubKey + prefix, value);
                                } else {
                                    if (slot->subKeyFields.nodeFlag == 1) {
                                        scanAllNodes((WRERTNode*)value, results, currentKeyPosition, curSubKey + prefix);
                                    } else {
                                        results.push_back(*(WRERTKeyValue*)value);
                                    }
                                }
                            } else if (curSubKey == leftSubKey || curSubKey == rightSubKey) {
                                if (currentKeyPosition == WRERT_KEY_MAX_BYTES) {
                                    results.emplace_back(curSubKey + prefix, value);
                                } else {
                                    if (slot->subKeyFields.nodeFlag == 1) {
                                        nodeScan((WRERTNode*)value, leftKey, rightKey, results, currentKeyPosition, curSubKey + prefix);
                                    } else {
                                        results.push_back(*(WRERTKeyValue*)value);
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
                bucketRangeIterate(leftMainBucketPosition_1, rightMainBucketPosition_2, localDepth);
                bucketRangeIterate(leftSharedBucketPosition, rightSharedBucketPosition, localDepth - 1);
            }
            _segmentIndex = segmentNodeIndexStart + segmentNodeNum;
        }
    }
    return;
}

void WRERT::scanAllNodes(WRERTNode* node, std::vector<WRERTKeyValue>& results, int currentKeyPosition, uint64_t prefix) {
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
        segmentNode = reinterpret_cast<WRERTSegmentNode*>(
          WRERT_GET_SEGMENT_POSITION(&currentNode->directoryNode, _segmentIndex));
        sharedBucketOffset = segmentNode->sharedBucketOffset == 0 ? 0 : segmentNode->sharedBucketOffset + 1;
        uint8_t localDepth = segmentNode->localDepth;
        segmentNodeNum = 1ULL << (globalDepth - localDepth);
        segmentNodeIndexStart = _segmentIndex & (~(segmentNodeNum - 1));
        if (_segmentIndex == 0) {
            if (segmentNode->segmentPtr == lastSegmentNode->segmentPtr) {
                sameSegmentIndex = lastSegmentIndex;
            } else {
                sameSegmentIndex = segmentNodeNum + segmentNodeIndexStart - 1;
            }
        } else if (_segmentIndex == currentNode->directoryNode.capacity - 1) {
            sameSegmentIndex = lastSegmentIndex;
        } else {
            if (segmentNode->segmentPtr == lastSegmentNode->segmentPtr) {
                sameSegmentIndex = lastSegmentIndex;
            } else {
                sameSegmentIndex = segmentNodeNum + segmentNodeIndexStart - 1;
            }
        }
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
    if (node == nullptr) {
        node = root;
    }
    uint64_t totalMemory = sizeof(WRERTNode) + sizeof(WRERTSegmentNode) * node->directoryNode.capacity;
    if (node->directoryNode.directory.globalSegmentPtr != 0) {
        totalMemory += sizeof(WRERTKeyValue) * WRERT_BUCKET_SIZE;
    }
    position += WRERT_NODE_SUBKEY_LENGTH;
    WRERTSegment* lastSegment = nullptr;

    for (uint64_t i = 0; i < node->directoryNode.capacity; i++) {
        WRERTSegmentNode* segmentNode = reinterpret_cast<WRERTSegmentNode*>(
          WRERT_GET_SEGMENT_POSITION(&node->directoryNode, i));
        WRERTSegment* segment = reinterpret_cast<WRERTSegment*>(segmentNode->segmentPtr);
        if (segment == lastSegment) {
            continue;
        } else {
            lastSegment = segment;
            totalMemory += WRERT_SEGMENT_SIZE * sizeof(WRERTBucket);
        }
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
}

WRERT::WRERT(uint32_t globalDepthTree) : globalDepthTree(globalDepthTree) {
    WRERT::initialize();
}

WRERT::~WRERT() {
    delete root;
}

void WRERT::initialize() {
    root = NewWRERTNode();
}

WRERT* NewWRERT() {
    WRERT* _new_wrert = static_cast<WRERT*>(concurrency_fast_alloc(sizeof(WRERT)));
    _new_wrert->initialize();
    _new_wrert->_base_ptr = reinterpret_cast<char*>(_new_wrert);

    return _new_wrert;
}

} // namespace wrert
