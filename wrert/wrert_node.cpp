#include <iostream>
#include <cstring>
#include <atomic>
#include <chrono>
#include <iostream>

#include "wrert_node.h"

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

thread_local WRERTBucket WRERTNode::migration_buffer_pool[WRERT_MIG_SHARED_BUCKET_NUM];

bool WRERTBucket::isFull(uint64_t segmentMSB, uint8_t localDepth) {
    const uint64_t lastSlotSubKey = this->counters[WRERT_SLOTS_PER_BUCKET - 1].subKeyFields.subKey;
    const uint64_t subKeyMSB = WRERT_GET_SEGMENT_NUMBER(lastSlotSubKey, WRERT_NODE_SPAN, localDepth);

    if (this->counters[WRERT_SLOTS_PER_BUCKET - 1].subKeyFields.validFlag != 1) {
        return false;
    } else if (subKeyMSB != segmentMSB) {
        return false;
    }

    return true;
}

int WRERTHeader::computePrefix(uint64_t key, int startPosition, uint32_t* arrayMatchLength) {
    if (this->prefixLength == 6) {
        return -1;
    }

    if (this->prefixLength == 0) {
        return 0;
    }

    const uint64_t subKey = WRERT_GET_SUBKEY(key, startPosition, (this->prefixLength * WRERT_NODE_SUBKEY_MAX_BYTES * WRERT_SIZE_OF_CHAR));
    // const uint64_t subKey = WRERT_GET_SUBKEY(key, startPosition, (this->prefixLength << 5));

    const uint64_t shiftedSubKey = subKey << (64 - this->prefixLength * WRERT_NODE_SUBKEY_MAX_BYTES * WRERT_SIZE_OF_CHAR);
    // const uint64_t shiftedSubKey = subKey << (64 - this->prefixLength << 5);

    int prefixMatchLength = 0;

    for (int i = 0; i < this->prefixLength; i++) {
        int flag = 0;

        for (int j = i * WRERT_NODE_SUBKEY_MAX_BYTES; j < (i + 1) * WRERT_NODE_SUBKEY_MAX_BYTES && j < WRERT_NODE_PREFIX_MAX_BYTES; j++) {
            const unsigned char byteFromKey = (unsigned char)((shiftedSubKey >> (56 - j * 8)) & 0xFF);
            if (byteFromKey != prefixArray[j]) {
                break;
            }

            arrayMatchLength++;
            flag++;
        }

        if (flag == WRERT_NODE_SUBKEY_MAX_BYTES) {
            prefixMatchLength++;
        }
    }

    return prefixMatchLength;
}

int WRERTHeader::computeCommonPrefix(uint64_t key1, uint64_t key2, int startPosition) {
    int remaining_bits = (WRERT_KEY_MAX_BYTES - startPosition) * WRERT_SIZE_OF_CHAR;
    uint64_t mask = (remaining_bits >= 64) ? 0xFFFFFFFFFFFFFFFFULL : (1ULL << remaining_bits) - 1;
    uint64_t masked_key1 = key1 & mask;
    uint64_t masked_key2 = key2 & mask;

    int commonPrefix = (__builtin_clzll(masked_key1 ^ masked_key2) / WRERT_SIZE_OF_CHAR) - startPosition;
    return commonPrefix;
}

void WRERTHeader::assignPrefix(uint64_t key, int startPosition) {
    uint64_t subkey = WRERT_GET_SUBKEY(key, startPosition, (this->prefixLength * WRERT_NODE_SUBKEY_MAX_BYTES * WRERT_SIZE_OF_CHAR));
    // uint64_t subkey = WRERT_GET_SUBKEY(key, startPosition, (this->prefixLength << 5));
    subkey <<= (WRERT_NODE_PREFIX_MAX_BITS - this->prefixLength * WRERT_NODE_SUBKEY_MAX_BYTES * WRERT_SIZE_OF_CHAR);
    // subkey <<= (WRERT_NODE_PREFIX_MAX_BITS - this->prefixLength << 5);

    for (int i = WRERT_NODE_PREFIX_MAX_BYTES - 1; i >= 0; i--) {
        prefixArray[i] = (unsigned char)(subkey & 0xFF);
        subkey >>= 8;
    }
}

void WRERTHeader::assignPrefix(const unsigned char* key, unsigned char assignedLength) {
    if (WRERT_UNLIKELY(assignedLength == WRERT_INIT_PREFIX_LENGTH)) {
        return;
    }
    const int maxBytes = assignedLength * WRERT_NODE_SUBKEY_MAX_BYTES;
    // for (int i = 0; assignedLength != WRERT_INIT_PREFIX_LENGTH && i < assignedLength * WRERT_NODE_SUBKEY_MAX_BYTES; i++) {
    for (int i = 0; i < maxBytes && i < WRERT_NODE_PREFIX_MAX_BYTES; i++) {
        prefixArray[i] = key[i];
    }
}

