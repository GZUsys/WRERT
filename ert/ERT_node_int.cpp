#include "ERT_node_int.h"

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

ERTIntKeyValue* NewERTIntKeyValue(uint64_t key, uint64_t value) {
    ERTIntKeyValue* _new_key_value = static_cast<ERTIntKeyValue*>(concurrency_fast_alloc(sizeof(ERTIntKeyValue)));
    _new_key_value->key = key;
    _new_key_value->value = value;
    return _new_key_value;
}

uint64_t ERTIntBucket::get(uint64_t key, bool& keyValueFlag) {
    for (int i = 0; i < ERT_BUCKET_SIZE; ++i) {
        // printf("槽位 %d subkey: 0x%08llx, value: %llu\n", i, REMOVE_NODE_FLAG(counter[i].subkey), counter[i].value);
        if (key == REMOVE_NODE_FLAG(counter[i].subkey)) {
            keyValueFlag = GET_NODE_FLAG(counter[i].subkey);
            return counter[i].value;
        }
    }
    return -1;
}

/**
 * ERTIntBucket::findPlace - 在桶中查找子键的插入位置
 *
 * 功能：在桶的4个槽位中查找指定子键的合适插入位置
 * 该方法支持三种查找策略，按优先级顺序执行：
 * 1. 查找完全匹配的子键（用于更新操作）
 * 2. 查找空槽位（用于插入操作）
 * 3. 查找段号不同的槽位（用于冲突处理）
 *
 * 参数：
 *   _key: 要查找的子键（32位，已移除标志位）
 *   _key_len: 子键长度（通常为ERT_NODE_LENGTH=32）
 *   _depth: 当前段的深度，用于计算段号
 *
 * 返回值：
 *   -1: 桶已满，没有可用位置
 *   >=0: 找到的槽位索引（0-3）
 *
 * 算法流程：
 *   1. 遍历桶的4个槽位
 *   2. 如果找到完全匹配的子键，返回该槽位索引
 *   3. 如果找到空槽位（子键和值都为0），记录为候选位置
 *   4. 如果找到段号不同的槽位，记录为候选位置（用于冲突处理）
 *   5. 返回第一个找到的候选位置，如果没有则返回-1
 */
int ERTIntBucket::findPlace(uint64_t _key, uint64_t _key_len, uint64_t _depth) {
    // full: return -1
    // exists or not full: return index or empty counter
    // 初始化返回值为-1（表示桶满）
    int res = -1;
    for (int i = 0; i < ERT_BUCKET_SIZE; ++i) {
        // 移除当前槽位子键的标志位，获取实际子键值
        uint64_t removedFlagKey = REMOVE_NODE_FLAG(counter[i].subkey);

        // 策略1：完全匹配查找（最高优先级）
        if (_key == removedFlagKey) {
            // 找到完全匹配的子键，直接返回该槽位索引
            // 用于后续的更新操作
            return i;
        }
        // 策略2：空槽位查找（次高优先级）
        else if ((res == -1) && removedFlagKey == 0 && counter[i].value == 0) {
            // 找到空槽位（子键和值都为0），记录为候选位置
            // 条件：res == -1 确保只记录第一个找到的空槽位
            res = i;
        }
        // 策略3：段号不同查找（最低优先级，有逻辑问题）
        else if ((res == -1) && (GET_SEG_NUM(_key, _key_len, _depth) != GET_SEG_NUM(removedFlagKey, _key_len, _depth))) { // todo: wrong logic
            res = i;
        }
    }
    // 返回找到的候选位置，如果没有找到则返回-1（桶满）
    return res;
}

ERTIntSegment::ERTIntSegment() {
    depth = 0;
    bucket = static_cast<ERTIntBucket*>(concurrency_fast_alloc(sizeof(ERTIntBucket) * ERT_MAX_BUCKET_NUM));
}

ERTIntSegment::~ERTIntSegment() {}

// ERTIntSegment类的初始化函数
// 功能：初始化段结构，设置深度参数并分配桶数组内存
// 参数：
//   _depth: 段深度，用于确定段的层级信息
void ERTIntSegment::init(uint64_t _depth) {
    // 设置段的深度参数
    depth = _depth;

    // 使用快速内存分配器分配桶数组内存
    // 分配大小 = 单个桶大小 × 最大桶数量（ERT_MAX_BUCKET_NUM = 256）
    // 即：sizeof(ERTIntBucket) × 256，为段分配完整的桶数组空间
    bucket = static_cast<ERTIntBucket*>(concurrency_fast_alloc(sizeof(ERTIntBucket) * ERT_MAX_BUCKET_NUM));
    // printf("bucket[0].counter[0].subkey: 0x%016llx, value: 0x%016llx\n", bucket[0].counter[0].subkey, bucket[0].counter[0].value);
}

