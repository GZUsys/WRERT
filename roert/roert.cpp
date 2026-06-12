#include <iostream>
#include <cstring>
#include <algorithm>
#include <vector>
#include <queue>
#include <chrono>    // 添加高精度时间测量头文件
#include <algorithm> // 添加sort函数支持
#include <vector>    // 添加vector支持
#include <iomanip>   // 添加格式化输出支持
#include <unistd.h>
#include <cstdint>

#include "roert.h"

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

OperationResults ROERT::insert(uint64_t key, uint64_t value, ROERTNode* node, int keyLength, ROERTKeyValue* _kv) {
    // 初始化当前节点为传入节点或根节点
    ROERTNode* currentNode = node;

    // 如果未指定节点，使用根节点
    if (ROERT_UNLIKELY(node == nullptr)) {
        currentNode = root;
    }

    uint32_t currentKeyPosition = keyLength; // 当前处理的键位置（字节偏移）
    // uint32_t remainingKeyLength = ROERT_KEY_MAX_BYTES - currentKeyPosition; // 剩余键长度（字节）
    uint64_t beforeAddress = (uint64_t)&root; // 记录前驱节点的地址

    uint32_t arrayMatchLength = 0; // 记录数组匹配长度
    uint8_t nodeFlag;              // 记录节点标志
    uint16_t fingerprint = 0;      // 计算指纹

    // if (keyLength == 0) {
    //     fingerprint = currentNode->fingerprint(key);
    // }

    // if (key == 6789187714496537665 || key == 2789205980574075690) {
    // printf("=======================================\n");
    // printf("insert key: 0x%016llx, (十进制)%llu, value: %llu, root: %lu, beforeAddress: %lu\n",
    //        key, key, value, root, beforeAddress);
    // printf("=======================================\n");
    // }

    // 匹配的前缀长度
    int prefixMatchLength;

    // 主循环：处理键的每个子键
    while (currentKeyPosition < ROERT_KEY_MAX_BYTES) {
        // 步骤1: 计算匹配的前缀
        // prefixMatchLength = currentNode->directoryNode.header.computePrefix(key, currentKeyPosition * ROERT_SIZE_OF_CHAR, &arrayMatchLength);
        prefixMatchLength = currentNode->directoryNode.header.computePrefix(key, currentKeyPosition << 2, &arrayMatchLength);

        // printf("prefixMatchLength: %d\n", prefixMatchLength);
        // if (prefixMatchLength != -1) {
        //     // 计算数组匹配长度
        //     arrayMatchLength = prefixMatchLength * ROERT_NODE_SUBKEY_MAX_BYTES;
        // }
        // if (key == 18014501588721665) {
        //     printf("前缀匹配长度 prefixMatchLength: %d, 数组匹配长度 arrayMatchLength: %d, 前缀长度 prefixLength: %d, nodeHeaderDepth: %d, 前驱地址: %lu\n",
        //            prefixMatchLength, arrayMatchLength, currentNode->directoryNode.header.prefixLength, currentNode->directoryNode.header.nodeHeaderDepth, beforeAddress);
        // }

        // 情况0：前缀数组为空
        if (prefixMatchLength == -1) {
            // printf("情况0: 前缀数组为空 - 为空前缀分配新前缀\n");

            // 设置前缀长度为 ROERT_NODE_PREFIX_MAX_BYTES
            // currentNode->directoryNode.header.prefixLength = std::min(remainingKeyLength,
            //                                                           static_cast<uint32_t>(ROERT_NODE_PREFIX_MAX_BYTES));

            currentNode->directoryNode.header.prefixLength = ROERT_NODE_PREFIX_MAX_BYTES / ROERT_NODE_SUBKEY_MAX_BYTES;

            // currentNode->directoryNode.header.nodeHeaderDepth = currentNode->directoryNode.header.prefixLength;

            // 为空前缀分配新前缀
            currentNode->directoryNode.header.assignPrefix(key, currentKeyPosition * ROERT_SIZE_OF_CHAR);

            continue;
        }

        // 情况1: 前缀匹配成功，到达键的末尾 - 直接插入当前 globalSegment
        else if (currentKeyPosition + arrayMatchLength == ROERT_KEY_MAX_BYTES) {
            // printf("情况1: 前缀匹配成功，到达键的末尾 - 直接插入当前 globalSegment\n");

            // 获取全局段指针
            ROERTKeyValue* globalSegment = reinterpret_cast<ROERTKeyValue*>(currentNode->directoryNode.directory.globalSegmentPtr);

            // 直接插入全局段
            currentNode->putDirNode(globalSegment, key, value);

            return OperationResults::Success;
        }

        // 情况2：前缀长度为与匹配长度相等 - 直接插入当前节点或者其子节点
        else if (currentNode->directoryNode.header.prefixLength == prefixMatchLength) {
            // 计算当前处理键的长度和剩余长度, 去掉进行前缀匹配的长度
            // currentKeyPosition += currentNode->directoryNode.header.prefixLength * ROERT_NODE_SUBKEY_MAX_BYTES;
            currentKeyPosition += currentNode->directoryNode.header.prefixLength << 2;

            // 计算子键：从当前键位置提取 ROERT_NODE_SUBKEY_LENGTH 位的子键
            // const uint64_t subKey = ROERT_GET_SUBKEY(key, currentKeyPosition * ROERT_SIZE_OF_CHAR, ROERT_NODE_SPAN);
            const uint64_t subKey = ROERT_GET_SUBKEY(key, (currentKeyPosition << 3), ROERT_NODE_SPAN);

            const uint8_t globalDepth = currentNode->directoryNode.directory.globalDepth;

            // 使用子键的 MSB 作为段索引
            const uint64_t segmentIndex = ROERT_GET_SEGMENT_NUMBER(subKey, ROERT_NODE_SPAN, globalDepth);

            // 获取段节点指针
            ROERTSegmentNode* segmentNode = reinterpret_cast<ROERTSegmentNode*>(
              ROERT_GET_SEGMENT_POSITION(&currentNode->directoryNode, segmentIndex));

            // 获取段指针
            ROERTSegment* segment = reinterpret_cast<ROERTSegment*>(segmentNode->segmentPtr);

            // 共享桶在段中的位置：从共享桶偏移开始
            const uint8_t sharedBucketOffset = segmentNode->sharedBucketOffset == 0 ? 0 : segmentNode->sharedBucketOffset + 1;
            const uint8_t localDepth = segmentNode->localDepth;

            // 计算共享桶索引、共享桶位置、共享桶指针
            const uint64_t sharedBucketIndex = ROERT_GET_SHARED_BUCKET_INDEX(subKey, localDepth, ROERT_MAIN_BUCKETS_MAX_BITS);
            const uint64_t sharedBucketPosition = sharedBucketOffset + (sharedBucketIndex & ROERT_SHARED_BUCKETS_PER_SEGMENT_MASK);
            ROERTBucket* sharedBucket = &segment->buckets[sharedBucketPosition];
            const int8_t sharedBucketVersion = localDepth - 1;

            uint64_t shareSlotIndex, shareSlotIndex1, mainSlotIndex;
            // 标志位：0 表示共享桶，1 表示主桶
            int8_t _isShareOrMainFlag = 0;
            ROERTSlotKeyValue* slot;
            bool _isValidSharedSlot = false, _isValidMainSlot = false, _isFindFlag = false;

            // auto start_time = std::chrono::high_resolution_clock::now();

            // 共享桶未满，遍历共享桶查找可用 slot
            // for (shareSlotIndex = 0; shareSlotIndex < ROERT_SLOTS_PER_BUCKET; ++shareSlotIndex) {
            //     // 获取共享桶 slot 指针
            //     slot = &sharedBucket->counters[shareSlotIndex];
            //     // if (key == 6789187714496537665 || key == 2789205980574075690) {
            //     // printf("共享桶插槽 %lu, subKey: 0x%08llx, slot->subKeyFields: 0x%016llx, slot->subKeyFields.subKey: 0x%08llx, value: %lu, validFlag: %lu, migVersion: %d, sharedBucketVersion: %d, localDepth: %lu, segmentIndex: %lu\n",
            //     //        shareSlotIndex1, subKey, slot->subKeyFields, slot->subKeyFields.subKey, slot->valueFields.value, slot->subKeyFields.validFlag, slot->subKeyFields.migVersion, sharedBucketVersion, localDepth, segmentIndex);
            //     // }
            //     // if (slot->subKeyFields.validFlag == 0 || slot->subKeyFields.migVersion != sharedBucketVersion) {
            //     //     _isValidSharedSlot = true;
            //     //     break;
            //     // } else if (slot->subKeyFields.subKey == subKey) {
            //     //     _isFindFlag = true;
            //     //     break; // 子键匹配，跳出循环
            //     // }
            //     if (slot->subKeyFields.validFlag == 1 && slot->subKeyFields.migVersion == sharedBucketVersion) {
            //         if (slot->subKeyFields.subKey == subKey) {
            //             _isFindFlag = true;
            //             break; // 找到旧值，跳出
            //         }
            //     } else {
            //         _isValidSharedSlot = true;
            //         break;
            //     }
            // }

            // 遍历共享桶查找匹配项 - AVX-512加速
            const __m512i v_subKey_target = _mm512_set1_epi64(subKey);
            const __m512i v_shared_validFlag_migVersion_target = _mm512_set1_epi64((1ULL << 41) | ((uint64_t)(sharedBucketVersion & ROERT_MIGVERSION_BITS) << 32));
            const __m512i v_validFlag_migVersion_mask = _mm512_set1_epi64(ROERT_VALIDFLAG_MIGVERSION_MASK);
            const __m512i v_subKey_mask = _mm512_set1_epi64(ROERT_SUBKEY_MASK);

            // auto start_time = std::chrono::high_resolution_clock::now();

            const __m512i v_offsets = _mm512_load_si512((__m512i*)offsets);
            for (shareSlotIndex = 0; shareSlotIndex < ROERT_SLOTS_PER_BUCKET; shareSlotIndex += 8) {
                __m512i keys = _mm512_mask_i64gather_epi64(
                  _mm512_setzero_si512(),                         // 初始值
                  summary_mask,                                   // 有效掩码
                  v_offsets,                                      // 偏移量向量
                  (void*)&sharedBucket->counters[shareSlotIndex], // 基地址
                  1                                               // 比例因子
                );
                __m512i v_validFlag_migVersion_keys = _mm512_and_si512(keys, v_validFlag_migVersion_mask);
                __mmask8 validFlag_migVersion_match_mask = _mm512_cmpeq_epi64_mask(v_validFlag_migVersion_keys, v_shared_validFlag_migVersion_target);
                __mmask8 bad_slot_mask = summary_mask & (~validFlag_migVersion_match_mask);
                // 打印v_keys的值
                // if (key == 6789187714496537665 || key == 2789205980574075690) {
                //     uint64_t v_keys1_array[8];
                //     uint64_t v_keys2_array[8];
                //     _mm512_storeu_si512((__m512i*)v_keys1_array, keys);
                //     _mm512_storeu_si512((__m512i*)v_keys2_array, v_validFlag_migVersion_keys);
                //     for (int i = 0; i < 8; i++) {
                //         printf("共享桶v_keys[%d] = 0x%016lx, 0x%016lx, shareSlotIndex: %lu, bad_slot_mask: %d, validFlag_migVersion_match_mask: %d, validFlag_migVersion_match_mask_ctz: %d\n",
                //                i, v_keys1_array[i], v_keys2_array[i], shareSlotIndex, bad_slot_mask, validFlag_migVersion_match_mask, __builtin_ctz(validFlag_migVersion_match_mask));
                //     }
                // }
                if (validFlag_migVersion_match_mask) {
                    __m512i v_subKey_keys = _mm512_and_si512(keys, v_subKey_mask);
                    __mmask8 subKey_match_mask = _mm512_cmpeq_epi64_mask(v_subKey_keys, v_subKey_target);
                    // printf("subKey_match_mask: %d, subKey_match_mask_ctz: %d\n", subKey_match_mask, __builtin_ctz(subKey_match_mask));
                    if (subKey_match_mask) {
                        int x = __builtin_ctz(subKey_match_mask);
                        int y = 31 - __builtin_clz(validFlag_migVersion_match_mask);
                        if (x <= y) {
                            _isFindFlag = true;
                            shareSlotIndex += __builtin_ctz(subKey_match_mask);
                            slot = &sharedBucket->counters[shareSlotIndex];
                            //     printf("avx1共享桶插槽 %lu 相同键, subKey: 0x%08llx, slot->subKeyFields.subKey: 0x%08llx, value: %lu, validFlag: %lu, migVersion: %d, sharedBucketVersion: %d, localDepth: %lu, segmentIndex: %lu\n",
                            //            shareSlotIndex, subKey, slot->subKeyFields.subKey, slot->valueFields.value, slot->subKeyFields.validFlag, slot->subKeyFields.migVersion, sharedBucketVersion, localDepth, segmentIndex);
                            break;
                        } else {
                            _isValidSharedSlot = true;
                            shareSlotIndex += __builtin_ctz(bad_slot_mask);
                            slot = &sharedBucket->counters[shareSlotIndex];
                            //     printf("avx2共享桶插槽 %lu 相同键, subKey: 0x%08llx, slot->subKeyFields.subKey: 0x%08llx, value: %lu, validFlag: %lu, migVersion: %d, sharedBucketVersion: %d, localDepth: %lu, segmentIndex: %lu\n",
                            //            shareSlotIndex, subKey, slot->subKeyFields.subKey, slot->valueFields.value, slot->subKeyFields.validFlag, slot->subKeyFields.migVersion, sharedBucketVersion, localDepth, segmentIndex);
                            break;
                        }
                    }
                    if (bad_slot_mask) {
                        _isValidSharedSlot = true;
                        shareSlotIndex += __builtin_ctz(bad_slot_mask);
                        slot = &sharedBucket->counters[shareSlotIndex];
                        //     printf("avx1共享桶插槽 %lu 新键, j: %d, subKey: 0x%08llx, slot->subKeyFields.subKey: 0x%08llx, value: %lu, validFlag: %lu, migVersion: %d, sharedBucketVersion: %d, localDepth: %lu, segmentIndex: %lu\n",
                        //            shareSlotIndex, j, subKey, slot->subKeyFields.subKey, slot->valueFields.value, slot->subKeyFields.validFlag, slot->subKeyFields.migVersion, sharedBucketVersion, localDepth, segmentIndex);
                        break;
                    }
                } else {
                    _isValidSharedSlot = true;
                    slot = &sharedBucket->counters[shareSlotIndex];
                    //     printf("avx2共享桶插槽 %lu 新键, j: %d, subKey: 0x%08llx, slot->subKeyFields.subKey: 0x%08llx, value: %lu, validFlag: %lu, migVersion: %d, sharedBucketVersion: %d, localDepth: %lu, segmentIndex: %lu\n",
                    //            shareSlotIndex, j, subKey, slot->subKeyFields.subKey, slot->valueFields.value, slot->subKeyFields.validFlag, slot->subKeyFields.migVersion, sharedBucketVersion, localDepth, segmentIndex);
                    break;
                }
            }

            // auto start_time = std::chrono::high_resolution_clock::now();

            // 每次循环处理 4 个 Slot（4 * 16字节 = 64字节，刚好填满一个 ZMM 寄存器）
            // const __m512i shuffle_subkeys = _mm512_setr_epi64(0, 2, 4, 6, 0, 0, 0, 0);
            // for (shareSlotIndex = 0; shareSlotIndex < ROERT_SLOTS_PER_BUCKET; shareSlotIndex += 4) {
            //     // 1. 【连续加载】直接读取 64 字节（4个subKey + 4个value），彻底替换掉高延迟的 Gather 指令
            //     __m512i raw_data = _mm512_loadu_si512((const __m512i*)&sharedBucket->counters[shareSlotIndex]);
            //     // 2. 【寄存器内洗牌】提取出 4 个 subKey
            //     __m512i keys = _mm512_permutexvar_epi64(shuffle_subkeys, raw_data);
            //     // 3. 【位运算与并行比较：版本/有效位】
            //     __m512i v_validFlag_migVersion_keys = _mm512_and_si512(keys, v_validFlag_migVersion_mask);
            //     __mmask8 validFlag_migVersion_match_mask = _mm512_cmpeq_epi64_mask(v_validFlag_migVersion_keys, v_shared_validFlag_migVersion_target) & 0x0F;
            //     __mmask8 bad_slot_mask = (~validFlag_migVersion_match_mask) & 0x0F;
            //     if (validFlag_migVersion_match_mask) {
            //         // 4. 【位运算与并行比较：subKey】
            //         __m512i v_subKey_keys = _mm512_and_si512(keys, v_subKey_mask);
            //         __mmask8 subKey_match_mask = _mm512_cmpeq_epi64_mask(v_subKey_keys, v_subKey_target) & 0x0F;
            //         if (subKey_match_mask) {
            //             int x = __builtin_ctz(subKey_match_mask);                    // 第一个 subKey 匹配的位置
            //             int y = 31 - __builtin_clz(validFlag_migVersion_match_mask); // 最后一个版本匹配的位置
            //             if (x <= y) {
            //                 // 找到了完全匹配的项（subKey 和版本都对）
            //                 _isFindFlag = true;
            //                 shareSlotIndex += x;
            //                 slot = &sharedBucket->counters[shareSlotIndex];
            //                 break;
            //             } else {
            //                 // subKey 匹配但版本不对（或者在坏槽位之后），视为找到了可复用的坏槽位
            //                 _isValidSharedSlot = true;
            //                 shareSlotIndex += __builtin_ctz(bad_slot_mask);
            //                 slot = &sharedBucket->counters[shareSlotIndex];
            //                 break;
            //             }
            //         }
            //         // 没找到 subKey 匹配，但有版本匹配的坏槽位
            //         if (bad_slot_mask) {
            //             _isValidSharedSlot = true;
            //             shareSlotIndex += __builtin_ctz(bad_slot_mask);
            //             slot = &sharedBucket->counters[shareSlotIndex];
            //             break;
            //         }
            //     } else {
            //         _isValidSharedSlot = true;
            //         slot = &sharedBucket->counters[shareSlotIndex];
            //         break;
            //     }
            // }

            // 共享桶没有找到，遍历主桶查找可用 slot
            if (!_isFindFlag) {
                // printf("共享桶没有找到，遍历主桶查找可用 slot\n");
                // 计算主桶索引：使用子键的高位索引主桶
                const uint64_t mainBucketIndex = ROERT_GET_MAIN_BUCKET_INDEX(subKey, localDepth, ROERT_MAIN_BUCKETS_MAX_BITS);
                // 主桶在段中的位置
                const uint64_t mainBucketPosition = calcBucketPosition(sharedBucketOffset, mainBucketIndex);

                printf("subKey: 0x%08llx, globalDepth: %lu, localDepth: %lu, segmentIndex: %lu, sharedBucketIndex: %lu, sharedBucketPosition: %lu, mainBucketIndex: %lu, mainBucketPosition: %lu\n",
                       subKey, globalDepth, localDepth, segmentIndex, sharedBucketIndex, sharedBucketPosition, mainBucketIndex, mainBucketPosition);

                // 获取主桶指针
                ROERTBucket* mainBucket = &segment->buckets[mainBucketPosition];

                // 主桶未满，遍历主桶查找可用 slot
                // for (mainSlotIndex = 0; mainSlotIndex < ROERT_SLOTS_PER_BUCKET; ++mainSlotIndex) {
                //     // 获取主桶 slot 指针
                //     slot = &mainBucket->counters[mainSlotIndex];
                //     // printf("主桶插槽 %lu, subKey: 0x%08llx, slot->subKeyFields.subKey: 0x%08llx, value: %lu, validFlag: %lu, migVersion: %d, localDepth: %lu, segmentIndex: %lu\n",
                //     //        mainSlotIndex, subKey, slot->subKeyFields.subKey, slot->valueFields.value, slot->subKeyFields.validFlag, slot->subKeyFields.migVersion, localDepth, segmentIndex);
                //     // if (slot->subKeyFields.validFlag == 0 || slot->subKeyFields.migVersion != localDepth) {
                //     //     _isValidMainSlot = true;
                //     //     break;
                //     // } else if (slot->subKeyFields.subKey == subKey) {
                //     //     _isFindFlag = true;
                //     //     break; // 子键匹配，跳出循环
                //     // }
                //     if (slot->subKeyFields.validFlag == 1 && slot->subKeyFields.migVersion == localDepth) {
                //         if (slot->subKeyFields.subKey == subKey) {
                //             _isFindFlag = true;
                //             break; // 子键匹配，跳出循环
                //         }
                //     } else {
                //         _isValidMainSlot = true;
                //         break;
                //     }
                // }

                const __m512i v_main_validFlag_migVersion_target = _mm512_set1_epi64((1ULL << 41) | ((uint64_t)localDepth << 32));
                for (mainSlotIndex = 0; mainSlotIndex < ROERT_SLOTS_PER_BUCKET; mainSlotIndex += 8) {
                    __m512i keys = _mm512_mask_i64gather_epi64(
                      _mm512_setzero_si512(),                      // 初始值
                      summary_mask,                                // 有效掩码
                      v_offsets,                                   // 偏移量向量
                      (void*)&mainBucket->counters[mainSlotIndex], // 基地址
                      1                                            // 比例因子
                    );
                    __m512i v_validFlag_migVersion_keys = _mm512_and_si512(keys, v_validFlag_migVersion_mask);
                    __mmask8 validFlag_migVersion_match_mask = _mm512_cmpeq_epi64_mask(v_validFlag_migVersion_keys, v_main_validFlag_migVersion_target);
                    __mmask8 bad_slot_mask = summary_mask & (~validFlag_migVersion_match_mask);
                    // 打印v_keys的值
                    // if (key == 6789187714496537665 || key == 2789205980574075690) {
                    //     uint64_t v_keys1_array[8];
                    //     uint64_t v_keys2_array[8];
                    //     _mm512_storeu_si512((__m512i*)v_keys1_array, keys);
                    //     _mm512_storeu_si512((__m512i*)v_keys2_array, v_validFlag_migVersion_keys);
                    //     for (int i = 0; i < 8; i++) {
                    //         printf("共享桶v_keys[%d] = 0x%016lx, 0x%016lx, shareSlotIndex: %lu, bad_slot_mask: %d, validFlag_migVersion_match_mask: %d, validFlag_migVersion_match_mask_ctz: %d\n",
                    //                i, v_keys1_array[i], v_keys2_array[i], shareSlotIndex, bad_slot_mask, validFlag_migVersion_match_mask, __builtin_ctz(validFlag_migVersion_match_mask));
                    //     }
                    // }
                    if (validFlag_migVersion_match_mask) {
                        __m512i v_subKey_keys = _mm512_and_si512(keys, v_subKey_mask);
                        __mmask8 subKey_match_mask = _mm512_cmpeq_epi64_mask(v_subKey_keys, v_subKey_target);
                        // printf("subKey_match_mask: %d, subKey_match_mask_ctz: %d\n", subKey_match_mask, __builtin_ctz(subKey_match_mask));
                        if (subKey_match_mask) {
                            int x = __builtin_ctz(subKey_match_mask);
                            int y = 31 - __builtin_clz(validFlag_migVersion_match_mask);
                            if (x <= y) {
                                _isFindFlag = true;
                                mainSlotIndex += __builtin_ctz(subKey_match_mask);
                                slot = &mainBucket->counters[mainSlotIndex];
                                //     printf("avx1主桶插槽 %lu 相同键, subKey: 0x%08llx, slot->subKeyFields.subKey: 0x%08llx, value: %lu, validFlag: %lu, migVersion: %d, mainBucketVersion: %d, localDepth: %lu, segmentIndex: %lu\n",
                                //            mainSlotIndex, subKey, slot->subKeyFields.subKey, slot->valueFields.value, slot->subKeyFields.validFlag, slot->subKeyFields.migVersion, mainBucketVersion, localDepth, segmentIndex);
                                break;
                            } else {
                                _isValidMainSlot = true;
                                mainSlotIndex += __builtin_ctz(bad_slot_mask);
                                slot = &mainBucket->counters[mainSlotIndex];
                                //     printf("avx2主桶插槽 %lu 相同键, subKey: 0x%08llx, slot->subKeyFields.subKey: 0x%08llx, value: %lu, validFlag: %lu, migVersion: %d, mainBucketVersion: %d, localDepth: %lu, segmentIndex: %lu\n",
                                //            mainSlotIndex, subKey, slot->subKeyFields.subKey, slot->valueFields.value, slot->subKeyFields.validFlag, slot->subKeyFields.migVersion, mainBucketVersion, localDepth, segmentIndex);
                                break;
                            }
                        }
                        if (bad_slot_mask) {
                            _isValidMainSlot = true;
                            mainSlotIndex += __builtin_ctz(bad_slot_mask);
                            slot = &mainBucket->counters[mainSlotIndex];
                            //     printf("avx1主桶插槽 %lu 新键, j: %d, subKey: 0x%08llx, slot->subKeyFields.subKey: 0x%08llx, value: %lu, validFlag: %lu, migVersion: %d, mainBucketVersion: %d, localDepth: %lu, segmentIndex: %lu\n",
                            //            mainSlotIndex, j, subKey, slot->subKeyFields.subKey, slot->valueFields.value, slot->subKeyFields.validFlag, slot->subKeyFields.migVersion, mainBucketVersion, localDepth, segmentIndex);
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

                // for (mainSlotIndex = 0; mainSlotIndex < ROERT_SLOTS_PER_BUCKET; mainSlotIndex += 4) {
                //     // 1. 【连续加载】直接读取 64 字节（4个subKey + 4个value），彻底替换掉高延迟的 Gather 指令
                //     __m512i raw_data = _mm512_loadu_si512((const __m512i*)&mainBucket->counters[mainSlotIndex]);
                //     // 2. 【寄存器内洗牌】提取出 4 个 subKey
                //     __m512i keys = _mm512_permutexvar_epi64(shuffle_subkeys, raw_data);
                //     // 3. 【位运算与并行比较：版本/有效位】
                //     __m512i v_validFlag_migVersion_keys = _mm512_and_si512(keys, v_validFlag_migVersion_mask);
                //     __mmask8 validFlag_migVersion_match_mask = _mm512_cmpeq_epi64_mask(v_validFlag_migVersion_keys, v_main_validFlag_migVersion_target) & 0x0F;
                //     __mmask8 bad_slot_mask = (~validFlag_migVersion_match_mask) & 0x0F;
                //     if (validFlag_migVersion_match_mask) {
                //         // 4. 【位运算与并行比较：subKey】
                //         __m512i v_subKey_keys = _mm512_and_si512(keys, v_subKey_mask);
                //         __mmask8 subKey_match_mask = _mm512_cmpeq_epi64_mask(v_subKey_keys, v_subKey_target) & 0x0F;
                //         if (subKey_match_mask) {
                //             int x = __builtin_ctz(subKey_match_mask);                    // 第一个 subKey 匹配的位置
                //             int y = 31 - __builtin_clz(validFlag_migVersion_match_mask); // 最后一个版本匹配的位置
                //             if (x <= y) {
                //                 // 找到了完全匹配的项（subKey 和版本都对）
                //                 _isFindFlag = true;
                //                 mainSlotIndex += x;
                //                 slot = &mainBucket->counters[mainSlotIndex];
                //                 break;
                //             } else {
                //                 // subKey 匹配但版本不对（或者在坏槽位之后），视为找到了可复用的坏槽位
                //                 _isValidMainSlot = true;
                //                 mainSlotIndex += __builtin_ctz(bad_slot_mask);
                //                 slot = &mainBucket->counters[mainSlotIndex];
                //                 break;
                //             }
                //         }
                //         // 没找到 subKey 匹配，但有版本匹配的坏槽位
                //         if (bad_slot_mask) {
                //             _isValidMainSlot = true;
                //             mainSlotIndex += __builtin_ctz(bad_slot_mask);
                //             slot = &mainBucket->counters[mainSlotIndex];
                //             break;
                //         }
                //     } else {
                //         _isValidMainSlot = true;
                //         slot = &mainBucket->counters[mainSlotIndex];
                //         break;
                //     }
                // }

                if (!_isFindFlag) {
                    if (_isValidSharedSlot) {
                        // 优先使用共享桶的可用slot
                        slot = &sharedBucket->counters[shareSlotIndex];
                    } else if (_isValidMainSlot) {
                        // 共享桶不可用时，标记主桶可用
                        _isShareOrMainFlag = 1;
                    } else {
                        // 都不可用时，设置slot为nullptr，表示需要扩展
                        slot = nullptr;
                    }
                }
            }

            // auto end_time = std::chrono::high_resolution_clock::now();
            // per_insert_node_traversal_latency_.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count());

            // 计算当前处理键的位置和剩余键长度，处理一个子键 ROERT_NODE_SUBKEY_MAX_BYTES 的长度
            currentKeyPosition += ROERT_NODE_SUBKEY_MAX_BYTES;

            // 如果到达键的末尾，说明是叶子节点
            if (currentKeyPosition == ROERT_KEY_MAX_BYTES) {
                // 如果 slot 有效位为 0 或者没有找到该子键，说明是新键值对，直接插入
                if (!_isFindFlag || slot == nullptr) {
                    // printf("情况2.1.1: 到达键的末尾，新键值对，直接插入\n");

                    currentNode->put(
                      subKey, value, segmentNode, segment, slot,
                      segmentIndex, sharedBucketOffset, fingerprint, _isShareOrMainFlag, nodeFlag = 0, &beforeAddress);

                    return OperationResults::Success;
                } else {
                    // 否则，说明是已有键值对，更新值
                    slot->valueFields.value = value;

                    clflush((char*)&slot->valueFields, sizeof(slot->valueFields));

                    // printf("情况2.1.2: 到达键的末尾，已有键值对，更新值\n");

                    return OperationResults::Success;
                }
            } else { // 还未到达键的末尾，说明是内部节点
                // 如果 slot 有效位为 0，说明是新键值对，直接插入
                if (!_isFindFlag || slot == nullptr) {
                    ROERTKeyValue* kv = _kv;

                    if (kv == nullptr) {
                        // 创建新的键值对并持久化
                        kv = NewROERTKeyValue(key, value);
                        clflush((char*)kv, sizeof(ROERTKeyValue));
                    }

                    // printf("情况2.2.1: 还未到达键的末尾，新键值对，直接插入\n");

                    // 插入新键值对到当前节点
                    currentNode->put(
                      subKey, (uint64_t)kv, segmentNode, segment, slot,
                      segmentIndex, sharedBucketOffset, fingerprint, _isShareOrMainFlag, nodeFlag = 0, &beforeAddress);

                    return OperationResults::Success;
                } else {
                    // 如果 slot 指向的下一个节点，递归处理
                    if (slot->subKeyFields.nodeFlag == 1) {
                        // 将当前节点的指针指向下一个节点
                        currentNode = (ROERTNode*)slot->valueFields.value;

                        // 更新前驱地址
                        beforeAddress = (uint64_t)slot;

                        // printf("情况2.2.2: 还未到达键的末尾，指向下一个节点，继续递归\n");

                    } else { // 否则，说明是键值对指针
                        // 提取 slot 指向的键值对指针，并更新键值对
                        ROERTKeyValue* kv = (ROERTKeyValue*)slot->valueFields.value;
                        // 如果键相同，更新键值对
                        if (kv->key == key) {
                            // 节点更新
                            // 更新键值对的值
                            kv->value = value;
                            // 持久化值的部分
                            clflush((char*)&kv->value, sizeof(kv->value));

                            // printf("情况2.2.3: 还未到达键的末尾，已有键值对，更新值\n");

                            return OperationResults::Success;
                        } else { // 如果键不同，需要创建新的节点
                            // 创建新的节点
                            ROERTNode* newNode = NewROERTNode();

                            // 计算两个键从 currentKeyPosition 开始的前缀匹配长度

                            // 初始化新节点的头部
                            newNode->directoryNode.header.initialize(
                              &currentNode->directoryNode.header, 0, currentKeyPosition / ROERT_NODE_SUBKEY_MAX_BYTES);
                            //   &currentNode->directoryNode.header, 0, currentKeyPosition >> 2);

                            // printf("情况2.2.4: 还未到达键的末尾，键不同，创建新节点\n");

                            // TUDO: 待优化

                            // 插入已存在的键值对到新节点
                            insert(kv->key, kv->value, newNode, currentKeyPosition, kv);

                            // 插入新的键值对到新节点
                            insert(key, value, newNode, currentKeyPosition, _kv);

                            // 持久化新节点
                            clflush((char*)newNode, sizeof(ROERTNode));

                            // 更新 slot 指向新节点
                            slot->subKeyFields.nodeFlag = 1;             // 标记为节点指针
                            slot->valueFields.value = (uint64_t)newNode; // 更新值为新节点的地址

                            clflush((char*)slot, sizeof(ROERTSlotKeyValue)); // 持久化 slot

                            return OperationResults::Success;
                        }
                    }
                }
            }
        }

        // 情况3: 前缀不匹配 - 需要节点路径压缩的分裂
        else if (currentNode->directoryNode.header.prefixLength != 0
                 && prefixMatchLength < currentNode->directoryNode.header.prefixLength) {
            // printf("情况3: 前缀不匹配，需要节点路径压缩的分裂\n");

            // 步骤1: 创建新的节点
            ROERTNode* newNode = NewROERTNode();

            // 步骤2: 初始化新节点的头部信息
            newNode->directoryNode.header.initialize(
              &currentNode->directoryNode.header, prefixMatchLength, currentNode->directoryNode.header.nodeHeaderDepth);

            // 步骤3: 获取全局段指针
            if (currentNode->directoryNode.directory.globalSegmentPtr != 0) {
                ROERTKeyValue* newGlobalSegment = reinterpret_cast<ROERTKeyValue*>(newNode->directoryNode.directory.globalSegmentPtr);
                ROERTKeyValue* globalSegment = reinterpret_cast<ROERTKeyValue*>(currentNode->directoryNode.directory.globalSegmentPtr);
                // 复制当前节点的全局段键值对到新节点（从匹配位置开始）
                for (int j = currentNode->directoryNode.header.prefixLength - prefixMatchLength; j <= currentNode->directoryNode.header.prefixLength; j++) {
                    newGlobalSegment[j - currentNode->directoryNode.header.prefixLength + prefixMatchLength] = globalSegment[j];
                }
            }

            // 步骤4: 计算子键：从当前键位置提取 ROERT_NODE_SUBKEY_LENGTH 位的子键
            uint64_t subKey = ROERT_GET_SUBKEY(key, (currentKeyPosition + prefixMatchLength) * ROERT_SIZE_OF_CHAR, ROERT_NODE_SPAN);
            // uint64_t subKey = ROERT_GET_SUBKEY(key, ((currentKeyPosition + prefixMatchLength) << 3), ROERT_NODE_SPAN);

            // 步骤5: 创建新的键值对并持久化
            ROERTKeyValue* kv = NewROERTKeyValue(key, value);
            clflush((char*)kv, sizeof(ROERTKeyValue));

            // 步骤6: 将新键值对插入新节点 (可以直接插入，待优化)
            newNode->putSegNode(subKey, (uint64_t)kv, fingerprint, &beforeAddress, nodeFlag = 0);

            // 步骤7: 将当前节点插入新节点
            newNode->putSegNode(ROERT_GET_PREFIX_ARRAY_BITS(currentNode->directoryNode.header.prefixArray,
                                                            prefixMatchLength * ROERT_NODE_SUBKEY_MAX_BYTES,
                                                            ROERT_NODE_SUBKEY_MAX_BYTES),
                                (uint64_t)currentNode, fingerprint, &beforeAddress, nodeFlag = 1);

            // printf("新键值对: 0x%08llx, 当前节点: 0x%08llx\n",
            //        subKey, ROERT_GET_PREFIX_ARRAY_BITS(currentNode->directoryNode.header.prefixArray, prefixMatchLength * ROERT_NODE_SUBKEY_MAX_BYTES, ROERT_NODE_SUBKEY_MAX_BYTES));

            // 步骤8: 更新当前节点的头部信息, 更新当前节点的前缀长度和节点头深度
            currentNode->directoryNode.header.prefixLength -=
              //   (prefixMatchLength + newNode->directoryNode.header.nodeHeaderDepth);
              (prefixMatchLength + newNode->directoryNode.header.nodeHeaderDepth + 1);
            currentNode->directoryNode.header.nodeHeaderDepth =
              //   newNode->directoryNode.header.prefixLength + newNode->directoryNode.header.nodeHeaderDepth;
              newNode->directoryNode.header.prefixLength + newNode->directoryNode.header.nodeHeaderDepth + 1;

            // 更新当前节点的前缀数组
            for (size_t i = 0; i <= prefixMatchLength; i++) {
                for (size_t j = i * ROERT_NODE_SUBKEY_MAX_BYTES; j < (i + 1) * ROERT_NODE_SUBKEY_MAX_BYTES; j++) {
                    if (j + ROERT_NODE_SUBKEY_MAX_BYTES < ROERT_NODE_PREFIX_MAX_BYTES) {
                        currentNode->directoryNode.header.prefixArray[j] = currentNode->directoryNode.header.prefixArray[j + ROERT_NODE_SUBKEY_MAX_BYTES];
                    } else {
                        currentNode->directoryNode.header.prefixArray[j] = 0;
                    }
                }
            }

            // 分别打印新节点和当前节点的所有头部信息
            // printf("newNode header: prefixLength = %d, nodeHeaderDepth = %d\n", newNode->directoryNode.header.prefixLength, newNode->directoryNode.header.nodeHeaderDepth);
            // for (int i = 0; i < 6; i++) {
            //     printf("prefixArray[%d] = %02x\n", i, newNode->directoryNode.header.prefixArray[i]);
            // }
            // printf("currentNode header: prefixLength = %d, nodeHeaderDepth = %d\n", currentNode->directoryNode.header.prefixLength, currentNode->directoryNode.header.nodeHeaderDepth);
            // for (int i = 0; i < 6; i++) {
            //     printf("prefixArray[%d] = %02x\n", i, currentNode->directoryNode.header.prefixArray[i]);
            // }

            // 步骤9: 持久化当前节点头部信息
            // 持久化修改后的新节点
            clflush((char*)newNode, sizeof(ROERTNode));

            clflush((char*)&(currentNode->directoryNode.header), sizeof(ROERTHeader));

            // 更新前驱节点的指针指向新节点
            if (newNode->directoryNode.header.nodeHeaderDepth != 0) {
                ((ROERTSlotKeyValue*)beforeAddress)->valueFields.value = (uint64_t)newNode;
                clflush((char*)(ROERTSlotKeyValue*)beforeAddress, sizeof(ROERTSlotKeyValue));
                // printf("前驱地址: %lu, 新节点地址: %lu, slot->subKeyFields.subKey: 0x%08llx, value: %lu\n",
                //        beforeAddress, newNode, ((ROERTSlotKeyValue*)beforeAddress)->subKeyFields.subKey, ((ROERTSlotKeyValue*)beforeAddress)->valueFields.value);
            } else {
                *(ROERTNode**)beforeAddress = newNode;
                clflush((char*)beforeAddress, sizeof(ROERTNode*));
                // printf("前驱地址: %lu, 新节点地址: %lu\n", beforeAddress, newNode);
            }

            return OperationResults::Success;
        }
    }

    return OperationResults::Failed;
}

uint64_t ROERT::search(uint64_t key, ROERTNode* node, int keyLength) {
    // 初始化当前节点为传入节点或根节点
    ROERTNode* currentNode = node;

    // 如果未指定节点，使用根节点
    if (ROERT_UNLIKELY(node == nullptr)) {
        currentNode = root;
    }

    if (ROERT_UNLIKELY(currentNode == nullptr)) {
        return (uint64_t)OperationResults::NotFound;
    }

    uint32_t currentKeyPosition = keyLength; // 当前处理的键位置（字节偏移）

    uint32_t arrayMatchLength; // 记录数组匹配长度
    ROERTSlotKeyValue* slot = nullptr;

    // printf("=======================================\n");
    // printf("search key: 0x%lx, (十进制)%llu, root: %lu\n", key, key, root);
    // printf("=======================================\n");

    int prefixMatchLength;
    while (currentKeyPosition < ROERT_KEY_MAX_BYTES) {
        // 情况1：前缀长度不为0时，当前节点需要进行前缀匹配
        if (ROERT_UNLIKELY(currentNode->directoryNode.header.prefixLength > 0)) {
            // printf("kkk\n");
            // 步骤1: 计算匹配的前缀
            prefixMatchLength = currentNode->directoryNode.header.computePrefix(key, currentKeyPosition * ROERT_SIZE_OF_CHAR, &arrayMatchLength);

            // 情况1.1: 前缀匹配成功，到达键的末尾 - 直接读取当前 globalSegment
            if (arrayMatchLength != 0 && currentKeyPosition + arrayMatchLength <= currentNode->directoryNode.header.prefixLength * ROERT_NODE_SUBKEY_MAX_BYTES) {
                // 获取全局段指针
                ROERTKeyValue* globalSegment = reinterpret_cast<ROERTKeyValue*>(currentNode->directoryNode.directory.globalSegmentPtr);

                // 直接读取全局段
                return currentNode->getDirNode(globalSegment, key);
            }
            // 情况1.2：前缀长度为与匹配长度相等 - 直接读取当前节点或者其子节点
            else if (currentNode->directoryNode.header.prefixLength == prefixMatchLength) {
                // printf("kkkasdasd\n");
                // 计算当前处理键的长度和剩余长度
                currentKeyPosition += currentNode->directoryNode.header.prefixLength * ROERT_NODE_SUBKEY_MAX_BYTES;
                // remainingKeyLength -= currentNode->directoryNode.header.prefixLength * ROERT_NODE_SUBKEY_MAX_BYTES;

            }
            // 情况1.3：前缀匹配失败 - 搜索失败
            else {
                return (uint64_t)OperationResults::Failed;
            }
        }

        // 情况2：前缀长度为0或者前缀匹配成功时，直接读取当前节点或者其子节点
        // 计算子键：从当前键位置提取 ROERT_NODE_SUBKEY_LENGTH 位的子键
        // uint64_t subKey = ROERT_GET_SUBKEY(key, currentKeyPosition * ROERT_SIZE_OF_CHAR, ROERT_NODE_SPAN);
        uint64_t subKey = ROERT_GET_SUBKEY(key, (currentKeyPosition << 3), ROERT_NODE_SPAN);

        // auto start_time = std::chrono::high_resolution_clock::now();
        // 根据子键从当前节点中读取子键的值
        slot = currentNode->getSegNode(subKey);
        // 结束计时并输出结果
        // auto end_time = std::chrono::high_resolution_clock::now();
        // per_search_per_node_latency_.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count());

        // 计算当前处理键的长度和剩余长度
        currentKeyPosition +=
          currentNode->directoryNode.header.prefixLength == 0 ? ROERT_NODE_SUBKEY_MAX_BYTES : currentNode->directoryNode.header.prefixLength << 2;
        //   currentNode->directoryNode.header.prefixLength == 0 ? ROERT_NODE_SUBKEY_MAX_BYTES : currentNode->directoryNode.header.prefixLength * ROERT_NODE_SUBKEY_MAX_BYTES;

        // printf("prefixLength = %d, currentKeyPosition = %d, remainingKeyLength = %d, subKey = %llx\n",
        //        currentNode->directoryNode.header.prefixLength, currentKeyPosition, remainingKeyLength, subKey);

        // 情况2.1：子键匹配失败 - 搜索失败
        if (slot == nullptr) {
            return (uint64_t)OperationResults::Failed;
        }

        // 情况2.2：找到子键，且存储的是值
        if (slot->subKeyFields.nodeFlag == 0) {
            // 情况2.2.1：找到子键，且到达键的末尾，说明是叶子节点，直接返回值
            if (currentKeyPosition == ROERT_KEY_MAX_BYTES) {
                // printf("找到子键，且到达键的末尾，说明是叶子节点，直接返回值\n");
                return slot->valueFields.value;
            }
            // 情况2.2.2：找到子键，且未到达键的末尾，说明是内部节点，通过指纹判断该子键是否真实存在
            else {
                // 检查指纹是否匹配
                // printf("key = %lu\n", (reinterpret_cast<ROERTKeyValue*>(slot->valueFields.value))->key);
                // if (slot->subKeyFields.fingerprint == currentNode->fingerprint(key)) {
                if (key == (reinterpret_cast<ROERTKeyValue*>(slot->valueFields.value))->key) {
                    // printf("找到子键，且未到达键的末尾，说明是内部节点，指纹匹配成功，将值转换为指向实际键值的指针，再提取值\n");

                    // 指纹匹配成功，将值转换为指向实际键值的指针，再提取值
                    return (reinterpret_cast<ROERTKeyValue*>(slot->valueFields.value))->value;
                } else {
                    // printf("找到子键，且未到达键的末尾，说明是内部节点，指纹匹配失败，搜索失败\n");
                    return (uint64_t)OperationResults::Failed;
                }
            }
        }
        // 情况2.3：找到子键，且存储的是指向下一个节点的指针
        else {
            // printf("找到子键，且存储的是指向下一个节点的指针\n");
            // 更新当前节点为子节点
            currentNode = reinterpret_cast<ROERTNode*>(slot->valueFields.value);
        }
    }
}

std::vector<ROERTKeyValue> ROERT::lookupRange(uint64_t leftKey, uint64_t rightKey) {
    // 查找范围 [leftKey, rightKey] 内的所有键值对，不要有重复值
    std::vector<ROERTKeyValue> results;

    // printf("=======================================\n");
    // printf("lookupRange: leftKey = %llu, 0x%016llx, rightKey = %llu, 0x%016llx\n", leftKey, leftKey, rightKey, rightKey);
    // printf("=======================================\n");

    // 从根节点开始递归查找
    // auto start_time = std::chrono::high_resolution_clock::now();
    nodeScan(root, leftKey, rightKey, results, 0, 0);
    // auto end_time = std::chrono::high_resolution_clock::now();
    // printf("lookupRange: %llu, %llu, latency = %llu\n", leftKey, rightKey, std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count());

    return results;
}

void ROERT::nodeScan(ROERTNode* node, uint64_t leftKey, uint64_t rightKey,
                     std::vector<ROERTKeyValue>& results, int currentKeyPosition, uint64_t prefix) {
    // 初始化当前节点为传入节点或根节点
    ROERTNode* currentNode = node;
    // 判断当前节点是否为空，默认从根节点开始
    if (ROERT_UNLIKELY(node == nullptr)) {
        currentNode = root;
    }

    if (ROERT_UNLIKELY(currentNode == nullptr)) {
        return;
    }

    uint32_t arrayMatchLength = 0; // 记录数组匹配长度

    // 初始化左右边界在目录中的位置，UINT64_MAX表示未确定
    uint64_t leftSegmentIndex = UINT64_MAX, rightSegmentIndex = UINT64_MAX;

    // 情况1：如果左键等于右键相当于是点查询单个键
    if (ROERT_UNLIKELY(leftKey == rightKey)) {
        // printf("情况1：如果左键等于右键相当于是点查询单个键\n");
        // 直接查询该键
        uint64_t result = search(leftKey);
        ROERTKeyValue keyValue;

        // 如果查询成功且值有效，将键值对添加到结果中
        if (result != (uint64_t)OperationResults::Failed) {
            keyValue.key = leftKey;
            keyValue.value = result;
            results.push_back(keyValue);
        }

        return;
    }

    // 情况2：如果左键大于右键，说明范围为空，直接返回
    if (ROERT_UNLIKELY(leftKey > rightKey)) {
        // printf("情况2：如果左键大于右键，说明范围为空，直接返回\n");
        return;
    }

    // 情况3：正常范围查询
    // 检查当前节点是否需要进行前缀判断
    if (ROERT_UNLIKELY(currentNode->directoryNode.header.prefixLength > 0)) {
        uint64_t subKey;

        // 循环检查左边界的前缀，如果大于前缀数组，则返回失败
        for (size_t i = 0; i < currentNode->directoryNode.header.prefixLength * ROERT_NODE_SUBKEY_MAX_BYTES; i++) {
            // 提取子键：从左键中提取当前位置的子键
            subKey = ROERT_GET_SUBKEY(leftKey, (currentKeyPosition + i) * ROERT_SIZE_OF_CHAR, ROERT_SIZE_OF_CHAR);

            // 如果子键与前缀数组匹配，继续检查下一个子键
            if (subKey == currentNode->directoryNode.header.prefixArray[i]) {
                continue;
            }

            // 如果子键大于前缀数组，说明查询范围的左边界超出当前节点的范围，直接返回
            else if (subKey > currentNode->directoryNode.header.prefixArray[i]) {
                return;
            }

            // 如果子键小于前缀数组，说明查询范围的左边界属于当前节点的范围，左边界为第一个段索引
            else {
                leftSegmentIndex = 0;
                break;
            }
        }

        // 循环检查右边界的前缀，如果大于前缀数组，则返回失败
        for (size_t i = 0; i < currentNode->directoryNode.header.prefixLength * ROERT_NODE_SUBKEY_MAX_BYTES; i++) {
            // 提取子键：从左键中提取当前位置的子键
            subKey = ROERT_GET_SUBKEY(rightKey, (currentKeyPosition + i) * ROERT_SIZE_OF_CHAR, ROERT_SIZE_OF_CHAR);

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
        prefix = (prefix << currentNode->directoryNode.header.prefixLength * ROERT_NODE_SUBKEY_MAX_BYTES * ROERT_SIZE_OF_CHAR);

        // 将节点前缀数组的字节按大端序添加到前缀值中
        for (size_t i = 0; i < currentNode->directoryNode.header.prefixLength * ROERT_NODE_SUBKEY_MAX_BYTES; ++i) {
            prefix += currentNode->directoryNode.header.prefixArray[i] << (currentNode->directoryNode.header.prefixLength * ROERT_NODE_SUBKEY_MAX_BYTES - i);
        }

        // 更新当前位置：跳过已处理的前缀长度（字节）
        currentKeyPosition += currentNode->directoryNode.header.prefixLength * ROERT_NODE_SUBKEY_MAX_BYTES;
    }

    const uint8_t globalDepth = currentNode->directoryNode.directory.globalDepth;

    uint64_t leftSubKey = UINT64_MAX, rightSubKey = UINT64_MAX;
    // 如果左边界位置未确定，计算左子键和段索引
    if (ROERT_LIKELY(leftSegmentIndex == UINT64_MAX)) {
        // 从当前键位置提取 ROERT_NODE_SUBKEY_LENGTH 位的子键
        // leftSubKey = ROERT_GET_SUBKEY(leftKey, currentKeyPosition * ROERT_SIZE_OF_CHAR, ROERT_NODE_SPAN);
        leftSubKey = ROERT_GET_SUBKEY(leftKey, (currentKeyPosition << 3), ROERT_NODE_SPAN);
        // 使用子键的 MSB 作为段索引
        leftSegmentIndex = ROERT_GET_SEGMENT_NUMBER(leftSubKey, ROERT_NODE_SPAN, globalDepth);
    }

    // 如果右边界位置未确定，计算右子键和段索引
    if (ROERT_LIKELY(rightSegmentIndex == UINT64_MAX)) {
        // 从当前键位置提取 ROERT_NODE_SUBKEY_LENGTH 位的子键
        // rightSubKey = ROERT_GET_SUBKEY(rightKey, currentKeyPosition * ROERT_SIZE_OF_CHAR, ROERT_NODE_SPAN);
        rightSubKey = ROERT_GET_SUBKEY(rightKey, (currentKeyPosition << 3), ROERT_NODE_SPAN);
        // 使用子键的 MSB 作为段索引
        rightSegmentIndex = ROERT_GET_SEGMENT_NUMBER(rightSubKey, ROERT_NODE_SPAN, globalDepth);
    }

    // 更新前缀：为子键腾出空间
    prefix = (prefix << ROERT_NODE_SPAN);

    // 更新当前位置：跳过已处理的子键长度（字节）
    currentKeyPosition += ROERT_NODE_SUBKEY_MAX_BYTES;

    // 情况3.1：如果左右段索引一致，则说明查询范围在一个段内
    if (leftSegmentIndex == rightSegmentIndex) {
        // printf("情况3.1：如果左右段索引一致，则说明查询范围在一个段内. 段索引 = %llu, 左边界子键 = 0x%08llx, 右边界子键 = 0x%08llx\n",
        //        leftSegmentIndex, leftSubKey, rightSubKey);

        // 获取段节点指针
        ROERTSegmentNode* segmentNode = reinterpret_cast<ROERTSegmentNode*>(
          ROERT_GET_SEGMENT_POSITION(&currentNode->directoryNode, leftSegmentIndex));

        // 获取段指针
        ROERTSegment* segment = reinterpret_cast<ROERTSegment*>(segmentNode->segmentPtr);
        // 左右子键共享桶在段中的位置：从共享桶偏移开始
        const uint64_t sharedBucketOffset = segmentNode->sharedBucketOffset == 0 ? 0 : segmentNode->sharedBucketOffset + 1;
        const uint8_t localDepth = segmentNode->localDepth;

        // 计算左右共享桶索引：根据主桶索引和共享桶偏移计算
        // 获取左右子键的共享桶索引
        const uint64_t leftSharedBucketIndex = ROERT_GET_SHARED_BUCKET_INDEX(leftSubKey, localDepth, ROERT_MAIN_BUCKETS_MAX_BITS);
        const uint64_t rightSharedBucketIndex = ROERT_GET_SHARED_BUCKET_INDEX(rightSubKey, localDepth, ROERT_MAIN_BUCKETS_MAX_BITS);

        const uint64_t leftSharedBucketPosition = sharedBucketOffset + (leftSharedBucketIndex & ROERT_SHARED_BUCKETS_PER_SEGMENT_MASK);
        const uint64_t rightSharedBucketPosition = sharedBucketOffset + (rightSharedBucketIndex & ROERT_SHARED_BUCKETS_PER_SEGMENT_MASK);

        // 分别计算左共享桶的主桶1，和右共享桶的主桶2，作为主桶的遍历区间
        const uint64_t leftMainBucketPosition_1 =
          ((sharedBucketOffset + ROERT_SHARED_BUCKETS_PER_SEGMENT) % ROERT_SEGMENT_SIZE + (leftSharedBucketPosition % ROERT_SHARED_BUCKETS_PER_SEGMENT) * 2) % ROERT_SEGMENT_SIZE;
        const uint64_t rightMainBucketPosition_2 =
          ((sharedBucketOffset + ROERT_SHARED_BUCKETS_PER_SEGMENT) % ROERT_SEGMENT_SIZE + (rightSharedBucketPosition % ROERT_SHARED_BUCKETS_PER_SEGMENT) * 2 + 1) % ROERT_SEGMENT_SIZE;

        // uint64_t temp_total_time = 0;
        // 桶遍历迭代器
        auto bucketRangeIterate = [&](uint64_t start, uint64_t end, int64_t _migVersion) {
            for (uint64_t i = start; i <= end; i++) {
                // 遍历共享桶中的所有 slot
                for (uint64_t j = 0; j < ROERT_SLOTS_PER_BUCKET; j++) {
                    const auto* slot = &segment->buckets[i].counters[j];
                    // printf("情况3.1：遍历桶------  %llu 中的所有 slot %llu:  curSubKey = 0x%08llx, value = %llu, migVersion = %d, _migVersion = %d, validFlag = %llu, globalDepth = %llu, localDepth = %d, leftSegmentIndex = %d\n",
                    //        i, j, slot->subKeyFields.subKey, slot->valueFields.value, slot->subKeyFields.migVersion, _migVersion, slot->subKeyFields.validFlag, globalDepth, localDepth, leftSegmentIndex);
                    // if (slot->subKeyFields.validFlag == 0 || (localDepth != ROERT_INIT_LOCAL_DEPTH && slot->subKeyFields.migVersion != _migVersion)) {
                    if (slot->subKeyFields.validFlag == 0 || slot->subKeyFields.migVersion != _migVersion) {
                        break;
                    }
                    // 获取当前 slot 的子键
                    uint64_t curSubKey = slot->subKeyFields.subKey;
                    // 如果共享桶的 slot 无效，则跳出循环
                    // 使用子键的 MSB 作为段索引
                    if (ROERT_GET_SEGMENT_NUMBER(curSubKey, ROERT_NODE_SPAN, globalDepth) == leftSegmentIndex) {
                        // 获取当前值
                        uint64_t value = slot->valueFields.value;
                        // printf("情况3.1：遍历桶  %llu 中的所有 slot %llu: subKeySegmentIndex = %llu, leftSegmentIndex = %llu, curSubKey = 0x%08llx, value = %llu, curSubKey = 0x%08llx, value = %llu, migVersion = %llu, _migVersion = %llu, globalDepth = %d, localDepth = %d\n",
                        //        i, j, subKeySegmentIndex, leftSegmentIndex, curSubKey, value, slot->subKeyFields.migVersion, _migVersion, globalDepth, segmentNode->localDepth);
                        // 如果当前 slot 存储的子键在查询范围中
                        // if ((leftSubKey == UINT64_MAX || curSubKey > leftSubKey)
                        //     && (rightSubKey == UINT64_MAX || curSubKey < rightSubKey)) {
                        if (curSubKey > leftSubKey && curSubKey < rightSubKey) {
                            if (currentKeyPosition == ROERT_KEY_MAX_BYTES) {     // 如果到键的末尾
                                results.emplace_back(curSubKey + prefix, value); // 记录匹配的键值对
                            } else {
                                if (slot->subKeyFields.nodeFlag == 1) { // 如果 slot 指向的下一个节点，递归处理
                                    // auto temp_start_time = std::chrono::high_resolution_clock::now();

                                    scanAllNodes((ROERTNode*)value, results, currentKeyPosition, curSubKey + prefix); // 如果是节点指针，获取该节点下的所有键值对
                                    // auto temp_end_time = std::chrono::high_resolution_clock::now();
                                    // temp_total_time += std::chrono::duration_cast<std::chrono::nanoseconds>(temp_end_time - temp_start_time).count();

                                } else {                                       // 否则，说明是键值对指针
                                    results.push_back(*(ROERTKeyValue*)value); // 如果是键值对指针，记录匹配的键值对
                                }
                            }
                        } else if (curSubKey == leftSubKey || curSubKey == rightSubKey) { // 如果当前子键等于查询范围的边界子键
                            if (currentKeyPosition == ROERT_KEY_MAX_BYTES) {              // 如果到键的末尾
                                results.emplace_back(curSubKey + prefix, value);          // 记录匹配的键值对
                            } else {
                                if (slot->subKeyFields.nodeFlag == 1) { // 如果 slot 指向的下一个节点，递归处理
                                    // auto temp_start_time = std::chrono::high_resolution_clock::now();

                                    nodeScan((ROERTNode*)value, leftKey, rightKey, results, currentKeyPosition, curSubKey + prefix); // 如果是节点指针，递归扫描子节点
                                    // auto temp_end_time = std::chrono::high_resolution_clock::now();
                                    // temp_total_time += std::chrono::duration_cast<std::chrono::nanoseconds>(temp_end_time - temp_start_time).count();

                                } else {                                       // 否则，说明是键值对指针
                                    results.push_back(*(ROERTKeyValue*)value); // 如果是键值对指针，记录匹配的键值对
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
        if (ROERT_SEGMENT_SIZE / 2 > leftSharedBucketPosition && ROERT_SEGMENT_SIZE / 2 < rightSharedBucketPosition) {
            // printf("主桶区间1: [%llu, %llu], 共享桶区间: [%llu, %llu], 主桶区间2: [%llu, %llu]\n",
            //        0, rightMainBucketPosition_2, leftSharedBucketPosition, rightSharedBucketPosition, leftMainBucketPosition_1, ROERT_SEGMENT_SIZE - 1);
            // 情况2: 三段区间 [startMainBucketPosition, sharedBucketOffset-1], [sharedBucketOffset + sharedBucketIndexStart, sharedBucketOffset + sharedBucketIndexEnd - 1], [sharedBucketOffset + ROERT_SHARED_BUCKETS_PER_SEGMENT, endMainBucketPosition]
            bucketRangeIterate(0, rightMainBucketPosition_2, localDepth);
            bucketRangeIterate(leftSharedBucketPosition, rightSharedBucketPosition, localDepth - 1);
            bucketRangeIterate(leftMainBucketPosition_1, ROERT_SEGMENT_SIZE - 1, localDepth);
        } else {
            // printf("主桶区间: [%llu, %llu], 共享桶区间: [%llu, %llu]\n", leftMainBucketPosition_1, rightMainBucketPosition_2, leftSharedBucketPosition, rightSharedBucketPosition);
            // 情况1: 两段区间 [startMainBucketPosition, endMainBucketPosition], [sharedBucketOffset + sharedBucketIndexStart, sharedBucketOffset + sharedBucketIndexEnd - 1]
            bucketRangeIterate(leftSharedBucketPosition, rightSharedBucketPosition, localDepth - 1);
            bucketRangeIterate(leftMainBucketPosition_1, rightMainBucketPosition_2, localDepth);
        }
        // auto end_time = std::chrono::high_resolution_clock::now();
        // range_segment_scan_latency_.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count() - temp_total_time);

        return;
    }

    // 情况3.2：如果左右段索引不一致，则说明查询范围跨多个段
    else {
        // printf("情况3.2：如果左右段索引不一致，则说明查询范围跨多个段. 段索引 = %llu - %llu, 左边界子键 = 0x%08llx, 右边界子键 = 0x%08llx\n",
        //        leftSegmentIndex, rightSegmentIndex, leftSubKey, rightSubKey);

        ROERTSegment* segment;
        ROERTSegmentNode* segmentNode;

        // 获取段节点指针
        ROERTSegmentNode* leftSegmentNode = reinterpret_cast<ROERTSegmentNode*>(
          ROERT_GET_SEGMENT_POSITION(&currentNode->directoryNode, leftSegmentIndex));

        ROERTSegmentNode* rightSegmentNode = reinterpret_cast<ROERTSegmentNode*>(
          ROERT_GET_SEGMENT_POSITION(&currentNode->directoryNode, rightSegmentIndex));

        // 获取左右子键的共享桶索引
        const uint64_t leftSharedBucketIndex = ROERT_GET_SHARED_BUCKET_INDEX(leftSubKey, leftSegmentNode->localDepth, ROERT_MAIN_BUCKETS_MAX_BITS);
        const uint64_t rightSharedBucketIndex = ROERT_GET_SHARED_BUCKET_INDEX(rightSubKey, rightSegmentNode->localDepth, ROERT_MAIN_BUCKETS_MAX_BITS);

        uint64_t leftSharedBucketPosition, rightSharedBucketPosition;
        uint64_t leftMainBucketPosition_1, rightMainBucketPosition_2;
        uint64_t sharedBucketOffset, segmentNodeNum, segmentNodeIndexStart;

        for (uint64_t _segmentIndex = leftSegmentIndex, sameSegmentIndex = leftSegmentIndex; _segmentIndex <= rightSegmentIndex;) {
            // printf("当前段索引 = %llu\n", _segmentIndex);
            // 获取段节点指针
            segmentNode = reinterpret_cast<ROERTSegmentNode*>(
              ROERT_GET_SEGMENT_POSITION(&currentNode->directoryNode, _segmentIndex));
            sharedBucketOffset = segmentNode->sharedBucketOffset == 0 ? 0 : segmentNode->sharedBucketOffset + 1;
            uint8_t localDepth = segmentNode->localDepth;
            segmentNodeNum = 1ULL << (globalDepth - localDepth);
            segmentNodeIndexStart = _segmentIndex & (~(segmentNodeNum - 1));
            // 如果是左边界
            if (_segmentIndex == leftSegmentIndex) {
                if (segmentNode->segmentPtr == rightSegmentNode->segmentPtr) { // 如果当前段节点与右节点指向同一个段
                    // printf("左边界，当前段节点与右节点指向同一个段\n");
                    leftSharedBucketPosition = sharedBucketOffset + (leftSharedBucketIndex & ROERT_SHARED_BUCKETS_PER_SEGMENT_MASK);
                    rightSharedBucketPosition = sharedBucketOffset + (rightSharedBucketIndex & ROERT_SHARED_BUCKETS_PER_SEGMENT_MASK);
                    sameSegmentIndex = rightSegmentIndex;
                } else { // 如果当前段节点与右节点指向不同的段
                    // printf("左边界，当前段节点与右节点指向不同的段\n");
                    leftSharedBucketPosition = sharedBucketOffset + (leftSharedBucketIndex & ROERT_SHARED_BUCKETS_PER_SEGMENT_MASK);
                    rightSharedBucketPosition = sharedBucketOffset + ROERT_SHARED_BUCKETS_PER_SEGMENT_MASK;
                    sameSegmentIndex = segmentNodeNum + segmentNodeIndexStart - 1;
                }
            }
            // 如果是右边界
            else if (_segmentIndex == rightSegmentIndex) {
                // printf("右边界\n");
                leftSharedBucketPosition = sharedBucketOffset;
                rightSharedBucketPosition = sharedBucketOffset + (rightSharedBucketIndex & ROERT_SHARED_BUCKETS_PER_SEGMENT_MASK);
                sameSegmentIndex = rightSegmentIndex;
            }
            // 中间段，从第一个共享桶开始遍历
            else {
                if (segmentNode->segmentPtr == rightSegmentNode->segmentPtr) { // 如果当前段节点与右节点指向同一个段
                    // printf("中间段，当前段节点与右节点指向同一个段\n");
                    leftSharedBucketPosition = sharedBucketOffset;
                    rightSharedBucketPosition = sharedBucketOffset + (rightSharedBucketIndex & ROERT_SHARED_BUCKETS_PER_SEGMENT_MASK);
                    sameSegmentIndex = rightSegmentIndex;
                } else {
                    // printf("中间段，当前段节点与右节点指向不同的段\n");
                    leftSharedBucketPosition = sharedBucketOffset;
                    rightSharedBucketPosition = sharedBucketOffset + ROERT_SHARED_BUCKETS_PER_SEGMENT_MASK;
                    sameSegmentIndex = segmentNodeNum + segmentNodeIndexStart - 1;
                }
            }
            // printf("相同段索引: %llu\n", sameSegmentIndex);
            // 获取段指针
            segment = reinterpret_cast<ROERTSegment*>(segmentNode->segmentPtr);
            // 分别计算左共享桶的主桶1，和右共享桶的主桶2，作为主桶的遍历区间
            leftMainBucketPosition_1 =
              ((sharedBucketOffset + ROERT_SHARED_BUCKETS_PER_SEGMENT) % ROERT_SEGMENT_SIZE + (leftSharedBucketPosition % ROERT_SHARED_BUCKETS_PER_SEGMENT) * 2) % ROERT_SEGMENT_SIZE;
            rightMainBucketPosition_2 =
              ((sharedBucketOffset + ROERT_SHARED_BUCKETS_PER_SEGMENT) % ROERT_SEGMENT_SIZE + (rightSharedBucketPosition % ROERT_SHARED_BUCKETS_PER_SEGMENT) * 2 + 1) % ROERT_SEGMENT_SIZE;
            // printf("左共享桶位置: %llu, 右共享桶位置: %llu, 开始主桶位置: %llu, 结束主桶位置: %llu\n", leftSharedBucketPosition, rightSharedBucketPosition, leftMainBucketPosition_1, rightMainBucketPosition_2);

            // uint64_t temp_total_time = 0;
            auto bucketRangeIterate = [&](uint64_t start, uint64_t end, int64_t _migVersion) {
                for (uint64_t i = start; i <= end; i++) {
                    // 遍历桶中的所有 slot
                    for (uint64_t j = 0; j < ROERT_SLOTS_PER_BUCKET; j++) {
                        const auto* slot = &segment->buckets[i].counters[j];
                        // if (slot->subKeyFields.validFlag == 0 || (localDepth != ROERT_INIT_LOCAL_DEPTH && slot->subKeyFields.migVersion != _migVersion)) {
                        if (slot->subKeyFields.validFlag == 0 || slot->subKeyFields.migVersion != _migVersion) {
                            break;
                        }
                        // 获取当前 slot 的子键
                        uint64_t curSubKey = slot->subKeyFields.subKey;
                        // 使用子键的 MSB 作为段索引
                        uint64_t subKeySegmentIndex = ROERT_GET_SEGMENT_NUMBER(curSubKey, ROERT_NODE_SPAN, globalDepth);
                        // 如果桶的 slot 无效，则跳出循环
                        if (subKeySegmentIndex >= _segmentIndex && subKeySegmentIndex <= sameSegmentIndex) {
                            // 获取当前值
                            uint64_t value = slot->valueFields.value;
                            // printf("情况3.2：遍历桶中的所有slot: segmentIndex = %llu, curSubKey = 0x%08llx, value = %llu, migVersion = %llu\n", _segmentIndex, curSubKey, value, bucket->counters[j].subKeyFields.migVersion);
                            // 如果当前 slot 存储的子键在查询范围中
                            // if ((leftSubKey == UINT64_MAX || curSubKey > leftSubKey)
                            //     && (rightSubKey == UINT64_MAX || curSubKey < rightSubKey)) {
                            if (curSubKey > leftSubKey && curSubKey < rightSubKey) {
                                if (currentKeyPosition == ROERT_KEY_MAX_BYTES) {     // 如果到键的末尾
                                    results.emplace_back(curSubKey + prefix, value); // 记录匹配的键值对
                                } else {
                                    if (slot->subKeyFields.nodeFlag == 1) { // 如果 slot 指向的下一个节点，递归处理
                                        // auto temp_start_time = std::chrono::high_resolution_clock::now();

                                        scanAllNodes((ROERTNode*)value, results, currentKeyPosition, curSubKey + prefix); // 如果是节点指针，获取该节点下的所有键值对
                                        // auto temp_end_time = std::chrono::high_resolution_clock::now();
                                        // temp_total_time += std::chrono::duration_cast<std::chrono::nanoseconds>(temp_end_time - temp_start_time).count();

                                    } else {                                       // 否则，说明是键值对指针
                                        results.push_back(*(ROERTKeyValue*)value); // 如果是键值对指针，记录匹配的键值对
                                    }
                                }
                            } else if (curSubKey == leftSubKey || curSubKey == rightSubKey) { // 如果当前子键等于查询范围的边界子键
                                if (currentKeyPosition == ROERT_KEY_MAX_BYTES) {              // 如果到键的末尾
                                    results.emplace_back(curSubKey + prefix, value);          // 记录匹配的键值对
                                } else {
                                    if (slot->subKeyFields.nodeFlag == 1) { // 如果 slot 指向的下一个节点，递归处理
                                        // auto temp_start_time = std::chrono::high_resolution_clock::now();

                                        nodeScan((ROERTNode*)value, leftKey, rightKey, results, currentKeyPosition, curSubKey + prefix); // 如果是节点指针，递归扫描子节点
                                        // auto temp_end_time = std::chrono::high_resolution_clock::now();
                                        // temp_total_time += std::chrono::duration_cast<std::chrono::nanoseconds>(temp_end_time - temp_start_time).count();

                                    } else {                                       // 否则，说明是键值对指针
                                        results.push_back(*(ROERTKeyValue*)value); // 如果是键值对指针，记录匹配的键值对
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
            if (ROERT_SEGMENT_SIZE / 2 > leftSharedBucketPosition && ROERT_SEGMENT_SIZE / 2 < rightSharedBucketPosition) {
                // printf("主桶区间1: [%llu, %llu], 共享桶区间: [%llu, %llu], 主桶区间2: [%llu, %llu]\n",
                //        0, rightMainBucketPosition_2, leftSharedBucketPosition, rightSharedBucketPosition, leftMainBucketPosition_1, ROERT_SEGMENT_SIZE - 1);
                // 情况2: 三段区间 [startMainBucketPosition, sharedBucketOffset-1], [sharedBucketOffset + sharedBucketIndexStart, sharedBucketOffset + sharedBucketIndexEnd - 1], [sharedBucketOffset + ROERT_SHARED_BUCKETS_PER_SEGMENT, endMainBucketPosition]
                bucketRangeIterate(0, rightMainBucketPosition_2, localDepth);
                bucketRangeIterate(leftSharedBucketPosition, rightSharedBucketPosition, localDepth - 1);
                bucketRangeIterate(leftMainBucketPosition_1, ROERT_SEGMENT_SIZE - 1, localDepth);
            } else {
                // printf("主桶区间: [%llu, %llu], 共享桶区间: [%llu, %llu]\n", leftMainBucketPosition_1, rightMainBucketPosition_2, leftSharedBucketPosition, rightSharedBucketPosition);
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

void ROERT::scanAllNodes(ROERTNode* node, std::vector<ROERTKeyValue>& results, int currentKeyPosition, uint64_t prefix) {
    // printf("ScanAllNodes called. currentKeyPosition: %d, prefix: %016llx\n", currentKeyPosition, prefix);
    // 初始化当前节点为传入节点或根节点
    ROERTNode* currentNode = node;

    if (ROERT_UNLIKELY(currentNode == nullptr)) {
        return;
    }

    if (ROERT_UNLIKELY(currentNode->directoryNode.header.prefixLength > 0)) {
        // 将当前前缀左移，为新的前缀字节腾出空间
        prefix = (prefix << currentNode->directoryNode.header.prefixLength * ROERT_NODE_SUBKEY_MAX_BYTES * ROERT_SIZE_OF_CHAR);

        // 将节点前缀数组的字节按大端序添加到前缀值中
        for (size_t i = 0; i < currentNode->directoryNode.header.prefixLength * ROERT_NODE_SUBKEY_MAX_BYTES; ++i) {
            prefix += currentNode->directoryNode.header.prefixArray[i] << (currentNode->directoryNode.header.prefixLength * ROERT_NODE_SUBKEY_MAX_BYTES - i);
        }

        // 更新当前位置：跳过已处理的前缀长度（字节）
        currentKeyPosition += currentNode->directoryNode.header.prefixLength * ROERT_NODE_SUBKEY_MAX_BYTES;
    }

    const uint8_t globalDepth = currentNode->directoryNode.directory.globalDepth;

    // 更新前缀：为子键腾出空间
    prefix = (prefix << ROERT_NODE_SPAN);

    // 更新当前位置：跳过已处理的子键长度（字节）
    currentKeyPosition += ROERT_NODE_SUBKEY_MAX_BYTES;

    ROERTSegment* segment;
    ROERTSegmentNode* segmentNode;

    ROERTSegmentNode* lastSegmentNode = reinterpret_cast<ROERTSegmentNode*>(
      ROERT_GET_SEGMENT_POSITION(&currentNode->directoryNode, currentNode->directoryNode.capacity - 1));

    uint64_t sharedBucketOffset, segmentNodeNum, segmentNodeIndexStart;
    uint64_t lastSegmentIndex = currentNode->directoryNode.capacity - 1;
    // 从 0 号段开始遍历所有桶
    for (uint64_t _segmentIndex = 0, sameSegmentIndex = 0; _segmentIndex <= lastSegmentIndex;) {
        // printf("当前段索引 = %llu\n", _segmentIndex);
        // 获取段节点指针
        segmentNode = reinterpret_cast<ROERTSegmentNode*>(
          ROERT_GET_SEGMENT_POSITION(&currentNode->directoryNode, _segmentIndex));
        sharedBucketOffset = segmentNode->sharedBucketOffset == 0 ? 0 : segmentNode->sharedBucketOffset + 1;
        uint8_t localDepth = segmentNode->localDepth;
        segmentNodeNum = 1ULL << (globalDepth - localDepth);
        segmentNodeIndexStart = _segmentIndex & (~(segmentNodeNum - 1));
        // 如果是左边界
        if (_segmentIndex == 0) {
            if (segmentNode->segmentPtr == lastSegmentNode->segmentPtr) { // 如果当前段节点与最后一个段节点指向同一个段
                // printf("左边界，当前段节点与最后一个段节点指向同一个段\n");
                sameSegmentIndex = lastSegmentIndex;
            } else { // 如果当前段节点与最后一个段节点指向不同的段
                // printf("左边界，当前段节点与最后一个段节点指向不同的段\n");
                sameSegmentIndex = segmentNodeNum + segmentNodeIndexStart - 1;
            }
        }
        // 如果是右边界
        else if (_segmentIndex == currentNode->directoryNode.capacity - 1) {
            // printf("右边界\n");
            sameSegmentIndex = lastSegmentIndex;
        }
        // 中间段，从第一个共享桶开始遍历
        else {
            if (segmentNode->segmentPtr == lastSegmentNode->segmentPtr) { // 如果当前段节点与最后一个段节点指向同一个段
                // printf("中间段，当前段节点与最后一个段节点指向同一个段\n");
                sameSegmentIndex = lastSegmentIndex;
            } else {
                // printf("中间段，当前段节点与最后一个段节点指向不同的段\n");
                sameSegmentIndex = segmentNodeNum + segmentNodeIndexStart - 1;
            }
        }
        // printf("相同段索引: %llu\n", sameSegmentIndex);
        // 获取段指针
        segment = reinterpret_cast<ROERTSegment*>(segmentNode->segmentPtr);

        // uint64_t temp_total_time = 0;
        auto bucketRangeIterate = [&](uint64_t start, uint64_t end, int64_t _migVersion) {
            for (uint64_t _bucketIndex = start; _bucketIndex <= end; _bucketIndex++) {
                // 遍历桶中的所有 slot
                for (uint64_t _slotIndex = 0; _slotIndex < ROERT_SLOTS_PER_BUCKET; _slotIndex++) {
                    const auto* slot = &segment->buckets[_bucketIndex].counters[_slotIndex];
                    // if (slot->subKeyFields.validFlag == 0 || (localDepth != ROERT_INIT_LOCAL_DEPTH && slot->subKeyFields.migVersion != _migVersion)) {
                    if (slot->subKeyFields.validFlag == 0 || slot->subKeyFields.migVersion != _migVersion) {
                        break;
                    }
                    // 获取当前 slot 的子键
                    uint64_t curSubKey = slot->subKeyFields.subKey;
                    // 使用子键的 MSB 作为段索引
                    uint64_t subKeySegmentIndex = ROERT_GET_SEGMENT_NUMBER(curSubKey, ROERT_NODE_SPAN, globalDepth);
                    // 如果桶的 slot 无效，则跳出循环
                    if (subKeySegmentIndex >= _segmentIndex && subKeySegmentIndex <= sameSegmentIndex) {
                        // 获取当前值
                        uint64_t value = slot->valueFields.value;
                        // printf("情况3.2：遍历桶中的所有slot: segmentIndex = %llu, curSubKey = 0x%08llx, value = %llu, migVersion = %llu\n", _segmentIndex, curSubKey, value, bucket->counters[j].subKeyFields.migVersion);
                        // 如果当前 slot 存储的子键在查询范围中
                        if (currentKeyPosition == ROERT_KEY_MAX_BYTES) {     // 如果到键的末尾
                            results.emplace_back(curSubKey + prefix, value); // 记录匹配的键值对
                        } else {
                            if (slot->subKeyFields.nodeFlag == 1) { // 如果 slot 指向的下一个节点，递归处理
                                // auto temp_start_time = std::chrono::high_resolution_clock::now();

                                scanAllNodes((ROERTNode*)value, results, currentKeyPosition, curSubKey + prefix); // 如果是节点指针，获取该节点下的所有键值对
                                // auto temp_end_time = std::chrono::high_resolution_clock::now();
                                // temp_total_time += std::chrono::duration_cast<std::chrono::nanoseconds>(temp_end_time - temp_start_time).count();

                            } else {                                       // 否则，说明是键值对指针
                                results.push_back(*(ROERTKeyValue*)value); // 如果是键值对指针，记录匹配的键值对
                            }
                        }
                    } else {
                        break;
                    }
                }
            }
        };

        // auto start_time = std::chrono::high_resolution_clock::now();
        if (sharedBucketOffset == ROERT_SHARED_BUCKETS_PER_SEGMENT) {
            // printf("主桶区间1: [%llu, %llu], 共享桶区间: [%llu, %llu], 主桶区间2: [%llu, %llu]\n",
            //        0, rightMainBucketPosition_2, leftSharedBucketPosition, rightSharedBucketPosition, leftMainBucketPosition_1, ROERT_SEGMENT_SIZE - 1);
            // 情况2: 三段区间 [startMainBucketPosition, sharedBucketOffset-1], [sharedBucketOffset + sharedBucketIndexStart, sharedBucketOffset + sharedBucketIndexEnd - 1], [sharedBucketOffset + ROERT_SHARED_BUCKETS_PER_SEGMENT, endMainBucketPosition]
            bucketRangeIterate(0, sharedBucketOffset - 1, localDepth);
            bucketRangeIterate(sharedBucketOffset, sharedBucketOffset + ROERT_SHARED_BUCKETS_PER_SEGMENT - 1, localDepth - 1);
            bucketRangeIterate(sharedBucketOffset + ROERT_SHARED_BUCKETS_PER_SEGMENT, ROERT_SEGMENT_SIZE - 1, localDepth);
        } else if (sharedBucketOffset == 0) {
            // printf("主桶区间: [%llu, %llu], 共享桶区间: [%llu, %llu]\n", leftMainBucketPosition_1, rightMainBucketPosition_2, leftSharedBucketPosition, rightSharedBucketPosition);
            // 情况1: 两段区间 [startMainBucketPosition, endMainBucketPosition], [sharedBucketOffset + sharedBucketIndexStart, sharedBucketOffset + sharedBucketIndexEnd - 1]
            bucketRangeIterate(0, sharedBucketOffset + ROERT_SHARED_BUCKETS_PER_SEGMENT - 1, localDepth - 1);
            bucketRangeIterate(sharedBucketOffset + ROERT_SHARED_BUCKETS_PER_SEGMENT, ROERT_SEGMENT_SIZE - 1, localDepth);
        } else {
            bucketRangeIterate(0, sharedBucketOffset - 1, localDepth);
            bucketRangeIterate(sharedBucketOffset, ROERT_SEGMENT_SIZE - 1, localDepth - 1);
        }
        // auto end_time = std::chrono::high_resolution_clock::now();
        // range_segment_scan_latency_.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count() - temp_total_time);

        _segmentIndex = segmentNodeIndexStart + segmentNodeNum;
        // printf("更新后段索引: %llu\n", _segmentIndex);
    }
}

uint64_t ROERT::memory_profile(ROERTNode* node, int position) {
    // 如果未指定节点，使用根节点
    if (node == nullptr) {
        node = root;
    }

    // 计算当前节点的内存占用：节点本身大小 + 目录节点大小 + 段节点数组大小
    uint64_t totalMemory = sizeof(ROERTNode) + sizeof(ROERTSegmentNode) * node->directoryNode.capacity;

    // 计算全局段的大小（如果存在）
    if (node->directoryNode.directory.globalSegmentPtr != 0) {
        totalMemory += sizeof(ROERTKeyValue) * ROERT_BUCKET_SIZE;
    }

    position += ROERT_NODE_SUBKEY_LENGTH;
    ROERTSegment* lastSegment = nullptr;

    // printf("sizeof(ROERTNode): %llu, sizeof(ROERTSegmentNode): %llu, 当前节点的内存占用 capacity: %llu,totalMemory: %llu\n",
    //        sizeof(ROERTNode), sizeof(ROERTSegmentNode), node->directoryNode.capacity, totalMemory);

    // 遍历所有段节点
    for (uint64_t i = 0; i < node->directoryNode.capacity; i++) {
        // 获取段节点指针
        ROERTSegmentNode* segmentNode = reinterpret_cast<ROERTSegmentNode*>(
          ROERT_GET_SEGMENT_POSITION(&node->directoryNode, i));
        // 获取段指针
        ROERTSegment* segment = reinterpret_cast<ROERTSegment*>(segmentNode->segmentPtr);
        // 跳过重复的段引用
        if (segment == lastSegment) {
            continue;
        } else {
            lastSegment = segment;
            // 计算段中所有桶的大小
            totalMemory += ROERT_SEGMENT_SIZE * sizeof(ROERTBucket);
            // printf("计算段中所有桶的大小 totalMemory: %llu, bucketSize: %llu\n", totalMemory, ROERT_SEGMENT_SIZE * sizeof(ROERTBucket));
        }
        uint64_t sharedBucketOffset = segmentNode->sharedBucketOffset == 0 ? 0 : segmentNode->sharedBucketOffset + 1;
        if (segment != nullptr) {
            // 遍历段中的所有桶
            for (uint32_t j = 0; j < ROERT_SEGMENT_SIZE; j++) {
                // 遍历桶中的所有slot
                for (uint32_t k = 0; k < ROERT_SLOTS_PER_BUCKET; k++) {
                    ROERTSlotKeyValue* slot = &segment->buckets[j].counters[k];
                    if (j >= sharedBucketOffset && j < sharedBucketOffset + ROERT_SHARED_BUCKETS_PER_SEGMENT) {
                        if (isInvalidSharedSlot(slot, segmentNode->localDepth)) {
                            break;
                        }
                        if (position != ROERT_KEY_LENGTH) {
                            // 如果是节点指针，递归计算子节点的内存占用
                            if (slot->subKeyFields.nodeFlag == 0) {
                                totalMemory += sizeof(ROERTKeyValue);
                            } else {
                                totalMemory += memory_profile(reinterpret_cast<ROERTNode*>(slot->valueFields.value), position);
                            }
                        }
                    } else {
                        if (isInvalidMainSlot(slot, segmentNode->localDepth)) {
                            break;
                        }
                        if (position != ROERT_KEY_LENGTH) {
                            // 如果是节点指针，递归计算子节点的内存占用
                            if (slot->subKeyFields.nodeFlag == 0) {
                                totalMemory += sizeof(ROERTKeyValue);
                            } else {
                                totalMemory += memory_profile(reinterpret_cast<ROERTNode*>(slot->valueFields.value), position);
                            }
                        }
                    }
                }
            }
        }
    }

    return totalMemory;
}

ROERT* ROERT::recoveryTree(int position, int nodeHeaderDepth) {
    // 获取新内存区域的基地址
    char* new_base_ptr = reinterpret_cast<char*>(concurrency_myallocator->nvm[0]);

    // 恢复 ROERT 对象
    ROERT* _roert_ptr = reinterpret_cast<ROERT*>(new_base_ptr);

    // 获取旧内存区域的基地址
    uintptr_t old_base = reinterpret_cast<uintptr_t>(_roert_ptr->_base_ptr);

    // 更新新内存区域的基地址
    _roert_ptr->_base_ptr = new_base_ptr;

    // printf("恢复 ROERT 对象: _roert_ptr: %p, globalDepthTree: %d\n", _roert_ptr, _roert_ptr->globalDepthTree);

    // 计算 offset
    uintptr_t _offset = reinterpret_cast<uintptr_t>(_roert_ptr->root) - old_base;

    // 恢复根节点指针，根据 offset 计算新地址
    _roert_ptr->root = reinterpret_cast<ROERTNode*>(new_base_ptr + _offset);

    // printf("恢复根节点指针: root: %p, root->directoryNode.capacity: %llu, root->directoryNode.header.prefixLength: %d, root->directoryNode.header.nodeHeaderDepth: %d\n",
    //        _roert_ptr->root, _roert_ptr->root->directoryNode.capacity, _roert_ptr->root->directoryNode.header.prefixLength, _roert_ptr->root->directoryNode.header.nodeHeaderDepth);

    if (_roert_ptr->root->directoryNode.header.prefixLength != 0) {
        position += _roert_ptr->root->directoryNode.header.prefixLength * ROERT_NODE_SUBKEY_LENGTH;
    }

    position += ROERT_NODE_SUBKEY_LENGTH;
    ROERTSegment* lastSegment = nullptr;

    const uint8_t globalDepth = _roert_ptr->root->directoryNode.directory.globalDepth;

    // printf("恢复树结构: globalDepth: %d, position: %d\n", globalDepth, position);

    // 找到任意两个叶子节点
    uint64_t leaf[2];
    uint64_t leaf_cnt = 0;

    for (uint64_t i = 0; i < _roert_ptr->root->directoryNode.capacity;) {
        // 获取段节点指针
        ROERTSegmentNode* segmentNode = reinterpret_cast<ROERTSegmentNode*>(
          ROERT_GET_SEGMENT_POSITION(&_roert_ptr->root->directoryNode, i));

        // 计算旧段节点指针的 offset
        uintptr_t segmentNode_offset = reinterpret_cast<uintptr_t>(segmentNode->segmentPtr) - old_base;

        // 更新段节点指针
        segmentNode->segmentPtr = reinterpret_cast<uint64_t>(new_base_ptr + segmentNode_offset);

        // 获取段指针
        ROERTSegment* segment = reinterpret_cast<ROERTSegment*>(segmentNode->segmentPtr);

        // printf("恢复段节点指针: segmentIndex: %llu, segmentNodePtr: %p, localDepth: %d, sharedBucketOffset: %d\n",
        //        i, segmentNode->segmentPtr, segmentNode->localDepth, segmentNode->sharedBucketOffset);

        // 恢复目录节点
        uint64_t stride = 1ULL << (globalDepth - segmentNode->localDepth);
        uint64_t buddyIndexStart = i & (~(stride - 1));
        uint64_t buddyIndex = buddyIndexStart + stride;

        // printf("i: %llu, buddyIndex: %llu, stride: %llu, capacity: %llu, globalDepth: %d, segmentNodelocalDepth: %d, segmentNode_offset: %llu, segmentNode->segmentPtr: %llu\n",
        //        i, buddyIndex, stride, _roert_ptr->root->directoryNode.capacity, globalDepth, segmentNode->localDepth, segmentNode_offset, segmentNode->segmentPtr);

        // 伙伴深度更大，说明当前项是分裂后的左半部分，需要将中间不一致的部分修正
        for (uint64_t _buddy = buddyIndex - 1; _buddy > i; _buddy--) {
            // 获取伙伴目录项 (Buddy)
            ROERTSegmentNode* segmentNodeBuddy = reinterpret_cast<ROERTSegmentNode*>(
              ROERT_GET_SEGMENT_POSITION(&_roert_ptr->root->directoryNode, _buddy));
            // 计算旧段节点指针的 offset
            uintptr_t segmentNodeBuddy_offset = reinterpret_cast<uintptr_t>(segmentNodeBuddy->segmentPtr) - old_base;
            // 更新伙伴段节点指针
            segmentNodeBuddy->segmentPtr = reinterpret_cast<uint64_t>(new_base_ptr + segmentNodeBuddy_offset);
            // printf("new_base_ptr + segmentNodeBuddy_offset: %llu\n", reinterpret_cast<uint64_t>(new_base_ptr + segmentNodeBuddy_offset));
            // printf("修正目录项: i: %llu, _buddy: %llu, stride: %llu, globalDepth: %d, segmentNodelocalDepth: %d, segmentNodeBuddy_offset: %llu, segmentNodeBuddy->segmentPtr: %llu\n",
            //        i, _buddy, stride, globalDepth, segmentNode->localDepth, segmentNodeBuddy_offset, segmentNodeBuddy->segmentPtr);

            if (segmentNode->localDepth < segmentNodeBuddy->localDepth) {
                memcpy(segmentNodeBuddy, segmentNode, sizeof(ROERTSegmentNode));
            }
        }

        i = buddyIndex;
        // i++;

        // 跳过重复的段引用
        // if (segment == lastSegment) {
        //     continue;
        // } else {
        //     lastSegment = segment;
        // }

        uint64_t sharedBucketOffset = segmentNode->sharedBucketOffset == 0 ? 0 : segmentNode->sharedBucketOffset + 1;

        if (segment != nullptr) {
            // 遍历段中的所有桶
            for (uint32_t j = 0; j < ROERT_SEGMENT_SIZE; j++) {
                // 遍历桶中的所有slot
                for (uint32_t k = 0; k < ROERT_SLOTS_PER_BUCKET; k++) {
                    ROERTSlotKeyValue* slot = &segment->buckets[j].counters[k];

                    if (j >= sharedBucketOffset && j < sharedBucketOffset + ROERT_SHARED_BUCKETS_PER_SEGMENT) {
                        if (isInvalidSharedSlot(slot, segmentNode->localDepth)) {
                            break;
                        }
                        if (position != ROERT_KEY_LENGTH) {
                            // 如果是节点指针，递归计算子节点的内存占用
                            if (slot->subKeyFields.nodeFlag == 0) {
                                // 计算旧 slot 指针的 offset
                                uintptr_t slot_offset = reinterpret_cast<uintptr_t>(slot->valueFields.value) - old_base;
                                // 更新 slot 指针
                                slot->valueFields.value = reinterpret_cast<uint64_t>(new_base_ptr + slot_offset);

                                if (leaf_cnt != 2) {
                                    leaf[leaf_cnt++] = (reinterpret_cast<ROERTKeyValue*>(slot->valueFields.value))->key;
                                }
                            } else {
                                // 计算旧 child 指针的 offset
                                uintptr_t child_offset = reinterpret_cast<uintptr_t>(slot->valueFields.value) - old_base;
                                // 更新 child 指针
                                slot->valueFields.value = reinterpret_cast<uint64_t>(new_base_ptr + child_offset);
                                recoveryAllNodes(reinterpret_cast<ROERTNode*>(slot->valueFields.value),
                                                 new_base_ptr, old_base, position, nodeHeaderDepth + _roert_ptr->root->directoryNode.header.prefixLength + 1);
                            }
                        }
                    } else {
                        if (isInvalidMainSlot(slot, segmentNode->localDepth)) {
                            break;
                        }
                        if (position != ROERT_KEY_LENGTH) {
                            // 如果是节点指针，递归计算子节点的内存占用
                            if (slot->subKeyFields.nodeFlag == 0) {
                                // 计算旧 slot 指针的 offset
                                uintptr_t slot_offset = reinterpret_cast<uintptr_t>(slot->valueFields.value) - old_base;
                                // 更新 slot 指针
                                slot->valueFields.value = reinterpret_cast<uint64_t>(new_base_ptr + slot_offset);
                                if (leaf_cnt != 2) {
                                    leaf[leaf_cnt++] = (reinterpret_cast<ROERTKeyValue*>(slot->valueFields.value))->key;
                                }
                            } else {
                                // 计算旧 child 指针的 offset
                                uintptr_t child_offset = reinterpret_cast<uintptr_t>(slot->valueFields.value) - old_base;
                                // 更新 child 指针
                                slot->valueFields.value = reinterpret_cast<uint64_t>(new_base_ptr + child_offset);
                                recoveryAllNodes(reinterpret_cast<ROERTNode*>(slot->valueFields.value),
                                                 new_base_ptr, old_base, position, nodeHeaderDepth + _roert_ptr->root->directoryNode.header.prefixLength + 1);
                            }
                        }
                    }
                }
            }
        }
    }

    // 路径压缩的崩溃恢复
    if (_roert_ptr->root->directoryNode.header.nodeHeaderDepth != nodeHeaderDepth && leaf_cnt == 2) {
        int prefixCommonLength = _roert_ptr->root->directoryNode.header.computeCommonPrefix(leaf[0], leaf[1], nodeHeaderDepth * ROERT_NODE_SUBKEY_MAX_BYTES);
        // 构建新的节点头信息
        _roert_ptr->root->directoryNode.header.nodeHeaderDepth = nodeHeaderDepth;
        _roert_ptr->root->directoryNode.header.prefixLength = prefixCommonLength / ROERT_NODE_SUBKEY_MAX_BYTES;
        // 将计算出的前缀字符填入节点头的 prefixArray 数组中
        if (_roert_ptr->root->directoryNode.header.prefixLength > 0) {
            _roert_ptr->root->directoryNode.header.assignPrefix(leaf[1], nodeHeaderDepth * ROERT_NODE_SUBKEY_MAX_BYTES * ROERT_SIZE_OF_CHAR);
        }
        clflush((char*)&_roert_ptr->root->directoryNode.header, sizeof(ROERTHeader));
    }

    // 刷新内存，确保写入可见
    // clflush((char*)_roert_ptr, sizeof(ROERT));
    return _roert_ptr;
}

void ROERT::recoveryAllNodes(ROERTNode* node, char* new_base_ptr, uintptr_t old_base, int position, int nodeHeaderDepth) {
    if (node->directoryNode.header.prefixLength != 0) {
        position += node->directoryNode.header.prefixLength * ROERT_NODE_SUBKEY_LENGTH;
    }

    position += ROERT_NODE_SUBKEY_LENGTH;
    ROERTSegment* lastSegment = nullptr;

    const uint8_t globalDepth = node->directoryNode.directory.globalDepth;

    // 找到任意两个叶子节点
    uint64_t leaf[2];
    uint64_t leaf_cnt = 0;

    for (uint64_t i = 0; i < node->directoryNode.capacity; i++) {
        // 获取段节点指针
        ROERTSegmentNode* segmentNode = reinterpret_cast<ROERTSegmentNode*>(
          ROERT_GET_SEGMENT_POSITION(&node->directoryNode, i));

        // 计算旧段节点指针的 offset
        uintptr_t segmentNode_offset = reinterpret_cast<uintptr_t>(segmentNode->segmentPtr) - old_base;

        // 更新段节点指针
        segmentNode->segmentPtr = reinterpret_cast<uint64_t>(new_base_ptr + segmentNode_offset);

        // 获取段指针
        ROERTSegment* segment = reinterpret_cast<ROERTSegment*>(segmentNode->segmentPtr);

        // 恢复目录节点
        uint64_t stride = 1ULL << (globalDepth - segmentNode->localDepth);
        uint64_t buddyIndexStart = i & (~(stride - 1));
        uint64_t buddyIndex = buddyIndexStart + stride;

        // printf("i: %llu, buddyIndex: %llu, stride: %llu, capacity: %llu, globalDepth: %d, segmentNodelocalDepth: %d, segmentNode_offset: %llu, segmentNode->segmentPtr: %llu\n",
        //        i, buddyIndex, stride, node->directoryNode.capacity, globalDepth, segmentNode->localDepth, segmentNode_offset, segmentNode->segmentPtr);

        // 伙伴深度更大，说明当前项是分裂后的左半部分，需要将中间不一致的部分修正
        for (uint64_t _buddy = buddyIndex - 1; _buddy > i; _buddy--) {
            // 获取伙伴目录项 (Buddy)
            ROERTSegmentNode* segmentNodeBuddy = reinterpret_cast<ROERTSegmentNode*>(
              ROERT_GET_SEGMENT_POSITION(&node->directoryNode, _buddy));
            // 计算旧段节点指针的 offset
            uintptr_t segmentNodeBuddy_offset = reinterpret_cast<uintptr_t>(segmentNodeBuddy->segmentPtr) - old_base;
            // 更新伙伴段节点指针
            segmentNodeBuddy->segmentPtr = reinterpret_cast<uint64_t>(new_base_ptr + segmentNodeBuddy_offset);
            // printf("new_base_ptr + segmentNodeBuddy_offset: %llu\n", reinterpret_cast<uint64_t>(new_base_ptr + segmentNodeBuddy_offset));
            // printf("修正目录项: i: %llu, _buddy: %llu, stride: %llu, globalDepth: %d, segmentNodelocalDepth: %d, segmentNodeBuddy_offset: %llu, segmentNodeBuddy->segmentPtr: %llu\n",
            //        i, _buddy, stride, globalDepth, segmentNode->localDepth, segmentNodeBuddy_offset, segmentNodeBuddy->segmentPtr);

            if (segmentNode->localDepth < segmentNodeBuddy->localDepth) {
                memcpy(segmentNodeBuddy, segmentNode, sizeof(ROERTSegmentNode));
            }
        }

        i = buddyIndex;

        // i++;

        // 跳过重复的段引用
        // if (segment == lastSegment) {
        //     continue;
        // } else {
        //     lastSegment = segment;
        // }

        uint64_t sharedBucketOffset = segmentNode->sharedBucketOffset == 0 ? 0 : segmentNode->sharedBucketOffset + 1;

        if (segment != nullptr) {
            // 遍历段中的所有桶
            for (uint32_t j = 0; j < ROERT_SEGMENT_SIZE; j++) {
                // 遍历桶中的所有slot
                for (uint32_t k = 0; k < ROERT_SLOTS_PER_BUCKET; k++) {
                    ROERTSlotKeyValue* slot = &segment->buckets[j].counters[k];

                    // printf("恢复段节点指针: segmentIndex: %llu, segmentNodePtr: %p, localDepth: %d, sharedBucketOffset: %d\n",
                    //        i, segmentNode->segmentPtr, segmentNode->localDepth, segmentNode->sharedBucketOffset);

                    if (j >= sharedBucketOffset && j < sharedBucketOffset + ROERT_SHARED_BUCKETS_PER_SEGMENT) {
                        if (isInvalidSharedSlot(slot, segmentNode->localDepth)) {
                            break;
                        }
                        if (position != ROERT_KEY_LENGTH) {
                            // 如果是节点指针，递归计算子节点的内存占用
                            if (slot->subKeyFields.nodeFlag == 0) {
                                // 计算旧 slot 指针的 offset
                                uintptr_t slot_offset = reinterpret_cast<uintptr_t>(slot->valueFields.value) - old_base;
                                // 更新 slot 指针
                                slot->valueFields.value = reinterpret_cast<uint64_t>(new_base_ptr + slot_offset);
                                if (leaf_cnt != 2) {
                                    leaf[leaf_cnt++] = (reinterpret_cast<ROERTKeyValue*>(slot->valueFields.value))->key;
                                }
                            } else {
                                // 计算旧 child 指针的 offset
                                uintptr_t child_offset = reinterpret_cast<uintptr_t>(slot->valueFields.value) - old_base;
                                // 更新 child 指针
                                slot->valueFields.value = reinterpret_cast<uint64_t>(new_base_ptr + child_offset);
                                recoveryAllNodes(reinterpret_cast<ROERTNode*>(slot->valueFields.value),
                                                 new_base_ptr, old_base, position, nodeHeaderDepth + node->directoryNode.header.prefixLength + 1);
                            }
                        }
                    } else {
                        if (isInvalidMainSlot(slot, segmentNode->localDepth)) {
                            break;
                        }
                        if (position != ROERT_KEY_LENGTH) {
                            // 如果是节点指针，递归计算子节点的内存占用
                            if (slot->subKeyFields.nodeFlag == 0) {
                                // 计算旧 slot 指针的 offset
                                uintptr_t slot_offset = reinterpret_cast<uintptr_t>(slot->valueFields.value) - old_base;
                                // 更新 slot 指针
                                slot->valueFields.value = reinterpret_cast<uint64_t>(new_base_ptr + slot_offset);
                                if (leaf_cnt != 2) {
                                    leaf[leaf_cnt++] = (reinterpret_cast<ROERTKeyValue*>(slot->valueFields.value))->key;
                                }
                            } else {
                                // 计算旧 child 指针的 offset
                                uintptr_t child_offset = reinterpret_cast<uintptr_t>(slot->valueFields.value) - old_base;
                                // 更新 child 指针
                                slot->valueFields.value = reinterpret_cast<uint64_t>(new_base_ptr + child_offset);
                                recoveryAllNodes(reinterpret_cast<ROERTNode*>(slot->valueFields.value),
                                                 new_base_ptr, old_base, position, nodeHeaderDepth + node->directoryNode.header.prefixLength + 1);
                            }
                        }
                    }
                }
            }
        }
    }

    // 路径压缩的崩溃恢复
    if (node->directoryNode.header.nodeHeaderDepth != nodeHeaderDepth && leaf_cnt == 2) {
        int prefixCommonLength = node->directoryNode.header.computeCommonPrefix(leaf[0], leaf[1], nodeHeaderDepth * ROERT_NODE_SUBKEY_MAX_BYTES);
        // 构建新的节点头信息
        node->directoryNode.header.nodeHeaderDepth = nodeHeaderDepth;
        node->directoryNode.header.prefixLength = prefixCommonLength / ROERT_NODE_SUBKEY_MAX_BYTES;
        // 将计算出的前缀字符填入节点头的 prefixArray 数组中
        if (node->directoryNode.header.prefixLength > 0) {
            node->directoryNode.header.assignPrefix(leaf[1], nodeHeaderDepth * ROERT_NODE_SUBKEY_MAX_BYTES * ROERT_SIZE_OF_CHAR);
        }
        clflush((char*)&node->directoryNode.header, sizeof(ROERTHeader));
    }
}

ROERT::ROERT() {
    root = NewROERTNode();

    // 持久化 ROERTNode* root 根节点指针;
    // clflush((char*)root, sizeof(ROERTNode*));
}

ROERT::ROERT(uint32_t globalDepthTree) : globalDepthTree(globalDepthTree) {
    ROERT::initialize();
}

ROERT::~ROERT() {
    delete root;
}

void ROERT::initialize() {
    // 创建根节点
    root = NewROERTNode();
}

ROERT* NewROERT() {
    // 使用快速内存分配器分配 ROERT 对象的内存空间
    // 大小：sizeof(ROERT) - 包含 globalDepthTree 和 root 指针
    ROERT* _new_roert = static_cast<ROERT*>(concurrency_fast_alloc(sizeof(ROERT)));
    // printf("sizeof(ROERT): %llu\n", sizeof(ROERT));
    // 初始化 ROERT 对象
    _new_roert->initialize();

    _new_roert->_base_ptr = reinterpret_cast<char*>(_new_roert);

    // _new_roert->globalDepthTree = 1;

    // printf("_new_roert: %p, _base_ptr: %p, root: %p, globalDepthTree: %d\n", _new_roert, _new_roert->_base_ptr, _new_roert->root, _new_roert->globalDepthTree);

    // 刷新内存，确保写入可见
    // clflush((char*)_new_roert, sizeof(ROERT));

    return _new_roert;
}

} // namespace roert