OperationResults WRERTNode::putDirNode(WRERTKeyValue* globalSegment, uint64_t key, uint64_t value) {
    for (uint64_t i = 0; i < WRERT_BUCKET_SIZE; i++) {
        if (globalSegment[i].key == 0 && globalSegment[i].value == -1) {
            globalSegment[i].key = key;
            globalSegment[i].value = value;
            clflush((char*)globalSegment + i * sizeof(WRERTKeyValue), sizeof(WRERTKeyValue));
            return OperationResults::Success;
        }
    }
    return OperationResults::Failed;
}

uint64_t WRERTNode::getDirNode(WRERTKeyValue* globalSegment, uint64_t key) {
    for (uint64_t i = 0; i < WRERT_BUCKET_SIZE; i++) {
        if (globalSegment[i].key == key) {
            return globalSegment[i].value;
        }
    }

    return (uint64_t)OperationResults::Failed;
}

OperationResults WRERTNode::putSegNode(uint64_t subKey, uint64_t value, uint16_t _fingerprint, uint64_t* beforeAddress, uint8_t _nodeFlag) {
    const uint8_t globalDepth = this->directoryNode.directory.globalDepth;
    const uint64_t segmentIndex = WRERT_GET_SEGMENT_NUMBER(subKey, WRERT_NODE_SPAN, globalDepth);
    WRERTSegmentNode* segmentNode = reinterpret_cast<WRERTSegmentNode*>(
      WRERT_GET_SEGMENT_POSITION(&this->directoryNode, segmentIndex));
    WRERTSegment* segment = reinterpret_cast<WRERTSegment*>(segmentNode->segmentPtr);

    const uint8_t sharedBucketOffset = segmentNode->sharedBucketOffset == 0 ? 0 : segmentNode->sharedBucketOffset + 1;
    const uint8_t localDepth = segmentNode->localDepth;
    const uint8_t sharedBucketVersion = localDepth - 1;

    const uint64_t sharedBucketIndex = WRERT_GET_SHARED_BUCKET_INDEX(subKey, localDepth, WRERT_MAIN_BUCKETS_MAX_BITS);
    const uint64_t mainBucketIndex = WRERT_GET_MAIN_BUCKET_INDEX(subKey, localDepth, WRERT_MAIN_BUCKETS_MAX_BITS);
    const uint64_t sharedBucketPosition = sharedBucketOffset + (sharedBucketIndex & WRERT_SHARED_BUCKETS_PER_SEGMENT_MASK);
    const uint64_t mainBucketPosition = calcBucketPosition(sharedBucketOffset, mainBucketIndex);
    WRERTBucket* sharedBucket = &segment->buckets[sharedBucketPosition];
    WRERTBucket* mainBucket = &segment->buckets[mainBucketPosition];
    _mm_prefetch((const char*)sharedBucket, _MM_HINT_T0);
    _mm_prefetch((const char*)mainBucket, _MM_HINT_T0);

    uint64_t shareSlotIndex, mainSlotIndex, currentSharedBucketIndex, currentMainBucketIndex;
    // 0 shared bucket，1 main bucket，-1 not found
    int _isShareOrMainFlag = 0;
    WRERTSlotKeyValue* slot;

    // for (shareSlotIndex = 0; shareSlotIndex < WRERT_SLOTS_PER_BUCKET; ++shareSlotIndex) {
    //     slot = &sharedBucket->counters[shareSlotIndex];
    //     if ((uint8_t)slot->subKeyFields.migVersion != sharedBucketVersion) {
    //         return this->put(
    //           subKey, value, segmentNode, segment, slot,
    //           segmentIndex, sharedBucketOffset, _fingerprint, _isShareOrMainFlag = 0, _nodeFlag, beforeAddress);
    //     }
    // }

    // const __m512i v_offsets = _mm512_load_si512((__m512i*)offsets);
    const __m512i idx = _mm512_set_epi64(14, 12, 10, 8, 6, 4, 2, 0);
    const __m512i v_target1 = _mm512_set1_epi64(sharedBucketVersion);
    const __m512i v_migVersion_mask = _mm512_set1_epi64(WRERT_MIGVERSION_MASK);
    for (uint64_t shareSlotIndex = 0; shareSlotIndex < WRERT_SLOTS_PER_BUCKET; shareSlotIndex += 8) {
        // __m512i keys = _mm512_mask_i64gather_epi64(_mm512_setzero_si512(), summary_mask, v_offsets, (void*)&sharedBucket->counters[shareSlotIndex], 1);
        __m512i shared_data0 = _mm512_load_si512((const __m512i*)&sharedBucket->counters[shareSlotIndex]);
        __m512i shared_data1 = _mm512_load_si512((const __m512i*)&sharedBucket->counters[shareSlotIndex + 4]);
        __m512i keys = _mm512_permutex2var_epi64(shared_data0, idx, shared_data1);
        __m512i v_migVersion = _mm512_and_si512(keys, v_migVersion_mask);
        v_migVersion = _mm512_srli_epi64(v_migVersion, 32);
        __mmask8 match_mask = _mm512_cmpneq_epi64_mask(v_migVersion, v_target1);
        if (match_mask) {
            int j = __builtin_ctz(match_mask);
            return this->put(
              subKey, value, segmentNode, segment, &sharedBucket->counters[shareSlotIndex + j],
              segmentIndex, sharedBucketOffset, _fingerprint, _isShareOrMainFlag = 0, _nodeFlag, beforeAddress);
        }
    }

    // for (mainSlotIndex = 0; mainSlotIndex < WRERT_SLOTS_PER_BUCKET; ++mainSlotIndex) {
    //     slot = &mainBucket->counters[mainSlotIndex];
    //     if (slot->subKeyFields.migVersion != localDepth) {
    //         return this->put(
    //           subKey, value, segmentNode, segment, slot,
    //           segmentIndex, sharedBucketOffset, _fingerprint, _isShareOrMainFlag = 1, _nodeFlag, beforeAddress);
    //     }
    // }

    const __m512i v_target2 = _mm512_set1_epi64(localDepth);
    for (uint64_t mainSlotIndex = 0; mainSlotIndex < WRERT_SLOTS_PER_BUCKET; mainSlotIndex += 8) {
        // __m512i keys = _mm512_mask_i64gather_epi64(_mm512_setzero_si512(), summary_mask, v_offsets, (void*)&mainBucket->counters[mainSlotIndex], 1);
        __m512i main_data0 = _mm512_load_si512((const __m512i*)&mainBucket->counters[mainSlotIndex]);
        __m512i main_data1 = _mm512_load_si512((const __m512i*)&mainBucket->counters[mainSlotIndex + 4]);
        __m512i keys = _mm512_permutex2var_epi64(main_data0, idx, main_data1);
        __m512i v_migVersion = _mm512_and_si512(keys, v_migVersion_mask);
        v_migVersion = _mm512_srli_epi64(v_migVersion, 32);
        __mmask8 match_mask = _mm512_cmpneq_epi64_mask(v_migVersion, v_target2);
        if (match_mask) {
            int j = __builtin_ctz(match_mask);
            return this->put(
              subKey, value, segmentNode, segment, &mainBucket->counters[mainSlotIndex + j],
              segmentIndex, sharedBucketOffset, _fingerprint, _isShareOrMainFlag = 1, _nodeFlag, beforeAddress);
        }
    }

    return this->put(
      subKey, value, segmentNode, segment, slot = nullptr,
      segmentIndex, sharedBucketOffset, _fingerprint, _isShareOrMainFlag, _nodeFlag, beforeAddress);
}