ERTIntSegment* NewERTIntSegment(uint64_t _depth) {
    ERTIntSegment* _new_ht_segment = static_cast<ERTIntSegment*>(concurrency_fast_alloc(sizeof(ERTIntSegment)));
    _new_ht_segment->init(_depth);
    return _new_ht_segment;
}

void ERTIntHeader::init(ERTIntHeader* oldHeader, unsigned char length, unsigned char depth) {
    assign(oldHeader->array, length);
    this->depth = depth;
    this->len = length;
}

int ERTIntHeader::computePrefix(uint64_t key, int startPos) {
    // 如果前缀长度为0，直接返回0（没有前缀可匹配）
    if (this->len == 0) {
        return 0;
    }
    // 从键的startPos位置提取len*8位的子键
    // uint64_t subkey = GET_SUBKEY(key, startPos, (this->len * SIZE_OF_CHAR));
    uint64_t subkey = GET_SUBKEY(key, startPos,
                                 (this->len == ERT_NODE_LENGTH / SIZE_OF_CHAR ? this->len * SIZE_OF_CHAR : ERT_NODE_LENGTH));
    // printf("key: 0x%016llx; startPos: %d; subkey: 0x%08llx\n", key, startPos, subkey);
    // 将子键左移到64位最高位对齐，便于逐字节比较
    // printf("subkey: 0x%08llx\n", subkey);
    // 例如：如果len=4（32位），则左移(64-32)=32位
    // subkey <<= (64 - this->len * SIZE_OF_CHAR);
    subkey <<= (64 - (this->len == ERT_NODE_LENGTH / SIZE_OF_CHAR ? this->len * SIZE_OF_CHAR : ERT_NODE_LENGTH));

    int res = 0;
    int size = this->len % (ERT_NODE_LENGTH / SIZE_OF_CHAR) == 0 ? this->len : this->len / (ERT_NODE_LENGTH / SIZE_OF_CHAR);
    // 逐字节比较子键与节点前缀数组
    // for (int i = 0; i < this->len; i++) {
    for (int i = 0; i < size; i++) {
        // printf("subKey = %02llx, array[%d] = %02x, res = %d\n", ((subkey >> (56 - i * 8)) & 0xFF), i, array[i], res);

        // 比较子键的最高字节（右移56位得到最高8位）与节点前缀数组的第i个字节
        // if ((subkey >> 56) != ((uint64_t)array[i])) {
        if (((subkey >> (56 - i * 8)) & 0xFF) != ((uint64_t)array[i])) {
            break;
        }
        res++;
    }

    return res;
}

/**
 * @brief 从64位键中分配前缀到头部数组
 *
 * 该方法用于从给定的64位键中提取指定位置和长度的子键，并将其存储到头部的前缀数组中。
 * 前缀存储采用大端字节序（从高位到低位），便于后续的前缀匹配和比较操作。
 *
 * @param key 输入的64位键值
 * @param startPos 开始提取的位置（以位为单位，从键的最高位开始计算）
 *
 * @note 算法步骤：
 * 1. 从键的startPos位置提取len*8位的子键
 * 2. 将子键左移到前缀数组的最高位对齐
 * 3. 按字节从高位到低位存储到array数组中
 */
void ERTIntHeader::assign(uint64_t key, int startPos) {
    // 从键的 startPos 位置提取 len*8 位的子键
    // GET_SUBKEY宏: (key >> (64 - startPos - length)) & ((1 << length) - 1)
    uint64_t subkey = GET_SUBKEY(key, startPos * SIZE_OF_CHAR, (this->len * SIZE_OF_CHAR));
    // printf("subkey = 0x%08llx\n", subkey);

    // 将子键左移到前缀数组的最高位对齐
    // 例如：如果len=6（48位），ERT_NODE_PREFIX_MAX_BITS=48，则不需要移位
    subkey <<= (ERT_NODE_PREFIX_MAX_BITS - this->len * SIZE_OF_CHAR);
    // 按字节从高位到低位存储到 array 数组中（大端字节序）
    for (int i = ERT_NODE_PREFIX_MAX_BYTES - 1; i >= 0; i--) {
        // 提取当前字节：subkey & 0xFF
        array[i] = (char)subkey & (((uint64_t)1 << 8) - 1);
        // 右移 8 位处理下一个字节
        subkey >>= 8;

        // printf("array[%d] = %02x\n", i, array[i]);
    }
}

