#include <iostream>
#include <cstring>
#include <atomic>
#include <chrono>
#include <iostream>

#include "roert_node.h"

namespace roert {

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

thread_local ROERTBucket ROERTNode::migration_buffer_pool[ROERT_MIG_SHARED_BUCKET_NUM];
// thread_local ROERTBucket ROERTNode::migration_buffer_pool[ROERT_SHARED_BUCKETS_PER_SEGMENT];

// ==================== ROERTBucket 类实现 ====================
bool ROERTBucket::isFull(uint64_t segmentMSB, uint8_t localDepth) {
    // 检查最后一个slot的子键最高位是否与段逻辑编号一致
    const uint64_t lastSlotSubKey = this->counters[ROERT_SLOTS_PER_BUCKET - 1].subKeyFields.subKey;

    // 提取子键的最高位（使用localDepth位）
    const uint64_t subKeyMSB = ROERT_GET_SEGMENT_NUMBER(lastSlotSubKey, ROERT_NODE_SPAN, localDepth);

    // 检查最后一个slot是否有效
    if (this->counters[ROERT_SLOTS_PER_BUCKET - 1].subKeyFields.validFlag != 1) {
        return false; // 最后一个slot无效，桶未满
    } else if (subKeyMSB != segmentMSB) {
        return false; // 子键最高位与段逻辑编号不一致，可能是迁移残留数据，桶未满
    }

    // 最后一个slot有效且子键最高位与段逻辑编号一致，桶已满
    return true;
}

uint64_t ROERTBucket::getValue(uint64_t key, bool& keyValueFlag) {}

int ROERTBucket::findPutPlace(uint64_t key, uint64_t keyLength, uint64_t depth) {}

// ==================== ROERTSegmentNode 类实现 ====================

// bool ROERTSegmentNode::tryReadLock() {
// }

// bool ROERTSegmentNode::tryWriteLock() {
// }

// void ROERTSegmentNode::readUnlock() {
// }

// void ROERTSegmentNode::writeUnlock() {
// }

// bool ROERTSegmentNode::casLockState(uint8_t expected, uint8_t desired) {
// }

// ==================== ROERTHeader 类实现 ====================

int ROERTHeader::computePrefix(uint64_t key, int startPosition, uint32_t* arrayMatchLength) {
    // 如果前缀长度为 ROERT_INIT_PREFIX_LENGTH，表示前缀未初始化，返回-1
    if (this->prefixLength == 6) {
        return -1;
    }

    // 如果前缀长度为0，说明没有前缀可匹配，返回0
    if (this->prefixLength == 0) {
        return 0;
    }

    // 从键的 startPosition 位置提取 len*8 位的子键
    const uint64_t subKey = ROERT_GET_SUBKEY(key, startPosition, (this->prefixLength * ROERT_NODE_SUBKEY_MAX_BYTES * ROERT_SIZE_OF_CHAR));
    // const uint64_t subKey = ROERT_GET_SUBKEY(key, startPosition, (this->prefixLength << 5));

    // 将子键左移到64位最高位对齐，便于逐字节比较
    const uint64_t shiftedSubKey = subKey << (64 - this->prefixLength * ROERT_NODE_SUBKEY_MAX_BYTES * ROERT_SIZE_OF_CHAR);
    // const uint64_t shiftedSubKey = subKey << (64 - this->prefixLength << 5);

    int prefixMatchLength = 0;

    // 逐字节比较子键与节点前缀数组
    for (int i = 0; i < this->prefixLength; i++) {
        int flag = 0;

        for (int j = i * ROERT_NODE_SUBKEY_MAX_BYTES; j < (i + 1) * ROERT_NODE_SUBKEY_MAX_BYTES && j < ROERT_NODE_PREFIX_MAX_BYTES; j++) {
            // printf("subKey = %02llx, prefixArray[%d] = %02x\n", ((shiftedSubKey >> (56 - j * 8)) & 0xFF), j, prefixArray[j]);

            // 比较子键的第i个字节（右移 (j * 8) 位得到当前字节）与节点前缀数组的第i个字节
            const unsigned char byteFromKey = (unsigned char)((shiftedSubKey >> (56 - j * 8)) & 0xFF);
            if (byteFromKey != prefixArray[j]) {
                break;
            }

            arrayMatchLength++;
            // (*arrayMatchLength)++;
            flag++;
        }

        if (flag == ROERT_NODE_SUBKEY_MAX_BYTES) {
            prefixMatchLength++;
        }
    }

    return prefixMatchLength;
}

int ROERTHeader::computeCommonPrefix(uint64_t key1, uint64_t key2, int startPosition) {
    int remaining_bits = (ROERT_KEY_MAX_BYTES - startPosition) * ROERT_SIZE_OF_CHAR;
    uint64_t mask = (remaining_bits >= 64) ? 0xFFFFFFFFFFFFFFFFULL : (1ULL << remaining_bits) - 1;

    uint64_t masked_key1 = key1 & mask;
    uint64_t masked_key2 = key2 & mask;

    int commonPrefix = (__builtin_clzll(masked_key1 ^ masked_key2) / ROERT_SIZE_OF_CHAR) - startPosition;

    return commonPrefix;
}

void ROERTHeader::assignPrefix(uint64_t key, int startPosition) {
    // 从键的 startPosition 位置提取 prefixLength*8 位的子键
    uint64_t subkey = ROERT_GET_SUBKEY(key, startPosition, (this->prefixLength * ROERT_NODE_SUBKEY_MAX_BYTES * ROERT_SIZE_OF_CHAR));
    // uint64_t subkey = ROERT_GET_SUBKEY(key, startPosition, (this->prefixLength << 5));
    // printf("subkey = %08llx\n", subkey);
    // 将子键左移到前缀数组的最高位对齐
    subkey <<= (ROERT_NODE_PREFIX_MAX_BITS - this->prefixLength * ROERT_NODE_SUBKEY_MAX_BYTES * ROERT_SIZE_OF_CHAR);
    // subkey <<= (ROERT_NODE_PREFIX_MAX_BITS - this->prefixLength << 5);

    // 按字节从高位到低位存储到 prefixArray 数组中（大端字节序）
    for (int i = ROERT_NODE_PREFIX_MAX_BYTES - 1; i >= 0; i--) {
        // 提取当前字节：subkey & 0xFF
        prefixArray[i] = (unsigned char)(subkey & 0xFF);
        // 右移 8 位处理下一个字节
        subkey >>= 8;

        // printf("prefixArray[%d] = %02x\n", i, prefixArray[i]);
    }
}

void ROERTHeader::assignPrefix(const unsigned char* key, unsigned char assignedLength) {
    if (ROERT_UNLIKELY(assignedLength == ROERT_INIT_PREFIX_LENGTH)) {
        return;
    }
    const int maxBytes = assignedLength * ROERT_NODE_SUBKEY_MAX_BYTES;
    // 将字节数组复制到前缀数组中
    // for (int i = 0; assignedLength != ROERT_INIT_PREFIX_LENGTH && i < assignedLength * ROERT_NODE_SUBKEY_MAX_BYTES; i++) {
    //     prefixArray[i] = key[i];
    // }
    for (int i = 0; i < maxBytes && i < ROERT_NODE_PREFIX_MAX_BYTES; i++) {
        prefixArray[i] = key[i];
    }
}

OperationResults ROERTNode::putDirNode(ROERTKeyValue* globalSegment, uint64_t key, uint64_t value) {
    for (uint64_t i = 0; i < ROERT_BUCKET_SIZE; i++) {
        if (globalSegment[i].key == 0 && globalSegment[i].value == -1) {
            globalSegment[i].key = key;
            globalSegment[i].value = value;

            // 刷新内存，确保写入可见
            clflush((char*)globalSegment + i * sizeof(ROERTKeyValue), sizeof(ROERTKeyValue));

            return OperationResults::Success;
        }
    }
    return OperationResults::Failed;
}

uint64_t ROERTNode::getDirNode(ROERTKeyValue* globalSegment, uint64_t key) {
    for (uint64_t i = 0; i < ROERT_BUCKET_SIZE; i++) {
        if (globalSegment[i].key == key) {
            return globalSegment[i].value;
        }
    }

    return (uint64_t)OperationResults::Failed;
}

OperationResults ROERTNode::putSegNode(uint64_t subKey, uint64_t value, uint16_t _fingerprint, uint64_t* beforeAddress, uint8_t _nodeFlag) {
    const uint8_t globalDepth = this->directoryNode.directory.globalDepth;

    // 使用子键的 MSB 作为段索引
    const uint64_t segmentIndex = ROERT_GET_SEGMENT_NUMBER(subKey, ROERT_NODE_SPAN, globalDepth);

    // 获取段节点指针
    ROERTSegmentNode* segmentNode = reinterpret_cast<ROERTSegmentNode*>(
      ROERT_GET_SEGMENT_POSITION(&this->directoryNode, segmentIndex));

    // 获取段指针
    ROERTSegment* segment = reinterpret_cast<ROERTSegment*>(segmentNode->segmentPtr);

    // 共享桶在段中的位置：从共享桶偏移开始
    const uint8_t sharedBucketOffset = segmentNode->sharedBucketOffset == 0 ? 0 : segmentNode->sharedBucketOffset + 1;
    const uint8_t localDepth = segmentNode->localDepth;

    // 计算共享桶索引、共享桶位置、共享桶指针
    const uint64_t sharedBucketIndex = ROERT_GET_SHARED_BUCKET_INDEX(subKey, localDepth, ROERT_MAIN_BUCKETS_MAX_BITS);
    const uint64_t sharedBucketPosition = sharedBucketOffset + (sharedBucketIndex & ROERT_SHARED_BUCKETS_PER_SEGMENT_MASK);
    ROERTBucket* sharedBucket = &segment->buckets[sharedBucketPosition];

    uint64_t shareSlotIndex, mainSlotIndex, currentSharedBucketIndex, currentMainBucketIndex;
    const uint8_t sharedBucketVersion = localDepth - 1;
    // 标志位：0 表示共享桶，1 表示主桶
    int _isShareOrMainFlag;
    ROERTSlotKeyValue* slot;

    // 共享桶未满，遍历共享桶查找可用 slot
    // for (shareSlotIndex = 0; shareSlotIndex < ROERT_SLOTS_PER_BUCKET; ++shareSlotIndex) {
    //     // 获取共享桶 slot 指针
    //     slot = &sharedBucket->counters[shareSlotIndex];
    //     if ((uint8_t)slot->subKeyFields.migVersion != sharedBucketVersion) {
    //         // printf("重新插入键共享桶插槽 %lu slot->subKeyFields.subKey: 0x%08llx, value: %lu, migVersion: %d, sharedBucketVersion: %u,  nodeFlag: %lu, validFlag: %lu, localDepth: %lu\n",
    //         //        shareSlotIndex, slot->subKeyFields.subKey, slot->valueFields.value, slot->subKeyFields.migVersion, sharedBucketVersion, slot->subKeyFields.nodeFlag, slot->subKeyFields.validFlag, localDepth);
    //         return this->put(
    //           subKey, value, segmentNode, segment, slot,
    //           segmentIndex, sharedBucketOffset, _fingerprint, _isShareOrMainFlag = 0, _nodeFlag, beforeAddress);
    //     }
    // }

    // 遍历共享桶查找匹配项 - AVX-512加速
    const __m512i v_target1 = _mm512_set1_epi64(sharedBucketVersion);
    const __m512i v_offsets = _mm512_load_si512((__m512i*)offsets);
    const __m512i v_migVersion_mask = _mm512_set1_epi64(ROERT_MIGVERSION_MASK);
    for (uint64_t shareSlotIndex = 0; shareSlotIndex < ROERT_SLOTS_PER_BUCKET; shareSlotIndex += 8) {
        // 使用gather指令加载8个slot的内存数据
        __m512i keys = _mm512_mask_i64gather_epi64(
          _mm512_setzero_si512(),                         // 初始值
          summary_mask,                                   // 有效掩码
          v_offsets,                                      // 偏移量向量
          (void*)&sharedBucket->counters[shareSlotIndex], // 基地址
          1                                               // 比例因子
        );
        // 提取migVersion (位34-41) - 使用AVX-512的位操作
        __m512i v_migVersion = _mm512_and_si512(keys, v_migVersion_mask);
        v_migVersion = _mm512_srli_epi64(v_migVersion, 32);
        __mmask8 match_mask = _mm512_cmpneq_epi64_mask(v_migVersion, v_target1); // 不相等的掩码
        if (match_mask) {
            int j = __builtin_ctz(match_mask); // 找到最低位的1
            // printf("重新插入键共享桶插槽 %lu slot->subKeyFields.subKey: 0x%08llx, value: %lu, migVersion: %d, sharedBucketVersion: %u, nodeFlag: %lu, validFlag: %lu, localDepth: %lu\n",
            //        shareSlotIndex + j, slot->subKeyFields.subKey, slot->valueFields.value, slot->subKeyFields.migVersion, sharedBucketVersion, slot->subKeyFields.nodeFlag, slot->subKeyFields.validFlag, localDepth);
            return this->put(
              subKey, value, segmentNode, segment, &sharedBucket->counters[shareSlotIndex + j],
              segmentIndex, sharedBucketOffset, _fingerprint, _isShareOrMainFlag = 0, _nodeFlag, beforeAddress);
        }
    }

    // const __m512i shuffle_subkeys = _mm512_setr_epi64(0, 2, 4, 6, 0, 0, 0, 0);
    // for (uint64_t shareSlotIndex = 0; shareSlotIndex < ROERT_SLOTS_PER_BUCKET; shareSlotIndex += 4) {
    //     // 1. 【连续加载】读取 4 个完整的 Slot（包含 4个subKey + 4个value）
    //     __m512i raw_data = _mm512_loadu_si512((const __m512i*)&sharedBucket->counters[shareSlotIndex]);
    //     // 2. 【寄存器内洗牌】从 8 个 64位整数中，提取出第 0, 2, 4, 6 个（即 4 个 subKey）
    //     __m512i keys = _mm512_permutexvar_epi64(shuffle_subkeys, raw_data);
    //     // 3. 【位运算提取 migVersion】
    //     __m512i v_migVersion = _mm512_and_si512(keys, v_migVersion_mask);
    //     v_migVersion = _mm512_srli_epi64(v_migVersion, 32);
    //     // 4. 【并行比较】找出不相等的项，并屏蔽掉高 4 位无效数据
    //     __mmask8 match_mask = _mm512_cmpneq_epi64_mask(v_migVersion, v_target1) & 0x0F;
    //     if (match_mask) {
    //         int j = __builtin_ctz(match_mask); // 找到命中的位置 (0-3)
    //         // 注意：这里的 j 是这 4 个 Slot 里的相对偏移，加上 shareSlotIndex 才是真实下标
    //         return this->put(
    //           subKey, value, segmentNode, segment, &sharedBucket->counters[shareSlotIndex + j],
    //           segmentIndex, sharedBucketOffset, _fingerprint, _isShareOrMainFlag = 0, _nodeFlag, beforeAddress);
    //     }
    // }

    // 共享桶没有找到，遍历主桶查找可用 slot
    // 计算主桶索引：使用子键的高位索引主桶
    const uint64_t mainBucketIndex = ROERT_GET_MAIN_BUCKET_INDEX(subKey, localDepth, ROERT_MAIN_BUCKETS_MAX_BITS);
    // 主桶在段中的位置
    const uint64_t mainBucketPosition = calcBucketPosition(sharedBucketOffset, mainBucketIndex);
    // 获取主桶指针
    ROERTBucket* mainBucket = &segment->buckets[mainBucketPosition];

    // 主桶未满，遍历主桶查找可用 slot
    // for (mainSlotIndex = 0; mainSlotIndex < ROERT_SLOTS_PER_BUCKET; ++mainSlotIndex) {
    //     // 获取主桶 slot 指针
    //     slot = &mainBucket->counters[mainSlotIndex];
    //     if (slot->subKeyFields.migVersion != localDepth) {
    //         // printf("重新插入键主桶插槽 %lu slot->subKeyFields.subKey: 0x%08llx, value: %lu, migVersion: %lu, sharedBucketVersion: %d, nodeFlag: %lu, validFlag: %lu, localDepth: %lu\n",
    //         //        mainSlotIndex, slot->subKeyFields.subKey, slot->valueFields.value, slot->subKeyFields.migVersion, sharedBucketVersion, slot->subKeyFields.nodeFlag, slot->subKeyFields.validFlag, localDepth);
    //         return this->put(
    //           subKey, value, segmentNode, segment, slot,
    //           segmentIndex, sharedBucketOffset, _fingerprint, _isShareOrMainFlag = 1, _nodeFlag, beforeAddress);
    //     }
    // }

    const __m512i v_target2 = _mm512_set1_epi64(localDepth);
    for (uint64_t mainSlotIndex = 0; mainSlotIndex < ROERT_SLOTS_PER_BUCKET; mainSlotIndex += 8) {
        // 使用gather指令加载8个slot的内存数据
        __m512i keys = _mm512_mask_i64gather_epi64(
          _mm512_setzero_si512(),                      // 初始值
          summary_mask,                                // 有效掩码
          v_offsets,                                   // 偏移量向量
          (void*)&mainBucket->counters[mainSlotIndex], // 基地址
          1                                            // 比例因子
        );
        // 提取migVersion (位32-40) - 使用AVX-512的位操作
        __m512i v_migVersion = _mm512_and_si512(keys, v_migVersion_mask);
        v_migVersion = _mm512_srli_epi64(v_migVersion, 32);
        __mmask8 match_mask = _mm512_cmpneq_epi64_mask(v_migVersion, v_target2); // 不相等的掩码
        if (match_mask) {
            int j = __builtin_ctz(match_mask); // 找到最低位的1
            // printf("重新插入键主桶插槽 %lu slot->subKeyFields.subKey: 0x%08llx, value: %lu, migVersion: %lu, sharedBucketVersion: %d, nodeFlag: %lu, validFlag: %lu, localDepth: %lu\n",
            //        mainSlotIndex + j, slot->subKeyFields.subKey, slot->valueFields.value, slot->subKeyFields.migVersion, sharedBucketVersion, slot->subKeyFields.nodeFlag, slot->subKeyFields.validFlag, localDepth);
            return this->put(
              subKey, value, segmentNode, segment, &mainBucket->counters[mainSlotIndex + j],
              segmentIndex, sharedBucketOffset, _fingerprint, _isShareOrMainFlag = 1, _nodeFlag, beforeAddress);
        }
    }

    // for (uint64_t mainSlotIndex = 0; mainSlotIndex < ROERT_SLOTS_PER_BUCKET; mainSlotIndex += 4) {
    //     // 1. 【连续加载】读取 4 个完整的 Slot（包含 4个subKey + 4个value）
    //     __m512i raw_data = _mm512_loadu_si512((const __m512i*)&mainBucket->counters[mainSlotIndex]);
    //     // 2. 【寄存器内洗牌】从 8 个 64位整数中，提取出第 0, 2, 4, 6 个（即 4 个 subKey）
    //     __m512i keys = _mm512_permutexvar_epi64(shuffle_subkeys, raw_data);
    //     // 3. 【位运算提取 migVersion】
    //     __m512i v_migVersion = _mm512_and_si512(keys, v_migVersion_mask);
    //     v_migVersion = _mm512_srli_epi64(v_migVersion, 32);
    //     // 4. 【并行比较】找出不相等的项，并屏蔽掉高 4 位无效数据
    //     __mmask8 match_mask = _mm512_cmpneq_epi64_mask(v_migVersion, v_target2) & 0x0F;
    //     if (match_mask) {
    //         int j = __builtin_ctz(match_mask); // 找到命中的位置 (0-3)
    //         // 注意：这里的 j 是这 4 个 Slot 里的相对偏移，加上 shareSlotIndex 才是真实下标
    //         return this->put(
    //           subKey, value, segmentNode, segment, &mainBucket->counters[mainSlotIndex + j],
    //           segmentIndex, sharedBucketOffset, _fingerprint, _isShareOrMainFlag = 1, _nodeFlag, beforeAddress);
    //     }
    // }

    // 主桶也没有找到，slot 为 nullptr，表示需要扩展
    return this->put(
      subKey, value, segmentNode, segment, slot = nullptr,
      segmentIndex, sharedBucketOffset, _fingerprint, _isShareOrMainFlag, _nodeFlag, beforeAddress);
}

ROERTSlotKeyValue* ROERTNode::getSegNode(uint64_t subKey) {
    const uint8_t globalDepth = this->directoryNode.directory.globalDepth;
    // 使用子键的 MSB 作为段索引
    const uint64_t segmentIndex = ROERT_GET_SEGMENT_NUMBER(subKey, ROERT_NODE_SPAN, globalDepth);

    // 获取段节点指针
    ROERTSegmentNode* segmentNode = reinterpret_cast<ROERTSegmentNode*>(
      ROERT_GET_SEGMENT_POSITION(&this->directoryNode, segmentIndex));

    // 获取段指针
    ROERTSegment* segment = reinterpret_cast<ROERTSegment*>(segmentNode->segmentPtr);

    // 共享桶在段中的位置：从共享桶偏移开始计算，每个共享桶占用ROERT_SHARED_BUCKETS_PER_SEGMENT个字节
    const uint8_t sharedBucketOffset = segmentNode->sharedBucketOffset == 0 ? 0 : segmentNode->sharedBucketOffset + 1;
    const uint8_t localDepth = segmentNode->localDepth;

    // 计算共享桶索引：根据主桶索引和共享桶偏移计算
    const uint64_t sharedBucketIndex = ROERT_GET_SHARED_BUCKET_INDEX(subKey, localDepth, ROERT_MAIN_BUCKETS_MAX_BITS);
    // 共享桶在段中的位置
    const uint64_t sharedBucketPosition = sharedBucketOffset + (sharedBucketIndex & ROERT_SHARED_BUCKETS_PER_SEGMENT_MASK);
    // 获取共享桶指针
    ROERTBucket* sharedBucket = &segment->buckets[sharedBucketPosition];

    // printf("segmentIndex: %lu, sharedBucketOffset: %lu, sharedBucketIndex: %lu, sharedBucketPosition: %lu, mainBucketIndex: %lu, mainBucketPosition: %lu\n",
    //        segmentIndex, sharedBucketOffset, sharedBucketIndex, sharedBucketPosition, mainBucketIndex, mainBucketPosition);

    const int8_t sharedBucketVersion = localDepth - 1;
    ROERTSlotKeyValue* slot;

    // 遍历共享桶查找匹配项
    // for (uint64_t shareSlotIndex = 0; shareSlotIndex < ROERT_SLOTS_PER_BUCKET; ++shareSlotIndex) {
    //     // 获取共享桶 slot 指针
    //     slot = &sharedBucket->counters[shareSlotIndex];
    //     // printf("search 共享桶插槽 %lu slot->subKeyFields.subKey: 0x%08llx, subKey: 0x%08llx,  value: %lu, nodeFlag: %lu, validFlag: %lu, migVersion: %d, sharedBucketVersion: %d, localDepth: %d, segmentIndex: %lu\n",
    //     //        shareSlotIndex, slot->subKeyFields.subKey, subKey, slot->valueFields.value, slot->subKeyFields.nodeFlag, slot->subKeyFields.validFlag, slot->subKeyFields.migVersion, sharedBucketVersion, localDepth, segmentIndex);
    //     // if ((localDepth == ROERT_INIT_LOCAL_DEPTH || slot->subKeyFields.migVersion == sharedBucketVersion) && slot->subKeyFields.subKey == subKey) {
    //     if (slot->subKeyFields.validFlag == 1 && slot->subKeyFields.migVersion == sharedBucketVersion && slot->subKeyFields.subKey == subKey) {
    //         return slot;
    //     }
    // }

    // 遍历共享桶查找匹配项 - AVX-512加速
    const __m512i v_shared_target = _mm512_set1_epi64((1ULL << 41) | ((uint64_t)(sharedBucketVersion & ROERT_MIGVERSION_BITS) << 32) | subKey);
    const __m512i v_offsets = _mm512_load_si512((__m512i*)offsets);
    const __m512i v_42bits_mask = _mm512_set1_epi64(ROERT_VALIDFLAG_MIGVERSION_SUBKEY_MASK);
    for (uint64_t shareSlotIndex = 0; shareSlotIndex < ROERT_SLOTS_PER_BUCKET; shareSlotIndex += 8) {
        __m512i keys = _mm512_mask_i64gather_epi64(
          _mm512_setzero_si512(),                         // 初始值
          summary_mask,                                   // 有效掩码
          v_offsets,                                      // 偏移量向量
          (void*)&sharedBucket->counters[shareSlotIndex], // 基地址
          1                                               // 比例因子
        );
        __m512i v_keys = _mm512_and_si512(keys, v_42bits_mask);
        // 打印v_keys的值
        // uint64_t v_keys1_array[8];
        // uint64_t v_keys2_array[8];
        // _mm512_storeu_si512((__m512i*)v_keys1_array, keys);
        // _mm512_storeu_si512((__m512i*)v_keys2_array, v_keys);
        // for (int i = 0; i < 8; i++) {
        //     printf("共享桶v_keys[%d] = 0x%016lx, 0x%016lx\n", i, v_keys1_array[i], v_keys2_array[i]);
        // }
        __mmask8 match_mask = _mm512_cmpeq_epi64_mask(v_keys, v_shared_target);
        if (match_mask) {
            int j = __builtin_ctz(match_mask); // 找到最低位的 1
            // printf("search 共享桶插槽 %lu slot->subKeyFields.subKey: 0x%08llx, value: %lu, nodeFlag: %lu, migVersion: %d, sharedBucketVersion: %d, localDepth: %lu, segmentIndex: %lu\n",
            //        shareSlotIndex + j, sharedBucket->counters[shareSlotIndex + j].subKeyFields.subKey, sharedBucket->counters[shareSlotIndex + j].valueFields.value, sharedBucket->counters[shareSlotIndex + j].subKeyFields.nodeFlag, sharedBucket->counters[shareSlotIndex + j].subKeyFields.migVersion, sharedBucketVersion, localDepth, segmentIndex);
            return &sharedBucket->counters[shareSlotIndex + j];
        }
    }

    // const __m512i shuffle_subkeys = _mm512_setr_epi64(0, 2, 4, 6, 0, 0, 0, 0);
    // for (uint64_t shareSlotIndex = 0; shareSlotIndex < ROERT_SLOTS_PER_BUCKET; shareSlotIndex += 4) {
    //     // 1. 【连续加载】直接读取 64 字节（4个subKey + 4个value），完全没有 Gather 的延迟！
    //     __m512i raw_data = _mm512_loadu_si512((const __m512i*)&sharedBucket->counters[shareSlotIndex]);
    //     // 2. 【寄存器内洗牌】利用 vpermq 指令，只把 4 个 subKey 压缩到寄存器的低 256 位
    //     __m512i keys = _mm512_permutexvar_epi64(shuffle_subkeys, raw_data);
    //     // 3. 【位运算与并行比较】
    //     __m512i v_keys = _mm512_and_si512(keys, v_42bits_mask);
    //     // 因为我们只提取了 4 个有效 key，所以比较时只需要看低 4 位的掩码 (0x0F)
    //     __mmask8 match_mask = _mm512_cmpeq_epi64_mask(v_keys, v_shared_target) & 0x0F;
    //     if (match_mask) {
    //         int j = __builtin_ctz(match_mask); // 找到命中的位置 (0-3)
    //         return &sharedBucket->counters[shareSlotIndex + j];
    //     }
    // }

    // 计算主桶索引：使用子键的高位索引主桶
    const uint64_t mainBucketIndex = ROERT_GET_MAIN_BUCKET_INDEX(subKey, localDepth, ROERT_MAIN_BUCKETS_MAX_BITS);
    // 主桶在段中的位置
    const uint64_t mainBucketPosition = calcBucketPosition(sharedBucketOffset, mainBucketIndex);
    // 获取主桶指针
    ROERTBucket* mainBucket = &segment->buckets[mainBucketPosition];

    // 遍历主桶查找匹配 slot
    // for (uint64_t mainSlotIndex = 0; mainSlotIndex < ROERT_SLOTS_PER_BUCKET; ++mainSlotIndex) {
    //     // 获取主桶 slot 指针
    //     slot = &mainBucket->counters[mainSlotIndex];
    //     // printf("search 主桶插槽 %lu slot->subKeyFields.subKey: 0x%08llx, nodeFlag: %lu, value: %lu, validFlag: %lu, migVersion: %lu, _segmentIndex: %lu, segmentIndex: %lu\n",
    //     //        mainSlotIndex, slot->subKeyFields.subKey, slot->subKeyFields.nodeFlag, slot->valueFields.value, slot->subKeyFields.validFlag, slot->subKeyFields.migVersion, _segmentIndex, segmentIndex);
    //     // if (isInvalidMainSlot(slot, localDepth)) {
    //     if (slot->subKeyFields.migVersion == localDepth && slot->subKeyFields.subKey == subKey) {
    //         return slot;
    //     }
    // }

    const __m512i v_main_target = _mm512_set1_epi64((1ULL << 41) | ((uint64_t)localDepth << 32) | subKey);
    for (uint64_t mainSlotIndex = 0; mainSlotIndex < ROERT_SLOTS_PER_BUCKET; mainSlotIndex += 8) {
        // 3. 使用 gather 指令加载 8 个 slot 的数据
        __m512i keys = _mm512_mask_i64gather_epi64(
          _mm512_setzero_si512(),                      // 初始值
          summary_mask,                                // 有效掩码
          v_offsets,                                   // 偏移量向量
          (void*)&mainBucket->counters[mainSlotIndex], // 基地址
          1                                            // 比例因子
        );
        __m512i v_keys = _mm512_and_si512(keys, v_42bits_mask);
        // uint64_t v_keys1_array[8];
        // uint64_t v_keys2_array[8];
        // _mm512_storeu_si512((__m512i*)v_keys1_array, keys);
        // _mm512_storeu_si512((__m512i*)v_keys2_array, v_keys);
        // for (int i = 0; i < 8; i++) {
        //     printf("主桶v_keys[%d] = 0x%016lx, 0x%016lx\n", i, v_keys1_array[i], v_keys2_array[i]);
        // }
        __mmask8 match_mask = _mm512_cmpeq_epi64_mask(v_keys, v_main_target);
        if (match_mask) {
            int j = __builtin_ctz(match_mask); // 找到最低位的 1
            // printf("search 主桶插槽 %lu slot->subKeyFields.subKey: 0x%08llx, value: %lu, nodeFlag: %lu, validFlag: %lu, migVersion: %ld, localDepth: %d, segmentIndex: %lu\n",
            //        mainSlotIndex + j, mainBucket->counters[mainSlotIndex + j].subKeyFields.subKey, mainBucket->counters[mainSlotIndex + j].valueFields.value, mainBucket->counters[mainSlotIndex + j].subKeyFields.nodeFlag, mainBucket->counters[mainSlotIndex + j].subKeyFields.migVersion, localDepth, segmentIndex);
            return &mainBucket->counters[mainSlotIndex + j];
        }
    }

    // for (uint64_t mainSlotIndex = 0; mainSlotIndex < ROERT_SLOTS_PER_BUCKET; mainSlotIndex += 4) {
    //     // 1. 【连续加载】直接读取 64 字节（4个subKey + 4个value），完全没有 Gather 的延迟！
    //     __m512i raw_data = _mm512_loadu_si512((const __m512i*)&mainBucket->counters[mainSlotIndex]);
    //     // 2. 【寄存器内洗牌】利用 vpermq 指令，只把 4 个 subKey 压缩到寄存器的低 256 位
    //     __m512i keys = _mm512_permutexvar_epi64(shuffle_subkeys, raw_data);
    //     // 3. 【位运算与并行比较】
    //     __m512i v_keys = _mm512_and_si512(keys, v_42bits_mask);
    //     // 因为我们只提取了 4 个有效 key，所以比较时只需要看低 4 位的掩码 (0x0F)
    //     __mmask8 match_mask = _mm512_cmpeq_epi64_mask(v_keys, v_main_target) & 0x0F;
    //     if (match_mask) {
    //         int j = __builtin_ctz(match_mask); // 找到命中的位置 (0-3)
    //         return &mainBucket->counters[mainSlotIndex + j];
    //     }
    // }

    // 主桶也没有找到，slot 为 nullptr
    return nullptr;
}

std::vector<uint64_t> roert_insert_segment_split_time_ = {};
std::vector<uint64_t> roert_insert_directory_grow_time_ = {};
std::vector<uint64_t> roert_per_insert_node_update_latency_ = {};

OperationResults ROERTNode::put(uint64_t subKey, uint64_t value, ROERTSegmentNode* segmentNode, ROERTSegment* segment, ROERTSlotKeyValue* slot,
                                uint64_t segmentIndex, uint64_t _sharedBucketOffset, uint16_t _fingerprint, int8_t _isShareOrMainFlag, uint8_t _nodeFlag, uint64_t* beforeAddress) {
    // 判断 slot 是否为空，若为空则需要节点扩展
    if (slot == nullptr) {
        // 情况1：段局部深度小于全局深度，只需进行段分裂
        if (ROERT_LIKELY(segmentNode->localDepth < this->directoryNode.directory.globalDepth)) {
            // printf("情况1：段局部深度小于全局深度，只需进行段分裂\n");
            // auto start_time = std::chrono::high_resolution_clock::now();

            // 创建新段
            ROERTSegment* newSegment = NewROERTSegment();

            const uint8_t globalDepth = this->directoryNode.directory.globalDepth;
            const uint8_t localDepth = segmentNode->localDepth;

            // 获取关联的段节点个数
            const uint64_t segmentNodeNum = 1ULL << (globalDepth - localDepth);
            const uint64_t tmpNodeNum = segmentNodeNum >> 1;

            // 计算迁移段索引起始位置，将 segmentIndex 的低 (globalDepth - localDepth) 位置为 0
            const uint64_t segmentIndexStart = segmentIndex & (~(segmentNodeNum - 1));
            const uint64_t segmentIndexMid = segmentIndexStart + tmpNodeNum;

            // 计算迁移的共享桶在段中的起始位置，迁移后一半的共享桶
            const uint8_t dstSharedBucketOffset = (_sharedBucketOffset + ROERT_SHARED_BUCKETS_PER_SEGMENT) % ROERT_SEGMENT_SIZE;
            const uint8_t migSharedBucketPositionStart = _sharedBucketOffset + ROERT_MIG_SHARED_BUCKET_NUM;
            const uint8_t migMainBucketPositionStart =
              ((_sharedBucketOffset + ROERT_SHARED_BUCKETS_PER_SEGMENT) % ROERT_SEGMENT_SIZE + (migSharedBucketPositionStart % ROERT_SHARED_BUCKETS_PER_SEGMENT) * 2) % ROERT_SEGMENT_SIZE;

            // 获取段指针
            const int8_t _migVersion = localDepth + 1;
            const int8_t sharedBucketVersion = localDepth - 1;
            ROERTBucket *migSharedBucket, *migMainBucket;
            uint64_t newMainBucketIndex, newMainBucketPosition;
            uint64_t dstMainBucketIndex, dstMainBucketPosition;

            uint8_t newMainBucketCounter[ROERT_MAIN_BUCKETS_PER_SEGMENT] = {}, dstMainBucketCounter[ROERT_MAIN_BUCKETS_PER_SEGMENT] = {};

            // 使用静态缓冲池
            ROERTBucket* _migSharedBuckets = ROERTNode::migration_buffer_pool;

            // 打印当前段和新段的地址
            // printf("_sharedBucketOffset: %lu, dstSharedBucketOffset: %lu, segmentIndexStart: %lu, segmentNodeNum: %lu, segmentIndex: %lu,  globalDepth: %lu, localDepth: %lu, migSharedBucketPositionStart: %lu, migMainBucketPositionStart: %lu\n",
            //        _sharedBucketOffset, dstSharedBucketOffset, segmentIndexStart, segmentNodeNum, segmentIndex,
            //        globalDepth, localDepth, migSharedBucketPositionStart, migMainBucketPositionStart);

            // 直接复制主桶的数据到新段的共享桶中
            memcpy(&newSegment->buckets[_sharedBucketOffset],
                   &segment->buckets[migMainBucketPositionStart], sizeof(ROERTBucket) * ROERT_MIG_MAIN_BUCKET_NUM);

            // 将需要迁移的共享桶复制到栈分配数组中
            memcpy(_migSharedBuckets, &segment->buckets[_sharedBucketOffset], sizeof(ROERTBucket) * ROERT_MIG_SHARED_BUCKET_NUM);

            // 循环遍历需要迁移的共享桶
            // for (int64_t i = ROERT_SHARED_BUCKETS_PER_SEGMENT - 1; i >= 0; i--) {
            //     if (i >= ROERT_MIG_SHARED_BUCKET_NUM) {
            //         // 循环遍历共享桶中的每个 slot
            //         for (int64_t j = 0; j < ROERT_SLOTS_PER_BUCKET; j++) {
            //             auto* slot = &segment->buckets[_sharedBucketOffset + i].counters[j];
            //             if (isInvalidSharedSlot(slot, localDepth)) {
            //                 break;
            //             }
            //             uint64_t subKey = slot->subKeyFields.subKey;
            //             // 使用子键的 MSB 作为段索引
            //             _migSegmentIndex = ROERT_GET_SEGMENT_NUMBER(subKey, ROERT_NODE_SPAN, globalDepth);
            //             // 如果共享桶的 slot 无效，则跳出循环
            //             if (_migSegmentIndex >= segmentIndexMid) {
            //                 // 如果 slot 有效，则迁移到新段中的主桶区
            //                 // printf("情况1：迁移共享桶 migSharedBucket->counters[%lu] key: 0x%08llx, value: %lu, migVersion: %lu, validFlag: %lu, migSegmentIndex: %lu\n",
            //                 //        j, migSharedBucket->counters[j].subKeyFields.subKey, migSharedBucket->counters[j].valueFields.value, migSharedBucket->counters[j].subKeyFields.migVersion, migSharedBucket->counters[j].subKeyFields.validFlag, _migSegmentIndex);
            //                 // 计算主桶索引：使用子键的高位索引主桶
            //                 newMainBucketIndex = ROERT_GET_MAIN_BUCKET_INDEX(subKey, _migVersion, ROERT_MAIN_BUCKETS_MAX_BITS);
            //                 // 计算新段中目标主桶位置
            //                 newMainBucketPosition = calcBucketPosition(_sharedBucketOffset, newMainBucketIndex);
            //                 size_t slotIndex = newMainBucketCounter[newMainBucketIndex & ROERT_MAIN_BUCKETS_PER_SEGMENT_MASK]++;
            //                 // 复制子键和值到新主桶的 slot
            //                 newSegment->buckets[newMainBucketPosition].counters[slotIndex] = *slot;
            //                 newSegment->buckets[newMainBucketPosition].counters[slotIndex].subKeyFields.migVersion = _migVersion;
            //             }
            //         }
            //     } else {
            //         // 循环遍历共享桶中的每个 slot
            //         for (int64_t j = 0; j < ROERT_SLOTS_PER_BUCKET; j++) {
            //             auto* slot = &_migSharedBuckets[i].counters[j];
            //             if (isInvalidSharedSlot(slot, localDepth)) {
            //                 break;
            //             }
            //             uint64_t subKey = slot->subKeyFields.subKey;
            //             // 使用子键的 MSB 作为段索引
            //             _migSegmentIndex = ROERT_GET_SEGMENT_NUMBER(subKey, ROERT_NODE_SPAN, globalDepth);
            //             // 如果共享桶的 slot 无效，则跳出循环
            //             if (_migSegmentIndex < segmentIndexMid) {
            //                 // printf("情况1：迁移当前共享桶数据 migSharedBucket->counters[%lu] key: 0x%08llx, value: %lu, migVersion: %lu, migSegmentIndex: %lu\n",
            //                 //        j, migSharedBucket->counters[j].subKeyFields.subKey, migSharedBucket->counters[j].valueFields.value, migSharedBucket->counters[j].subKeyFields.migVersion, _migSegmentIndex);
            //                 // 计算主桶索引：使用子键的高位索引主桶
            //                 dstMainBucketIndex = ROERT_GET_MAIN_BUCKET_INDEX(subKey, _migVersion, ROERT_MAIN_BUCKETS_MAX_BITS);
            //                 // 计算当前段中目标主桶位置
            //                 dstMainBucketPosition = calcBucketPosition(dstSharedBucketOffset, dstMainBucketIndex);
            //                 size_t slotIndex = dstMainBucketCounter[dstMainBucketIndex & ROERT_MAIN_BUCKETS_PER_SEGMENT_MASK]++;
            //                 // 复制子键和值到当前主桶
            //                 segment->buckets[dstMainBucketPosition].counters[slotIndex] = *slot;
            //                 segment->buckets[dstMainBucketPosition].counters[slotIndex].subKeyFields.migVersion = _migVersion;
            //             }
            //         }
            //     }
            // }

            // printf("_sharedBucketOffset + migMainBucketPositionStart %% ROERT_SHARED_BUCKETS_PER_SEGMENT: %lu\n", _sharedBucketOffset + migMainBucketPositionStart % ROERT_SHARED_BUCKETS_PER_SEGMENT);

            // 循环遍历需要迁移的共享桶
            for (size_t i = 0; i < ROERT_MIG_SHARED_BUCKET_NUM; i++) {
                // 循环遍历共享桶中的每个 slot
                for (size_t j = 0; j < ROERT_SLOTS_PER_BUCKET; j++) {
                    auto* slot = &segment->buckets[migSharedBucketPositionStart + i].counters[j];
                    // if (slot->subKeyFields.validFlag == 0 || (localDepth != ROERT_INIT_LOCAL_DEPTH && slot->subKeyFields.migVersion != sharedBucketVersion)) {
                    if (slot->subKeyFields.validFlag == 0 || slot->subKeyFields.migVersion != sharedBucketVersion) {
                        break;
                    }
                    uint64_t subKey = slot->subKeyFields.subKey;
                    // 如果共享桶的 slot 无效，则跳出循环
                    if (ROERT_GET_SEGMENT_NUMBER(subKey, ROERT_NODE_SPAN, globalDepth) >= segmentIndexMid) {
                        // if (slot->subKeyFields.migVersion == sharedBucketVersion && ROERT_GET_SEGMENT_NUMBER(subKey, ROERT_NODE_SPAN, globalDepth) >= segmentIndexMid) {
                        // 如果 slot 有效，则迁移到新段中的主桶区
                        // printf("情况1：迁移共享桶 %lu 数据 _migSharedBuckets[%lu].counters[%lu] key: 0x%08llx, value: %lu, migVersion: %d, segmentIndex: %lu, localDepth: %d, sharedBucketVersion: %d\n",
                        //        i, i, j, slot->subKeyFields.subKey, slot->valueFields.value, slot->subKeyFields.migVersion, segmentIndex, localDepth, sharedBucketVersion);
                        // 计算主桶索引：使用子键的高位索引主桶
                        newMainBucketIndex = ROERT_GET_MAIN_BUCKET_INDEX(subKey, _migVersion, ROERT_MAIN_BUCKETS_MAX_BITS);
                        // 计算新段中目标主桶位置
                        newMainBucketPosition = calcBucketPosition(_sharedBucketOffset, newMainBucketIndex);
                        size_t slotIndex = newMainBucketCounter[newMainBucketIndex & ROERT_MAIN_BUCKETS_PER_SEGMENT_MASK]++;
                        // 复制子键和值到新主桶的 slot
                        newSegment->buckets[newMainBucketPosition].counters[slotIndex] = *slot;
                        newSegment->buckets[newMainBucketPosition].counters[slotIndex].subKeyFields.migVersion = _migVersion;
                    }
                }
            }

            // 循环遍历当前段的共享桶
            for (size_t i = 0; i < ROERT_MIG_SHARED_BUCKET_NUM; i++) {
                // 循环遍历共享桶中的每个 slot
                for (size_t j = 0; j < ROERT_SLOTS_PER_BUCKET; j++) {
                    auto* slot = &_migSharedBuckets[i].counters[j];
                    // if (slot->subKeyFields.validFlag == 0 || (localDepth != ROERT_INIT_LOCAL_DEPTH && slot->subKeyFields.migVersion != sharedBucketVersion)) {
                    if (slot->subKeyFields.validFlag == 0 || slot->subKeyFields.migVersion != sharedBucketVersion) {
                        break;
                    }
                    uint64_t subKey = slot->subKeyFields.subKey;
                    // 如果共享桶的 slot 无效，则跳出循环
                    if (ROERT_GET_SEGMENT_NUMBER(subKey, ROERT_NODE_SPAN, globalDepth) < segmentIndexMid) {
                        // if (slot->subKeyFields.migVersion == sharedBucketVersion && ROERT_GET_SEGMENT_NUMBER(subKey, ROERT_NODE_SPAN, globalDepth) < segmentIndexMid) {
                        // printf("情况1：迁移当前共享桶数据 migSharedBucket->counters[%lu] key: 0x%08llx, value: %lu, migVersion: %lu, migSegmentIndex: %lu\n",
                        //        j, migSharedBucket->counters[j].subKeyFields.subKey, migSharedBucket->counters[j].valueFields.value, migSharedBucket->counters[j].subKeyFields.migVersion, _migSegmentIndex);
                        // 计算主桶索引：使用子键的高位索引主桶
                        dstMainBucketIndex = ROERT_GET_MAIN_BUCKET_INDEX(subKey, _migVersion, ROERT_MAIN_BUCKETS_MAX_BITS);
                        // 计算当前段中目标主桶位置
                        dstMainBucketPosition = calcBucketPosition(dstSharedBucketOffset, dstMainBucketIndex);
                        size_t slotIndex = dstMainBucketCounter[dstMainBucketIndex & ROERT_MAIN_BUCKETS_PER_SEGMENT_MASK]++;
                        // 复制子键和值到当前主桶
                        segment->buckets[dstMainBucketPosition].counters[slotIndex] = *slot;
                        segment->buckets[dstMainBucketPosition].counters[slotIndex].subKeyFields.migVersion = _migVersion;
                    }
                }
            }

            // 持久化新段到 NVM
            clflush((char*)newSegment, sizeof(ROERTSegment));

            // 持久化从当前段迁移位置开始的64个主桶到 NVM
            if (dstSharedBucketOffset == ROERT_SHARED_BUCKETS_OFFSET) {
                clflush((char*)segment + ROERT_SHARED_BUCKETS_PER_SEGMENT, sizeof(ROERTBucket) * ROERT_MAIN_BUCKETS_PER_SEGMENT);
            } else if (dstSharedBucketOffset == ROERT_MAIN_BUCKETS_PER_SEGMENT) {
                clflush((char*)segment, sizeof(ROERTBucket) * ROERT_MAIN_BUCKETS_PER_SEGMENT);
            } else {
                clflush((char*)segment, sizeof(ROERTBucket) * ROERT_SHARED_BUCKETS_PER_SEGMENT);
                clflush((char*)segment + (dstSharedBucketOffset + ROERT_SHARED_BUCKETS_PER_SEGMENT), sizeof(ROERTBucket) * ROERT_SHARED_BUCKETS_PER_SEGMENT);
            }

            // clflush((char*)segment, sizeof(ROERTSegment));

            // 循环更新段目录节点的局部深度、共享桶起始偏移、段指针
            for (size_t i = 0; i < tmpNodeNum; i++) {
                ROERTSegmentNode* newSegNode = reinterpret_cast<ROERTSegmentNode*>(ROERT_GET_SEGMENT_POSITION(&this->directoryNode, (segmentIndexStart + tmpNodeNum + i)));
                ROERTSegmentNode* oldSegNode = reinterpret_cast<ROERTSegmentNode*>(ROERT_GET_SEGMENT_POSITION(&this->directoryNode, (segmentIndexStart + i)));
                newSegNode->localDepth++;
                newSegNode->segmentPtr = reinterpret_cast<uint64_t>(newSegment);
                oldSegNode->localDepth++;
                oldSegNode->sharedBucketOffset = dstSharedBucketOffset == 0 ? 0 : dstSharedBucketOffset - 1;
            }

            // 持久化目录节点更改到 NVM
            clflush((char*)reinterpret_cast<ROERTSegmentNode*>(ROERT_GET_SEGMENT_POSITION(&this->directoryNode, segmentIndexStart)), sizeof(ROERTSegmentNode) * segmentNodeNum);
            // printf("segmentNodeNum: %d\n", segmentNodeNum);

            // auto end_time = std::chrono::high_resolution_clock::now();
            // roert_insert_segment_split_time_.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count());

            // 重新尝试插入当前子键值对
            this->putSegNode(subKey, value, _fingerprint, beforeAddress, _nodeFlag);

            return OperationResults::Success;
        }
        // 情况2：段局部深度等于全局深度，需要扩展目录节点
        else if (ROERT_LIKELY(segmentNode->localDepth == this->directoryNode.directory.globalDepth)) {
            // printf("情况2：段局部深度等于全局深度，需要扩展目录节点，需进行目录分裂\n");
            // auto start_time = std::chrono::high_resolution_clock::now();
            // auto end_time = std::chrono::high_resolution_clock::now();
            // roert_insert_directory_grow_time_.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count());

            const uint8_t globalDepth = this->directoryNode.directory.globalDepth;
            const uint8_t localDepth = segmentNode->localDepth;

            // 创建新目录节点 ROERTNode
            ROERTNode* newNode = static_cast<ROERTNode*>(concurrency_fast_alloc(
              sizeof(ROERTNode) + sizeof(ROERTSegmentNode) * (1 << (globalDepth + 1))));

            // 初始化新目录节点的ROERTDirectory信息
            newNode->directoryNode.directory = ROERTDirectoryNode::ROERTDirectory(
              &this->directoryNode.directory,
              globalDepth + 1);

            const uint8_t newGlobalDepth = newNode->directoryNode.directory.globalDepth;

            newNode->directoryNode.capacity = 1 << newGlobalDepth;

            // 初始化新目录节点的头部
            newNode->directoryNode.header.initialize(&this->directoryNode.header, this->directoryNode.header.prefixLength, this->directoryNode.header.nodeHeaderDepth);

            // #pragma GCC unroll 4
            // 设置新目录节点的段节点类
            for (size_t i = 0; i < this->directoryNode.capacity; i++) {
                const ROERTSegmentNode* oldSegmentNode =
                  reinterpret_cast<const ROERTSegmentNode*>(ROERT_GET_SEGMENT_POSITION(&this->directoryNode, i));

                *reinterpret_cast<ROERTSegmentNode*>(
                  ROERT_GET_SEGMENT_POSITION(&newNode->directoryNode, (i * 2))) = *oldSegmentNode;
                *reinterpret_cast<ROERTSegmentNode*>(
                  ROERT_GET_SEGMENT_POSITION(&newNode->directoryNode, (i * 2 + 1))) = *oldSegmentNode;
            }

            // printf("newNode->directoryNode.capacity: %u, this->directoryNode.capacity: %u\n", newNode->directoryNode.capacity, this->directoryNode.capacity);

            // 创建新段
            ROERTSegment* newSegment = NewROERTSegment();

            ROERTSegmentNode *migSegmentNode, *newSegmentNode;

            // 获取迁移段节点指针
            migSegmentNode = reinterpret_cast<ROERTSegmentNode*>(
              ROERT_GET_SEGMENT_POSITION(&newNode->directoryNode, (segmentIndex * 2)));

            // 获取新段节点指针
            newSegmentNode = reinterpret_cast<ROERTSegmentNode*>(
              ROERT_GET_SEGMENT_POSITION(&newNode->directoryNode, (segmentIndex * 2 + 1)));

            // 计算迁移的共享桶在段中的起始位置，迁移后一半的共享桶
            const uint8_t dstSharedBucketOffset = (_sharedBucketOffset + ROERT_SHARED_BUCKETS_PER_SEGMENT) % ROERT_SEGMENT_SIZE;
            const uint8_t migSharedBucketPositionStart = _sharedBucketOffset + ROERT_MIG_SHARED_BUCKET_NUM;
            const uint8_t migMainBucketPositionStart =
              ((_sharedBucketOffset + ROERT_SHARED_BUCKETS_PER_SEGMENT) % ROERT_SEGMENT_SIZE + (migSharedBucketPositionStart % ROERT_SHARED_BUCKETS_PER_SEGMENT) * 2) % ROERT_SEGMENT_SIZE;

            // 获取需要进行迁移的段指针
            const uint8_t _migVersion = localDepth + 1;
            const int8_t sharedBucketVersion = localDepth - 1;
            uint64_t newSegmentIndex = segmentIndex * 2 + 1, _segmentIndex = segmentIndex * 2;
            uint64_t newMainBucketIndex, newMainBucketPosition;
            uint64_t dstMainBucketIndex, dstMainBucketPosition;

            // 定义一个计数数组，用于统计每个主桶的 slot 数量
            uint8_t newMainBucketCounter[ROERT_MAIN_BUCKETS_PER_SEGMENT] = {}, dstMainBucketCounter[ROERT_MAIN_BUCKETS_PER_SEGMENT] = {};

            ROERTBucket* _migSharedBuckets = ROERTNode::migration_buffer_pool;

            // printf("_sharedBucketOffset: %lu, newSharedBucketOffset: %lu, dstSharedBucketOffset: %lu, newSegmentNode->sharedBucketOffset: %lu, newSegmentNode->localDepth: %lu, migSegmentNode->localDepth: %lu, segmentIndex: %lu,  globalDepth: %lu, migSharedBucketPositionStart: %lu, migMainBucketPositionStart: %lu\n",
            //        _sharedBucketOffset, newSharedBucketOffset, dstSharedBucketOffset, newSegmentNode->sharedBucketOffset, newSegmentNode->localDepth, migSegmentNode->localDepth,
            //        segmentIndex, newGlobalDepth, migSharedBucketPositionStart, migMainBucketPositionStart);

            // printf("newSharedBucketOffset + migMainBucketPositionStart %% ROERT_SHARED_BUCKETS_PER_SEGMENT: %lu\n", newSharedBucketOffset + migMainBucketPositionStart % ROERT_SHARED_BUCKETS_PER_SEGMENT);

            // 直接复制主桶的数据到新段的共享桶中
            memcpy(&newSegment->buckets[ROERT_SHARED_BUCKETS_OFFSET],
                   &segment->buckets[migMainBucketPositionStart], sizeof(ROERTBucket) * ROERT_MIG_MAIN_BUCKET_NUM);

            // printf("本次段扩容：旧段索引 %d -> 扩容后段索引 %d, 本次迁移：段索引 %d -> 段索引 %d\n", segmentIndex, segmentIndex * 2, segmentIndex * 2, segmentIndex * 2 + 1);

            // 将需要迁移的共享桶复制到栈分配数组中
            // memcpy(&_migSharedBuckets, &segment->buckets[_sharedBucketOffset], sizeof(ROERTBucket) * ROERT_MIG_SHARED_BUCKET_NUM);
            memcpy(_migSharedBuckets, &segment->buckets[_sharedBucketOffset], sizeof(ROERTBucket) * ROERT_MIG_SHARED_BUCKET_NUM);

            // 循环遍历需要迁移的共享桶及其关联的主桶
            for (size_t i = 0; i < ROERT_MIG_SHARED_BUCKET_NUM; i++) {
                // 循环遍历共享桶中的每个 slot
                for (size_t j = 0; j < ROERT_SLOTS_PER_BUCKET; j++) {
                    // 获取迁移的共享桶指针
                    auto* slot = &segment->buckets[migSharedBucketPositionStart + i].counters[j];
                    // printf("情况2：迁移共享桶---- migSharedBucket->counters[%lu] key: 0x%08llx, value: %lu, migVersion: %lu, newSegmentIndex: %lu\n",
                    //        j, migSharedBucket->counters[j].subKeyFields.subKey, migSharedBucket->counters[j].valueFields.value, migSharedBucket->counters[j].subKeyFields.migVersion, newSegmentIndex);
                    if (slot->subKeyFields.validFlag == 0 || slot->subKeyFields.migVersion != sharedBucketVersion) {
                        break;
                    }
                    uint64_t subKey = slot->subKeyFields.subKey;
                    // 使用子键的 MSB 作为段索引
                    // 如果共享桶的 slot 无效，则跳出循环
                    if (ROERT_GET_SEGMENT_NUMBER(subKey, ROERT_NODE_SPAN, newGlobalDepth) == newSegmentIndex) {
                        // if (slot->subKeyFields.migVersion == sharedBucketVersion && ROERT_GET_SEGMENT_NUMBER(subKey, ROERT_NODE_SPAN, newGlobalDepth) == newSegmentIndex) {
                        // 如果 slot 有效，则迁移到新段中的主桶区
                        // printf("情况2：迁移共享桶 %lu 数据 _migSharedBuckets[%lu].counters[%lu] key: 0x%08llx, value: %lu, migVersion: %d, segmentIndex: %lu, localDepth: %d, sharedBucketVersion: %d\n",
                        //        i, i, j, slot->subKeyFields.subKey, slot->valueFields.value, slot->subKeyFields.migVersion, segmentIndex, localDepth, sharedBucketVersion);
                        // 计算主桶索引：使用子键的高位索引主桶
                        newMainBucketIndex = ROERT_GET_MAIN_BUCKET_INDEX(subKey, _migVersion, ROERT_MAIN_BUCKETS_MAX_BITS);
                        // 计算新段中目标主桶位置
                        newMainBucketPosition = calcBucketPosition(ROERT_SHARED_BUCKETS_OFFSET, newMainBucketIndex);
                        size_t slotIndex = newMainBucketCounter[newMainBucketIndex & ROERT_MAIN_BUCKETS_PER_SEGMENT_MASK]++;
                        // 复制子键和值到新主桶, 更新迁移版本号
                        newSegment->buckets[newMainBucketPosition].counters[slotIndex] = *slot;
                        newSegment->buckets[newMainBucketPosition].counters[slotIndex].subKeyFields.migVersion = _migVersion;
                    }
                }
            }

            // 循环遍历当前段的共享桶
            for (size_t i = 0; i < ROERT_MIG_SHARED_BUCKET_NUM; i++) {
                // 循环遍历共享桶中的每个 slot
                for (size_t j = 0; j < ROERT_SLOTS_PER_BUCKET; j++) {
                    // 获取迁移的共享桶指针
                    auto* slot = &_migSharedBuckets[i].counters[j];
                    // if (slot->subKeyFields.validFlag == 0 || (localDepth != ROERT_INIT_LOCAL_DEPTH && slot->subKeyFields.migVersion != sharedBucketVersion)) {
                    if (slot->subKeyFields.validFlag == 0 || slot->subKeyFields.migVersion != sharedBucketVersion) {
                        break;
                    }
                    uint64_t subKey = slot->subKeyFields.subKey;
                    // 使用子键的 MSB 作为段索引
                    // 如果共享桶的 slot 无效，则跳出循环
                    if (ROERT_GET_SEGMENT_NUMBER(subKey, ROERT_NODE_SPAN, newGlobalDepth) == _segmentIndex) {
                        // if (slot->subKeyFields.migVersion == sharedBucketVersion && ROERT_GET_SEGMENT_NUMBER(subKey, ROERT_NODE_SPAN, newGlobalDepth) == _segmentIndex) {
                        // printf("情况2：迁移当前共享桶 %lu 数据 _migSharedBuckets[%lu].counters[%lu] key: 0x%08llx, value: %lu, migVersion: %d, segmentIndex: %lu, localDepth: %d, sharedBucketVersion: %d\n",
                        //        i, i, j, _migSharedBuckets[i].counters[j].subKeyFields.subKey, _migSharedBuckets[i].counters[j].valueFields.value, _migSharedBuckets[i].counters[j].subKeyFields.migVersion, segmentIndex, localDepth, sharedBucketVersion);
                        // 计算主桶索引：使用子键的高位索引主桶
                        dstMainBucketIndex = ROERT_GET_MAIN_BUCKET_INDEX(subKey, _migVersion, ROERT_MAIN_BUCKETS_MAX_BITS);
                        // 计算当前段中目标主桶位置
                        dstMainBucketPosition = calcBucketPosition(dstSharedBucketOffset, dstMainBucketIndex);
                        size_t slotIndex = dstMainBucketCounter[dstMainBucketIndex & ROERT_MAIN_BUCKETS_PER_SEGMENT_MASK]++;
                        // 复制子键和值到当前主桶, 更新迁移版本号
                        segment->buckets[dstMainBucketPosition].counters[slotIndex] = *slot;
                        segment->buckets[dstMainBucketPosition].counters[slotIndex].subKeyFields.migVersion = _migVersion;
                    }
                }
            }

            // 持久化新段到 NVM
            clflush((char*)newSegment, sizeof(ROERTSegment));

            // 持久化从当前段迁移位置开始的64个主桶到 NVM
            if (dstSharedBucketOffset == ROERT_SHARED_BUCKETS_OFFSET) {
                clflush((char*)segment + ROERT_SHARED_BUCKETS_PER_SEGMENT, sizeof(ROERTBucket) * ROERT_MAIN_BUCKETS_PER_SEGMENT);
            } else if (dstSharedBucketOffset == ROERT_MAIN_BUCKETS_PER_SEGMENT) {
                clflush((char*)segment, sizeof(ROERTBucket) * ROERT_MAIN_BUCKETS_PER_SEGMENT);
            } else {
                clflush((char*)segment, sizeof(ROERTBucket) * ROERT_SHARED_BUCKETS_PER_SEGMENT);
                clflush((char*)segment + (dstSharedBucketOffset + ROERT_SHARED_BUCKETS_PER_SEGMENT), sizeof(ROERTBucket) * ROERT_SHARED_BUCKETS_PER_SEGMENT);
            }

            // clflush((char*)segment, sizeof(ROERTSegment));

            // 更新新目录
            newSegmentNode->localDepth++;
            newSegmentNode->segmentPtr = reinterpret_cast<uint64_t>(newSegment);
            newSegmentNode->sharedBucketOffset = ROERT_SHARED_BUCKETS_OFFSET;

            // 更新当前迁移段节点目录
            migSegmentNode->localDepth++;
            migSegmentNode->sharedBucketOffset = dstSharedBucketOffset == 0 ? 0 : dstSharedBucketOffset - 1;

            // 持久化新节点到 NVM
            clflush((char*)newNode, sizeof(ROERTNode) + sizeof(ROERTSegmentNode) * newNode->directoryNode.capacity);

            // 更新前驱目录节点的指针并持久化
            if (newNode->directoryNode.header.nodeHeaderDepth != 0) {
                ((ROERTSlotKeyValue*)(*beforeAddress))->valueFields.value = (uint64_t)newNode;
                clflush((char*)(ROERTSlotKeyValue*)(*beforeAddress), sizeof(ROERTSlotKeyValue));
                // printf("前驱地址: %lu, 新节点地址: %lu, slot->subKeyFields.subKey: 0x%08llx, value: %lu\n",
                //        *beforeAddress, newNode, ((ROERTSlotKeyValue*)(*beforeAddress))->subKeyFields.subKey, ((ROERTSlotKeyValue*)(*beforeAddress))->valueFields.value);
            } else {
                *(ROERTNode**)(*beforeAddress) = newNode;
                clflush((char*)(*beforeAddress), sizeof(ROERTNode*));
                // printf("前驱地址: %lu, 新节点地址: %lu\n", *beforeAddress, newNode);
            }

            // printf("时间: %lu ns, 目录节点扩展: %lu\n", std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count(), roert_insert_directory_grow_time_.size());

            newNode->putSegNode(subKey, value, _fingerprint, beforeAddress, _nodeFlag);

            return OperationResults::Success;
        }
    } else { // 桶有可用空间，直接插入并持久化
        // 子键相同则更新值
        if (slot->subKeyFields.subKey == subKey && slot->subKeyFields.validFlag == 1) {
            // printf("桶有可用空间，直接插入，子键相同则更新值\n");

            slot->valueFields.value = value;
            // 刷新内存，确保写入可见
            clflush((char*)&slot->valueFields, sizeof(slot->valueFields));

        } else { // 新子键直接插入并持久化
            // printf("桶有可用空间，直接插入，新子键直接插入并持久化，子键: 0x%08llx, 值: %lu\n", subKey, value);

            // 设置值
            slot->valueFields.value = value;

            // 内存屏障：确保值写入可见
            mfence();

            // 设置子键
            slot->subKeyFields.subKey = subKey;
            // 设置有效位为1
            slot->subKeyFields.validFlag = 1;
            // 设置指纹
            // slot->subKeyFields.fingerprint = _fingerprint;

            // 设置迁移版本号为当前段的局部深度
            slot->subKeyFields.migVersion = segmentNode->localDepth - !_isShareOrMainFlag;
            // printf(" segmentNode->localDepth - !_isShareOrMainFlag: %d\n", segmentNode->localDepth - !_isShareOrMainFlag);
            // printf("插入子键: 0x%08llx, 值: %lu, migVersion: %d, validFlag: %lu, localDepth: %lu, isShareOrMainFlag: %lu\n",
            //        slot->subKeyFields.subKey, slot->valueFields.value, slot->subKeyFields.migVersion, slot->subKeyFields.validFlag, segmentNode->localDepth, _isShareOrMainFlag);
            // 设置节点标记为0（表示键值对）
            slot->subKeyFields.nodeFlag = _nodeFlag;

            // 刷新内存，确保写入可见
            clflush((char*)slot, sizeof(ROERTSlotKeyValue));
        }

        return OperationResults::Success;
    }
}

uint64_t ROERTNode::fingerprint(uint64_t key) {
    // 使用STEPH的懒分裂算法：动态调整指纹提取位置

    // 计算已使用的比特位数：当前键位置 + 子键长度
    // 这表示前缀匹配和子键索引总共使用的比特数
    // auto bitUsed = depth + ROERT_BUCKET_INDEX_BIT_NUM;

    // 如果已使用比特数超过懒分裂阈值，使用简单指纹（取低16位）
    // if (bitUsed >= ROERT_SPLIT_THRESHOLD) {
    return key & ((1ul << ROERT_FINGERPRINT_BITS) - 1);
    // }

    // 对齐到8位边界
    // auto alignment = bitUsed & (~(ROERT_FINGERPRINT_BIT_ALIGNMENT - 1ul));

    // 提取指纹：先将键左移对齐位数，然后右移48位取高16位
    // 这样可以从键的中间位置提取指纹，避免与段索引和桶索引重叠
    // return (key << (alignment)) >> 48ul;
}

uint64_t ROERTNode::stale_fingerprint(uint64_t key, uint8_t depth) {
    // 类似fingerprint，但调整已使用比特数
    auto bitUsed = depth + ROERT_MAIN_BUCKETS_MAX_BITS;

    // 如果已使用比特数超过懒分裂阈值，使用简单指纹（取低16位）
    if (bitUsed >= ROERT_SPLIT_THRESHOLD) {
        return key & ((1ul << ROERT_FINGERPRINT_BITS) - 1);
    }

    bitUsed = bitUsed >= 8 ? bitUsed - 8 : bitUsed;

    auto alignment = bitUsed & (~(ROERT_FINGERPRINT_BIT_ALIGNMENT - 1ul));

    return (key << (alignment)) >> 48ul;
}

ROERTBucket::ROERTBucket() {
    memset(this, 0, sizeof(ROERTBucket));

    for (int i = 0; i < ROERT_SLOTS_PER_BUCKET; i++) {
        counters[i].subKeyFields.migVersion = 1;
    }
}

ROERTBucket::~ROERTBucket() {
    // 桶的析构函数，不需要特殊清理
    // 所有slot都是内联存储的，不需要手动释放
}

ROERTSegment::ROERTSegment() {
    // 构造函数：不需要特殊初始化
    // buckets数组会在内存分配时自动构造
}

ROERTSegment::~ROERTSegment() {
    // 析构函数：不需要特殊清理
    // buckets数组会在内存释放时自动析构
}

ROERTSegmentNode::ROERTSegmentNode() {}

ROERTSegmentNode::~ROERTSegmentNode() {
    // 段节点的析构函数
    // 注意：桶的释放由上层逻辑控制
}

ROERTDirectoryNode::ROERTDirectoryNode() {}

ROERTDirectoryNode::~ROERTDirectoryNode() {
    // 析构函数：不需要特殊清理
    // 所有成员都是内联存储的，不需要手动释放
}

ROERTNode::ROERTNode() {}

ROERTNode::~ROERTNode() {
    // 析构函数：不需要特殊清理
    // directoryNode成员会在对象销毁时自动析构
}

void ROERTSegment::initialize() {
    // 初始化段内的所有桶
    // for (int i = 0; i < ROERT_SEGMENT_SIZE; i++) {
    //     // 使用placement new调用每个桶的构造函数
    //     new (&buckets[i]) ROERTBucket();
    // }

    memset(buckets, 0, sizeof(ROERTBucket) * ROERT_SEGMENT_SIZE);
}

void ROERTHeader::initialize(const ROERTHeader* oldHeader, unsigned char prefixLength, unsigned char nodeHeaderDepth) {
    if (oldHeader != nullptr) {
        assignPrefix(oldHeader->prefixArray, prefixLength);
    }
    this->prefixLength = prefixLength;
    this->nodeHeaderDepth = nodeHeaderDepth;
}

void ROERTDirectoryNode::initialize(uint8_t globalDepth) {
    // 初始化目录大小 = 2^globalDepth
    this->capacity = 1 << globalDepth;

    // 初始化 ROERTDirectory
    this->directory.globalDepth = globalDepth;
    // this->directory.nodeType = static_cast<uint8_t>(ROERTDirectoryNode::ROERTNodeType::ROERT_NODE_TYPE_INTERNAL);
    // this->directory.isLocked = 0;

    // 分配 globalSegment 空间并初始化 ROERTSegmentNode
    // ROERTKeyValue* globalSegment = static_cast<ROERTKeyValue*>(
    //   concurrency_fast_alloc(sizeof(ROERTKeyValue) * ROERT_BUCKET_SIZE));

    // // 初始化 globalSegment 中的每个 ROERTKeyValue
    // for (uint64_t i = 0; i < ROERT_BUCKET_SIZE; i++) {
    //     globalSegment[i].key = 0;
    //     globalSegment[i].value = -1;
    // }

    // // 设置目录节点的全局段指针
    // this->directory.globalSegmentPtr = reinterpret_cast<uint64_t>(globalSegment);
    // this->directory.globalSegmentPtr = 0;
}

void ROERTNode::initialize(unsigned char nodeHeaderDepth, unsigned char globalDepth) {
    // 初始化头部信息
    this->directoryNode.header.initialize(nullptr, ROERT_INIT_PREFIX_LENGTH, nodeHeaderDepth);

    // 初始化目录信息
    this->directoryNode.initialize(globalDepth);

    // 直接设置ROERTSegmentNode的成员变量
    ROERTSegmentNode* segmentNode = reinterpret_cast<ROERTSegmentNode*>(ROERT_GET_SEGMENT_POSITION(this, 0));
    segmentNode->sharedBucketOffset = ROERT_SHARED_BUCKETS_OFFSET;           // 第一个共享桶的偏移 = 63
    segmentNode->segmentPtr = reinterpret_cast<uint64_t>(NewROERTSegment()); // 分配新段并设置指针
}

// ==================== 创建函数实现 ====================
ROERTKeyValue* NewROERTKeyValue(uint64_t key, uint64_t value) {
    // 分配 ROERTKeyValue 内存
    ROERTKeyValue* _newKV = static_cast<ROERTKeyValue*>(concurrency_fast_alloc(sizeof(ROERTKeyValue)));

    _newKV->key = key;
    _newKV->value = value;

    return _newKV;
}

ROERTBucket* NewROERTBucket() {
    ROERTBucket* newBucket = new ROERTBucket();
    return newBucket;
}

ROERTSegment* NewROERTSegment() {
    // 分配 ROERTSegment 内存
    ROERTSegment* _newSegment = static_cast<ROERTSegment*>(concurrency_fast_alloc(sizeof(ROERTSegment)));
    // printf("sizeof(ROERTSegment): %llu\n", sizeof(ROERTSegment));

    // 初始化段
    // _newSegment->initialize();

    // 刷新内存，确保写入可见
    // clflush((char*)_newSegment, sizeof(ROERTSegment));

    return _newSegment;
}

ROERTNode* NewROERTNode(unsigned char nodeHeaderDepth, unsigned char globalDepth) {
    ROERTNode* _newNode = static_cast<ROERTNode*>(concurrency_fast_alloc(
      sizeof(ROERTNode) + sizeof(ROERTSegmentNode) * (1 << globalDepth)));

    _newNode->initialize(nodeHeaderDepth, globalDepth);

    // 刷新内存，确保写入可见
    // clflush((char*)_newNode, sizeof(ROERTNode) + sizeof(ROERTSegmentNode) * (1 << globalDepth));

    return _newNode;
}

} // namespace roert