WRERTSlotKeyValue* WRERTNode::getSegNode(uint64_t subKey) {
    const uint8_t globalDepth = this->directoryNode.directory.globalDepth;
    const uint64_t segmentIndex = WRERT_GET_SEGMENT_NUMBER(subKey, WRERT_NODE_SPAN, globalDepth);
    WRERTSegmentNode* segmentNode = reinterpret_cast<WRERTSegmentNode*>(
      WRERT_GET_SEGMENT_POSITION(&this->directoryNode, segmentIndex));
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
    WRERTSlotKeyValue* slot = nullptr;
    // const __m512i v_offsets = _mm512_load_si512((__m512i*)offsets);
    const __m512i idx = _mm512_set_epi64(14, 12, 10, 8, 6, 4, 2, 0);
    const __m512i v_42bits_mask = _mm512_set1_epi64(WRERT_VALIDFLAG_MIGVERSION_SUBKEY_MASK);
    const __m512i v_shared_target = _mm512_set1_epi64((1ULL << 41) | ((uint64_t)(sharedBucketVersion & WRERT_MIGVERSION_BITS) << 32) | subKey);
    const __m512i v_main_target = _mm512_set1_epi64((1ULL << 41) | ((uint64_t)localDepth << 32) | subKey);
    for (uint64_t slotIndex = 0; slotIndex < WRERT_SLOTS_PER_BUCKET; slotIndex += 8) {
        // __m512i keys_shared = _mm512_mask_i64gather_epi64(_mm512_setzero_si512(), summary_mask, v_offsets, (void*)&sharedBucket->counters[slotIndex], 1);
        // __m512i keys_main = _mm512_mask_i64gather_epi64(_mm512_setzero_si512(), summary_mask, v_offsets, (void*)&mainBucket->counters[slotIndex], 1);
        __m512i shared_data0 = _mm512_load_si512((const __m512i*)&sharedBucket->counters[slotIndex]);
        __m512i shared_data1 = _mm512_load_si512((const __m512i*)&sharedBucket->counters[slotIndex + 4]);
        __m512i main_data0 = _mm512_load_si512((const __m512i*)&mainBucket->counters[slotIndex]);
        __m512i main_data1 = _mm512_load_si512((const __m512i*)&mainBucket->counters[slotIndex + 4]);
        __m512i keys_shared = _mm512_permutex2var_epi64(shared_data0, idx, shared_data1);
        __m512i keys_main = _mm512_permutex2var_epi64(main_data0, idx, main_data1);
        __m512i v_keys_shared = _mm512_and_si512(keys_shared, v_42bits_mask);
        __m512i v_keys_main = _mm512_and_si512(keys_main, v_42bits_mask);
        __mmask8 match_mask_shared = _mm512_cmpeq_epi64_mask(v_keys_shared, v_shared_target);
        __mmask8 match_mask_main = _mm512_cmpeq_epi64_mask(v_keys_main, v_main_target);
        if (match_mask_shared) {
            int j = __builtin_ctz(match_mask_shared);
            return &sharedBucket->counters[slotIndex + j];
        } else if (match_mask_main) {
            int j = __builtin_ctz(match_mask_main);
            return &mainBucket->counters[slotIndex + j];
        }
    }
    return slot;
}