void ERTIntHeader::assign(unsigned char* key, unsigned char assignedLength) {
    for (int i = 0; i < assignedLength; i++) {
        array[i] = key[i];
    }
}

ERTIntNode::ERTIntNode() {
    global_depth = 0;
    dir_size = pow(2, global_depth);
    for (int i = 0; i < dir_size; ++i) {
        *(ERTIntSegment**)GET_SEG_POS(this, i) = NewERTIntSegment();
    }
}

ERTIntNode::~ERTIntNode() {}

// ERTIntNode类的初始化函数
// 功能：初始化节点结构，设置深度参数并分配相关内存
// 参数：
//   headerDepth: 头部深度，控制前缀长度
//   global_depth: 全局深度，决定目录大小和段指针数量
void ERTIntNode::init(unsigned char headerDepth, unsigned char global_depth) {
    // 设置节点的全局深度
    this->global_depth = global_depth;

    // 计算目录大小：2^global_depth，即段指针的数量
    this->dir_size = pow(2, global_depth);

    // 设置头部深度，用于前缀管理
    header.depth = headerDepth;

    header.len = 7;

    // printf("Initializing ERTIntNode: global_depth=%u, dir_size=%u, headerDepth=%u, header.depth=%u, header.len=%u\n", this->global_depth,
    //        this->dir_size, headerDepth, header.depth, header.len);

    // 分配节点值数组内存
    // 数组大小 = 1 + (ERT_NODE_PREFIX_MAX_BITS / ERT_NODE_LENGTH)
    // 即：1 + (48位最大前缀 / 32位节点长度) = 1 + 1 = 2
    treeNodeValues = static_cast<ERTIntKeyValue*>(concurrency_fast_alloc(
      sizeof(ERTIntKeyValue) * (1 + ERT_NODE_PREFIX_MAX_BITS / ERT_NODE_LENGTH)));
    // 为每个目录索引创建并分配段指针
    for (int i = 0; i < this->dir_size; ++i) {
        // 使用 GET_SEG_POS 宏计算段指针位置
        // 创建新的段对象并赋值给对应的目录位置
        *(ERTIntSegment**)GET_SEG_POS(this, i) = NewERTIntSegment(global_depth);
    }
}

void ERTIntNode::put(uint64_t subkey, uint64_t value, uint64_t kvFLAG, uint64_t beforeAddress) {
    uint64_t dir_index = GET_SEG_NUM(subkey, ERT_NODE_LENGTH, global_depth);
    ERTIntSegment* tmp_seg = *(ERTIntSegment**)GET_SEG_POS(this, dir_index);
    uint64_t seg_index = GET_BUCKET_NUM(subkey, ERT_BUCKET_MASK_LEN);
    ERTIntBucket* tmp_bucket = &(tmp_seg->bucket[seg_index]);
    // printf("put: subkey: 0x%08llx, value: %llu, dir_index: %llu, seg_index: %llu\n", subkey, value, dir_index, seg_index);
    put(subkey, value, tmp_seg, tmp_bucket, dir_index, seg_index, kvFLAG, beforeAddress);
}

/**
 * ERTIntNode::put - 在ERT节点中插入键值对（完整版本）
 *
 * 功能：将子键和值插入到指定的段和桶中，处理桶满时的分裂和节点扩展
 * 这是ERT插入操作的核心方法，支持段分裂和节点扩展等复杂操作
 *
 * 参数：
 *   subkey: 要插入的子键（32位）
 *   value: 对应的值（64位指针，指向键值对或节点）
 *   tmp_seg: 目标段指针，子键对应的段
 *   tmp_bucket: 目标桶指针，子键对应的桶
 *   dir_index: 目录索引，用于定位段
 *   seg_index: 段内索引，用于定位桶
 *   beforeAddress: 前驱节点地址，用于更新指针
 *
 * 算法流程：
 *   1. 在桶中查找插入位置
 *   2. 如果桶未满，直接插入
 *   3. 如果桶满，根据段深度决定分裂策略：
 *      - 段深度 < 全局深度：段分裂
 *      - 段深度 == 全局深度：节点扩展
 */

std::vector<uint64_t> ert_insert_segment_split_time_ = {};
std::vector<uint64_t> ert_insert_directory_grow_time_ = {};
std::vector<uint64_t> ert_per_insert_node_update_latency_ = {};