std::vector<uint64_t> wrert_insert_segment_split_time_ = {};
std::vector<uint64_t> wrert_insert_directory_grow_time_ = {};
std::vector<uint64_t> wrert_per_insert_node_update_latency_ = {};

OperationResults WRERTNode::put(uint64_t subKey, uint64_t value, WRERTSegmentNode* segmentNode, WRERTSegment* segment, WRERTSlotKeyValue* slot,
                                uint64_t segmentIndex, uint64_t _sharedBucketOffset, uint16_t _fingerprint, int8_t _isShareOrMainFlag, uint8_t _nodeFlag, uint64_t* beforeAddress) {
    if (slot == nullptr) {
        if (WRERT_LIKELY(segmentNode->localDepth < this->directoryNode.directory.globalDepth)) {
            WRERTSegment* newSegment = NewWRERTSegment();
            const uint8_t globalDepth = this->directoryNode.directory.globalDepth;
            const uint8_t localDepth = segmentNode->localDepth;
            const uint64_t segmentNodeNum = 1ULL << (globalDepth - localDepth);
            const uint64_t tmpNodeNum = segmentNodeNum >> 1;

            const uint64_t segmentIndexStart = segmentIndex & (~(segmentNodeNum - 1));
            const uint64_t segmentIndexMid = segmentIndexStart + tmpNodeNum;

            const uint8_t dstSharedBucketOffset = (_sharedBucketOffset + WRERT_SHARED_BUCKETS_PER_SEGMENT) % WRERT_SEGMENT_SIZE;
            const uint8_t migSharedBucketPositionStart = _sharedBucketOffset + WRERT_MIG_SHARED_BUCKET_NUM;
            const uint8_t migMainBucketPositionStart =
              ((_sharedBucketOffset + WRERT_SHARED_BUCKETS_PER_SEGMENT) % WRERT_SEGMENT_SIZE + (migSharedBucketPositionStart % WRERT_SHARED_BUCKETS_PER_SEGMENT) * 2) % WRERT_SEGMENT_SIZE;

            const int8_t _migVersion = localDepth + 1;
            const int8_t sharedBucketVersion = localDepth - 1;
            WRERTBucket *migSharedBucket, *migMainBucket;
            uint64_t newMainBucketIndex, newMainBucketPosition;
            uint64_t dstMainBucketIndex, dstMainBucketPosition;

            uint8_t newMainBucketCounter[WRERT_MAIN_BUCKETS_PER_SEGMENT] = {}, dstMainBucketCounter[WRERT_MAIN_BUCKETS_PER_SEGMENT] = {};
            WRERTBucket* _migSharedBuckets = WRERTNode::migration_buffer_pool;

            memcpy(&newSegment->buckets[_sharedBucketOffset],
                   &segment->buckets[migMainBucketPositionStart], sizeof(WRERTBucket) * WRERT_MIG_MAIN_BUCKET_NUM);
            memcpy(_migSharedBuckets, &segment->buckets[_sharedBucketOffset], sizeof(WRERTBucket) * WRERT_MIG_SHARED_BUCKET_NUM);

            for (size_t i = 0; i < WRERT_MIG_SHARED_BUCKET_NUM; i++) {
                for (size_t j = 0; j < WRERT_SLOTS_PER_BUCKET; j++) {
                    auto* slot = &segment->buckets[migSharedBucketPositionStart + i].counters[j];
                    if (slot->subKeyFields.validFlag == 0 || slot->subKeyFields.migVersion != sharedBucketVersion) {
                        break;
                    }
                    uint64_t subKey = slot->subKeyFields.subKey;
                    if (WRERT_GET_SEGMENT_NUMBER(subKey, WRERT_NODE_SPAN, globalDepth) >= segmentIndexMid) {
                        newMainBucketIndex = WRERT_GET_MAIN_BUCKET_INDEX(subKey, _migVersion, WRERT_MAIN_BUCKETS_MAX_BITS);
                        newMainBucketPosition = calcBucketPosition(_sharedBucketOffset, newMainBucketIndex);
                        size_t slotIndex = newMainBucketCounter[newMainBucketIndex & WRERT_MAIN_BUCKETS_PER_SEGMENT_MASK]++;
                        newSegment->buckets[newMainBucketPosition].counters[slotIndex] = *slot;
                        newSegment->buckets[newMainBucketPosition].counters[slotIndex].subKeyFields.migVersion = _migVersion;
                    }
                }
            }

            for (size_t i = 0; i < WRERT_MIG_SHARED_BUCKET_NUM; i++) {
                for (size_t j = 0; j < WRERT_SLOTS_PER_BUCKET; j++) {
                    auto* slot = &_migSharedBuckets[i].counters[j];
                    if (slot->subKeyFields.validFlag == 0 || slot->subKeyFields.migVersion != sharedBucketVersion) {
                        break;
                    }
                    uint64_t subKey = slot->subKeyFields.subKey;
                    if (WRERT_GET_SEGMENT_NUMBER(subKey, WRERT_NODE_SPAN, globalDepth) < segmentIndexMid) {
                        dstMainBucketIndex = WRERT_GET_MAIN_BUCKET_INDEX(subKey, _migVersion, WRERT_MAIN_BUCKETS_MAX_BITS);
                        dstMainBucketPosition = calcBucketPosition(dstSharedBucketOffset, dstMainBucketIndex);
                        size_t slotIndex = dstMainBucketCounter[dstMainBucketIndex & WRERT_MAIN_BUCKETS_PER_SEGMENT_MASK]++;
                        segment->buckets[dstMainBucketPosition].counters[slotIndex] = *slot;
                        segment->buckets[dstMainBucketPosition].counters[slotIndex].subKeyFields.migVersion = _migVersion;
                    }
                }
            }

            clflush((char*)newSegment, sizeof(WRERTSegment));

            if (dstSharedBucketOffset == WRERT_SHARED_BUCKETS_OFFSET) {
                clflush((char*)segment + WRERT_SHARED_BUCKETS_PER_SEGMENT, sizeof(WRERTBucket) * WRERT_MAIN_BUCKETS_PER_SEGMENT);
            } else if (dstSharedBucketOffset == WRERT_MAIN_BUCKETS_PER_SEGMENT) {
                clflush((char*)segment, sizeof(WRERTBucket) * WRERT_MAIN_BUCKETS_PER_SEGMENT);
            } else {
                clflush((char*)segment, sizeof(WRERTBucket) * WRERT_SHARED_BUCKETS_PER_SEGMENT);
                clflush((char*)segment + (dstSharedBucketOffset + WRERT_SHARED_BUCKETS_PER_SEGMENT), sizeof(WRERTBucket) * WRERT_SHARED_BUCKETS_PER_SEGMENT);
            }

            // clflush((char*)segment, sizeof(WRERTSegment));

            for (size_t i = 0; i < tmpNodeNum; i++) {
                WRERTSegmentNode* newSegNode = reinterpret_cast<WRERTSegmentNode*>(WRERT_GET_SEGMENT_POSITION(&this->directoryNode, (segmentIndexStart + tmpNodeNum + i)));
                WRERTSegmentNode* oldSegNode = reinterpret_cast<WRERTSegmentNode*>(WRERT_GET_SEGMENT_POSITION(&this->directoryNode, (segmentIndexStart + i)));
                newSegNode->localDepth++;
                newSegNode->segmentPtr = reinterpret_cast<uint64_t>(newSegment);
                oldSegNode->localDepth++;
                oldSegNode->sharedBucketOffset = dstSharedBucketOffset == 0 ? 0 : dstSharedBucketOffset - 1;
            }

            clflush((char*)reinterpret_cast<WRERTSegmentNode*>(WRERT_GET_SEGMENT_POSITION(&this->directoryNode, segmentIndexStart)), sizeof(WRERTSegmentNode) * segmentNodeNum);

            this->putSegNode(subKey, value, _fingerprint, beforeAddress, _nodeFlag);

            return OperationResults::Success;
        } else if (WRERT_LIKELY(segmentNode->localDepth == this->directoryNode.directory.globalDepth)) {
            const uint8_t globalDepth = this->directoryNode.directory.globalDepth;
            const uint8_t localDepth = segmentNode->localDepth;

            WRERTNode* newNode = static_cast<WRERTNode*>(concurrency_fast_alloc(
              sizeof(WRERTNode) + sizeof(WRERTSegmentNode) * (1 << (globalDepth + 1))));
            newNode->directoryNode.directory = WRERTDirectoryNode::WRERTDirectory(
              &this->directoryNode.directory,
              globalDepth + 1);
            const uint8_t newGlobalDepth = newNode->directoryNode.directory.globalDepth;
            newNode->directoryNode.capacity = 1 << newGlobalDepth;
            newNode->directoryNode.header.initialize(&this->directoryNode.header, this->directoryNode.header.prefixLength, this->directoryNode.header.nodeHeaderDepth);

            for (size_t i = 0; i < this->directoryNode.capacity; i++) {
                const WRERTSegmentNode* oldSegmentNode =
                  reinterpret_cast<const WRERTSegmentNode*>(WRERT_GET_SEGMENT_POSITION(&this->directoryNode, i));
                *reinterpret_cast<WRERTSegmentNode*>(
                  WRERT_GET_SEGMENT_POSITION(&newNode->directoryNode, (i * 2))) = *oldSegmentNode;
                *reinterpret_cast<WRERTSegmentNode*>(
                  WRERT_GET_SEGMENT_POSITION(&newNode->directoryNode, (i * 2 + 1))) = *oldSegmentNode;
            }

            WRERTSegment* newSegment = NewWRERTSegment();
            WRERTSegmentNode *migSegmentNode, *newSegmentNode;

            migSegmentNode = reinterpret_cast<WRERTSegmentNode*>(
              WRERT_GET_SEGMENT_POSITION(&newNode->directoryNode, (segmentIndex * 2)));
            newSegmentNode = reinterpret_cast<WRERTSegmentNode*>(
              WRERT_GET_SEGMENT_POSITION(&newNode->directoryNode, (segmentIndex * 2 + 1)));

            const uint8_t dstSharedBucketOffset = (_sharedBucketOffset + WRERT_SHARED_BUCKETS_PER_SEGMENT) % WRERT_SEGMENT_SIZE;
            const uint8_t migSharedBucketPositionStart = _sharedBucketOffset + WRERT_MIG_SHARED_BUCKET_NUM;
            const uint8_t migMainBucketPositionStart =
              ((_sharedBucketOffset + WRERT_SHARED_BUCKETS_PER_SEGMENT) % WRERT_SEGMENT_SIZE + (migSharedBucketPositionStart % WRERT_SHARED_BUCKETS_PER_SEGMENT) * 2) % WRERT_SEGMENT_SIZE;

            const uint8_t _migVersion = localDepth + 1;
            const int8_t sharedBucketVersion = localDepth - 1;
            uint64_t newSegmentIndex = segmentIndex * 2 + 1, _segmentIndex = segmentIndex * 2;
            uint64_t newMainBucketIndex, newMainBucketPosition;
            uint64_t dstMainBucketIndex, dstMainBucketPosition;

            uint8_t newMainBucketCounter[WRERT_MAIN_BUCKETS_PER_SEGMENT] = {}, dstMainBucketCounter[WRERT_MAIN_BUCKETS_PER_SEGMENT] = {};
            WRERTBucket* _migSharedBuckets = WRERTNode::migration_buffer_pool;

            memcpy(&newSegment->buckets[WRERT_SHARED_BUCKETS_OFFSET],
                   &segment->buckets[migMainBucketPositionStart], sizeof(WRERTBucket) * WRERT_MIG_MAIN_BUCKET_NUM);
            memcpy(_migSharedBuckets, &segment->buckets[_sharedBucketOffset], sizeof(WRERTBucket) * WRERT_MIG_SHARED_BUCKET_NUM);

            for (size_t i = 0; i < WRERT_MIG_SHARED_BUCKET_NUM; i++) {
                for (size_t j = 0; j < WRERT_SLOTS_PER_BUCKET; j++) {
                    auto* slot = &segment->buckets[migSharedBucketPositionStart + i].counters[j];
                    if (slot->subKeyFields.validFlag == 0 || slot->subKeyFields.migVersion != sharedBucketVersion) {
                        break;
                    }
                    uint64_t subKey = slot->subKeyFields.subKey;
                    if (WRERT_GET_SEGMENT_NUMBER(subKey, WRERT_NODE_SPAN, newGlobalDepth) == newSegmentIndex) {
                        newMainBucketIndex = WRERT_GET_MAIN_BUCKET_INDEX(subKey, _migVersion, WRERT_MAIN_BUCKETS_MAX_BITS);
                        newMainBucketPosition = calcBucketPosition(WRERT_SHARED_BUCKETS_OFFSET, newMainBucketIndex);
                        size_t slotIndex = newMainBucketCounter[newMainBucketIndex & WRERT_MAIN_BUCKETS_PER_SEGMENT_MASK]++;
                        newSegment->buckets[newMainBucketPosition].counters[slotIndex] = *slot;
                        newSegment->buckets[newMainBucketPosition].counters[slotIndex].subKeyFields.migVersion = _migVersion;
                    }
                }
            }

            for (size_t i = 0; i < WRERT_MIG_SHARED_BUCKET_NUM; i++) {
                for (size_t j = 0; j < WRERT_SLOTS_PER_BUCKET; j++) {
                    auto* slot = &_migSharedBuckets[i].counters[j];
                    if (slot->subKeyFields.validFlag == 0 || slot->subKeyFields.migVersion != sharedBucketVersion) {
                        break;
                    }
                    uint64_t subKey = slot->subKeyFields.subKey;
                    if (WRERT_GET_SEGMENT_NUMBER(subKey, WRERT_NODE_SPAN, newGlobalDepth) == _segmentIndex) {
                        dstMainBucketIndex = WRERT_GET_MAIN_BUCKET_INDEX(subKey, _migVersion, WRERT_MAIN_BUCKETS_MAX_BITS);
                        dstMainBucketPosition = calcBucketPosition(dstSharedBucketOffset, dstMainBucketIndex);
                        size_t slotIndex = dstMainBucketCounter[dstMainBucketIndex & WRERT_MAIN_BUCKETS_PER_SEGMENT_MASK]++;
                        segment->buckets[dstMainBucketPosition].counters[slotIndex] = *slot;
                        segment->buckets[dstMainBucketPosition].counters[slotIndex].subKeyFields.migVersion = _migVersion;
                    }
                }
            }

            clflush((char*)newSegment, sizeof(WRERTSegment));

            if (dstSharedBucketOffset == WRERT_SHARED_BUCKETS_OFFSET) {
                clflush((char*)segment + WRERT_SHARED_BUCKETS_PER_SEGMENT, sizeof(WRERTBucket) * WRERT_MAIN_BUCKETS_PER_SEGMENT);
            } else if (dstSharedBucketOffset == WRERT_MAIN_BUCKETS_PER_SEGMENT) {
                clflush((char*)segment, sizeof(WRERTBucket) * WRERT_MAIN_BUCKETS_PER_SEGMENT);
            } else {
                clflush((char*)segment, sizeof(WRERTBucket) * WRERT_SHARED_BUCKETS_PER_SEGMENT);
                clflush((char*)segment + (dstSharedBucketOffset + WRERT_SHARED_BUCKETS_PER_SEGMENT), sizeof(WRERTBucket) * WRERT_SHARED_BUCKETS_PER_SEGMENT);
            }

            // clflush((char*)segment, sizeof(WRERTSegment));

            newSegmentNode->localDepth++;
            newSegmentNode->segmentPtr = reinterpret_cast<uint64_t>(newSegment);
            newSegmentNode->sharedBucketOffset = WRERT_SHARED_BUCKETS_OFFSET;

            migSegmentNode->localDepth++;
            migSegmentNode->sharedBucketOffset = dstSharedBucketOffset == 0 ? 0 : dstSharedBucketOffset - 1;

            clflush((char*)newNode, sizeof(WRERTNode) + sizeof(WRERTSegmentNode) * newNode->directoryNode.capacity);

            if (newNode->directoryNode.header.nodeHeaderDepth != 0) {
                ((WRERTSlotKeyValue*)(*beforeAddress))->valueFields.value = (uint64_t)newNode;
                clflush((char*)(WRERTSlotKeyValue*)(*beforeAddress), sizeof(WRERTSlotKeyValue));
            } else {
                *(WRERTNode**)(*beforeAddress) = newNode;
                clflush((char*)(*beforeAddress), sizeof(WRERTNode*));
            }

            newNode->putSegNode(subKey, value, _fingerprint, beforeAddress, _nodeFlag);
            return OperationResults::Success;
        }
    } else {
        if (slot->subKeyFields.subKey == subKey && slot->subKeyFields.validFlag == 1) {
            slot->valueFields.value = value;
            clflush((char*)&slot->valueFields, sizeof(slot->valueFields));
        } else {
            slot->valueFields.value = value;
            mfence();
            slot->subKeyFields.subKey = subKey;
            slot->subKeyFields.validFlag = 1;
            slot->subKeyFields.fingerprint = _fingerprint;

            slot->subKeyFields.migVersion = segmentNode->localDepth - !_isShareOrMainFlag;
            slot->subKeyFields.nodeFlag = _nodeFlag;
            clflush((char*)slot, sizeof(WRERTSlotKeyValue));
        }

        return OperationResults::Success;
    }
}

uint64_t WRERTNode::fingerprint(uint64_t key) {
    return key & ((1ul << WRERT_FINGERPRINT_BITS) - 1);
}

uint64_t WRERTNode::stale_fingerprint(uint64_t key, uint8_t depth) {
    auto bitUsed = depth + WRERT_MAIN_BUCKETS_MAX_BITS;
    if (bitUsed >= WRERT_SPLIT_THRESHOLD) {
        return key & ((1ul << WRERT_FINGERPRINT_BITS) - 1);
    }

    bitUsed = bitUsed >= 8 ? bitUsed - 8 : bitUsed;
    auto alignment = bitUsed & (~(WRERT_FINGERPRINT_BIT_ALIGNMENT - 1ul));
    return (key << (alignment)) >> 48ul;
}

WRERTBucket::WRERTBucket() {
    memset(this, 0, sizeof(WRERTBucket));

    for (int i = 0; i < WRERT_SLOTS_PER_BUCKET; i++) {
        counters[i].subKeyFields.migVersion = 1;
    }
}

WRERTBucket::~WRERTBucket() {}

WRERTSegment::WRERTSegment() {}

WRERTSegment::~WRERTSegment() {}

WRERTSegmentNode::WRERTSegmentNode() {}

WRERTSegmentNode::~WRERTSegmentNode() {}