void ERTIntNode::put(uint64_t subkey, uint64_t value, ERTIntSegment* tmp_seg, ERTIntBucket* tmp_bucket, uint64_t dir_index,
                     uint64_t seg_index, uint64_t kvFLAG, uint64_t beforeAddress) {
    // 在桶中查找可用的插入位置
    int bucket_index = tmp_bucket->findPlace(subkey, ERT_NODE_LENGTH, tmp_seg->depth);

    if (bucket_index == -1) // 情况1：桶已满（bucket_index == -1），需要分裂处理
    {
        // condition: full
        if (likely(tmp_seg->depth < global_depth)) // 情况1.1：段深度小于全局深度，进行段分裂
        {
            // printf("情况1.1：段深度小于全局深度，进行段分裂\n");
            // auto start_time = std::chrono::high_resolution_clock::now();

            // 创建新的段，深度增加1
            ERTIntSegment* new_seg = NewERTIntSegment(tmp_seg->depth + 1);

            // 计算分裂步长：2^(全局深度-段深度)
            int64_t stride = pow(2, global_depth - tmp_seg->depth);
            int64_t left = dir_index - dir_index % stride;
            int64_t mid = left + stride / 2, right = left + stride;

            // migrate previous data to the new bucket
            // 迁移数据到新段：将属于[mid, right)区间的键值对迁移到新段
            for (int i = 0; i < ERT_MAX_BUCKET_NUM; ++i) {
                uint64_t bucket_cnt = 0;
                for (int j = 0; j < ERT_BUCKET_SIZE; ++j) {
                    // 移除标志位获取实际子键
                    uint64_t tmp_key = REMOVE_NODE_FLAG(tmp_seg->bucket[i].counter[j].subkey);
                    uint64_t tmp_value = tmp_seg->bucket[i].counter[j].value;

                    // 计算子键对应的目录索引
                    dir_index = GET_SEG_NUM(tmp_key, ERT_NODE_LENGTH, global_depth);

                    // 如果目录索引在[mid, right)区间，迁移到新段
                    if (dir_index >= mid) {
                        ERTIntSegment* dst_seg = new_seg;
                        seg_index = i;
                        ERTIntBucket* dst_bucket = &(dst_seg->bucket[seg_index]);

                        // 复制键值对到新段的对应桶
                        dst_bucket->counter[bucket_cnt].value = tmp_value;
                        dst_bucket->counter[bucket_cnt].subkey = tmp_seg->bucket[i].counter[j].subkey;
                        bucket_cnt++;
                    }
                }
            }

            // printf("时间: %lu ns\n", ert_insert_segment_split_time_.back());

            // 持久化新段到NVM
            clflush((char*)new_seg, sizeof(ERTIntSegment));

            clflush((char*)new_seg->bucket, sizeof(ERTIntBucket) * ERT_MAX_BUCKET_NUM);

            // set dir[mid, right) to the new bucket
            // 更新目录：将[mid, right)区间的段指针指向新段
            for (int i = right - 1; i >= mid; --i) {
                *(ERTIntSegment**)GET_SEG_POS(this, i) = new_seg;
            }
            // std::cout << "ERT 迁移共享桶循环执行时间: " << duration.count() << " 纳秒 (" << duration.count() / 1000000.0 << " 毫秒)" << std::endl;

            // 持久化目录更新到NVM
            clflush((char*)GET_SEG_POS(this, right - 1), sizeof(ERTIntSegment*) * (right - mid));

            // 增加原段深度并持久化
            tmp_seg->depth = tmp_seg->depth + 1;
            clflush((char*)&(tmp_seg->depth), sizeof(tmp_seg->depth));

            // auto end_time = std::chrono::high_resolution_clock::now();
            // ert_insert_segment_split_time_.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count());

            // 重新尝试插入（现在桶应该有空间了）
            this->put(subkey, value, kvFLAG, beforeAddress);

            return;
        } else {
            // printf("情况1.2：段深度等于全局深度，需要扩展节点\n");
            // ert_insert_directory_grow_num_++;
            // printf("ert_insert_directory_grow_num_: %d\n", ert_insert_directory_grow_num_);

            // auto start_time = std::chrono::high_resolution_clock::now();
            // auto end_time = std::chrono::high_resolution_clock::now();
            // ert_insert_directory_grow_time_.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count());

            // condition: tmp_bucket->depth == global_depth
            // 情况1.2：段深度等于全局深度，需要扩展节点
            // 创建新节点，全局深度增加1，目录大小翻倍
            ERTIntNode* newNode = static_cast<ERTIntNode*>(concurrency_fast_alloc(
              sizeof(ERTIntNode) + sizeof(ERTIntNode*) * dir_size * 2));
            newNode->global_depth = global_depth + 1;
            newNode->dir_size = dir_size * 2;

            // 初始化新节点的头部信息（复制当前节点的前缀）
            newNode->header.init(&this->header, this->header.len, this->header.depth);
            // set dir
            // 设置新节点的目录：每个旧目录项对应新目录的两个项
            for (int i = 0; i < newNode->dir_size; ++i) {
                *(ERTIntSegment**)GET_SEG_POS(newNode, i) = *(ERTIntSegment**)GET_SEG_POS(this, (i / 2));
            }

            // printf("newNode->dir_size: %u\n", newNode->dir_size);
            // 持久化新节点
            clflush((char*)newNode, sizeof(ERTIntNode) + sizeof(ERTIntSegment*) * newNode->dir_size);
            // 更新前驱节点指针指向新节点
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

            // 在新节点中重新插入键值对
            newNode->put(subkey, value, kvFLAG, beforeAddress);

            return;
        }
    } else // 情况2：桶有可用空间，直接插入
    {
        // 情况2.1：子键已存在（更新操作）
        if (unlikely(tmp_bucket->counter[bucket_index].subkey == subkey) && subkey != 0) {
            // key exists，键已存在，更新值
            tmp_bucket->counter[bucket_index].value = value;
            clflush((char*)&(tmp_bucket->counter[bucket_index].value), 8);

            // printf("情况2.1：桶有可用空间，直接插入，子键已存在（更新操作）\n");
        } else // 情况2.2：子键不存在（插入操作），先设置值，然后设置带标志位的子键（防止部分写入）
        {
            // there is a place to insert
            tmp_bucket->counter[bucket_index].value = value;
            // 内存屏障，确保值写入完成
            mfence();
            // 设置带键值对标志位的子键（第56位=1表示存储的是键值对指针）
            if (kvFLAG) {
                /* code */
                tmp_bucket->counter[bucket_index].subkey = PUT_KEY_VALUE_FLAG(subkey);
            } else {
                tmp_bucket->counter[bucket_index].subkey = REMOVE_NODE_FLAG(subkey);
            }

            // Here we clflush 16bytes rather than two 8 bytes because all counter are set to 0.
            // If crash after key flushed, then the value is 0. When we return the value, we w|ould find that the key is not inserted.
            // 持久化16字节（子键和值一起刷新）
            // 设计考虑：如果崩溃发生在子键刷新后但值刷新前，查询时会发现键不存在
            clflush((char*)&(tmp_bucket->counter[bucket_index].subkey), 16);

            // printf("情况2.2：桶有可用空间，直接插入，子键不存在（插入操作），直接插入并持久化\n");
        }
    }
    return;
}