WRERTDirectoryNode::WRERTDirectoryNode() {}

WRERTDirectoryNode::~WRERTDirectoryNode() {}

WRERTNode::WRERTNode() {}

WRERTNode::~WRERTNode() {}

void WRERTSegment::initialize() {
    memset(buckets, 0, sizeof(WRERTBucket) * WRERT_SEGMENT_SIZE);
}

void WRERTHeader::initialize(const WRERTHeader* oldHeader, unsigned char prefixLength, unsigned char nodeHeaderDepth) {
    if (oldHeader != nullptr) {
        assignPrefix(oldHeader->prefixArray, prefixLength);
    }
    this->prefixLength = prefixLength;
    this->nodeHeaderDepth = nodeHeaderDepth;
}

void WRERTDirectoryNode::initialize(uint8_t globalDepth) {
    this->capacity = 1 << globalDepth;
    this->directory.globalDepth = globalDepth;

    // WRERTKeyValue* globalSegment = static_cast<WRERTKeyValue*>(
    //   concurrency_fast_alloc(sizeof(WRERTKeyValue) * WRERT_BUCKET_SIZE));

    // for (uint64_t i = 0; i < WRERT_BUCKET_SIZE; i++) {
    //     globalSegment[i].key = 0;
    //     globalSegment[i].value = -1;
    // }

    // this->directory.globalSegmentPtr = reinterpret_cast<uint64_t>(globalSegment);
    // this->directory.globalSegmentPtr = 0;
}