void ERTIntNode::nodePut(int pos, ERTIntKeyValue* kv) {
    treeNodeValues[header.len - pos] = *kv;
}

uint64_t ERTIntNode::get(uint64_t subkey, bool& keyValueFlag) {
    uint64_t dir_index = GET_SEG_NUM(subkey, ERT_NODE_LENGTH, global_depth);
    ERTIntSegment* tmp_seg = *(ERTIntSegment**)GET_SEG_POS(this, dir_index);
    uint64_t seg_index = GET_BUCKET_NUM(subkey, ERT_BUCKET_MASK_LEN);
    ERTIntBucket* tmp_bucket = &(tmp_seg->bucket[seg_index]);
    // printf("get: subkey: 0x%08llx, dir_index: %llu, seg_index: %llu\n", subkey, dir_index, seg_index);

    return tmp_bucket->get(subkey, keyValueFlag);
}

// 创建新的ERTIntNode节点
// 参数：
//   _key_len: 键长度（通常为ERT_NODE_LENGTH=32）
//   headerDepth: 头部深度，控制前缀长度
//   globalDepth: 全局深度，决定目录大小（2^globalDepth个段指针）
// 返回值：指向新创建的ERTIntNode对象的指针
ERTIntNode* NewERTIntNode(int _key_len, unsigned char headerDepth, unsigned char globalDepth) {
    // 使用快速内存分配器分配节点内存空间
    // 内存大小 = 节点结构体大小 + 段指针数组大小（2^globalDepth个指针）
    ERTIntNode* _new_node = static_cast<ERTIntNode*>(concurrency_fast_alloc(
      sizeof(ERTIntNode) + sizeof(ERTIntSegment*) * pow(2, globalDepth)));
    // 初始化新创建的节点，设置头部深度和全局深度
    _new_node->init(headerDepth, globalDepth);

    // 返回初始化后的节点指针
    return _new_node;
}