void WRERTNode::initialize(unsigned char nodeHeaderDepth, unsigned char globalDepth) {
    this->directoryNode.header.initialize(nullptr, WRERT_INIT_PREFIX_LENGTH, nodeHeaderDepth);
    this->directoryNode.initialize(globalDepth);
    WRERTSegmentNode* segmentNode = reinterpret_cast<WRERTSegmentNode*>(WRERT_GET_SEGMENT_POSITION(this, 0));
    segmentNode->sharedBucketOffset = WRERT_SHARED_BUCKETS_OFFSET;
    segmentNode->segmentPtr = reinterpret_cast<uint64_t>(NewWRERTSegment());
}

WRERTKeyValue* NewWRERTKeyValue(uint64_t key, uint64_t value) {
    WRERTKeyValue* _newKV = static_cast<WRERTKeyValue*>(concurrency_fast_alloc(sizeof(WRERTKeyValue)));
    _newKV->key = key;
    _newKV->value = value;
    return _newKV;
}

WRERTBucket* NewWRERTBucket() {
    WRERTBucket* newBucket = new WRERTBucket();
    return newBucket;
}

WRERTSegment* NewWRERTSegment() {
    WRERTSegment* _newSegment = static_cast<WRERTSegment*>(concurrency_fast_alloc(sizeof(WRERTSegment)));
    return _newSegment;
}

WRERTNode* NewWRERTNode(unsigned char nodeHeaderDepth, unsigned char globalDepth) {
    WRERTNode* _newNode = static_cast<WRERTNode*>(concurrency_fast_alloc(
      sizeof(WRERTNode) + sizeof(WRERTSegmentNode) * (1 << globalDepth)));
    _newNode->initialize(nodeHeaderDepth, globalDepth);
    return _newNode;
}

} // namespace wrert
