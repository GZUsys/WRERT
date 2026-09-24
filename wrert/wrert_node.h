#ifndef NVMKV_WRERT_NODE_H
#define NVMKV_WRERT_NODE_H

#include <cstdint>
#include <vector>
#include <memory>
#include <atomic>
#include <stdexcept>
#include <immintrin.h>

#include "../fastalloc/fastalloc.h"

namespace wrert {

#ifdef __GNUC__
#define WRERT_LIKELY(x) (__builtin_expect(!!(x), 1))
#define WRERT_UNLIKELY(x) (__builtin_expect(!!(x), 0))
#else
#define WRERT_LIKELY(x) (x)
#define WRERT_UNLIKELY(x) (x)
#endif

// 获取段索引（目录索引）
#define WRERT_GET_SEGMENT_NUMBER(key, keyLength, globalDepth) \
    ((key >> (keyLength - globalDepth)) & (((uint64_t)1 << globalDepth) - 1))

// 获取桶索引（共享桶索引）
#define WRERT_GET_BUCKET_NUMBER(key, bucketMaskLength) \
    ((key) & (((uint64_t)1 << bucketMaskLength) - 1))

#define WRERT_GET_SEGMENT_POSITION(currentNode, directoryIndex) \
    (((uint64_t)(currentNode) + sizeof(WRERTNode) + directoryIndex * sizeof(WRERTSegmentNode)))

#define WRERT_GET_SUBKEY(key, startPosition, subKeyLength) \
    ((key >> (64 - startPosition - subKeyLength) & (((uint64_t)1 << subKeyLength) - 1)))

#define WRERT_REMOVE_NODE_FLAG(key) \
    (key & (((uint64_t)1 << 56) - 1))

#define WRERT_PUT_KEY_VALUE_FLAG(key) \
    (key | ((uint64_t)1 << 56))

#define WRERT_GET_NODE_FLAG(key) \
    (key >> 56)

// 获取前缀数组
#define WRERT_GET_PREFIX_ARRAY_BITS(prefixArray, startPosition, subKeyLength) \
    ({                                                                        \
        uint64_t result = 0;                                                  \
                                                                              \
        /* 依次取subKeyLength个字节，合成uint64_t */                          \
        for (uint64_t i = 0; i < subKeyLength; i++) {                         \
            result = (result << 8) | prefixArray[startPosition + i];          \
        }                                                                     \
        result;                                                               \
    })

// 共享桶索引相关宏定义
// 共享桶索引：使用子键的[local_depth-1:B-1]位（5位索引32个共享桶）
#define WRERT_GET_SHARED_BUCKET_INDEX(subKey, localDepth, indexBits) \
    ((subKey >> (WRERT_NODE_SUBKEY_LENGTH - (localDepth - 1 + indexBits))) & (((uint64_t)1 << indexBits) - 1))

// 主桶索引：使用子键的[local_depth:B-1]位（6位索引64个主桶）
#define WRERT_GET_MAIN_BUCKET_INDEX(subKey, localDepth, indexBits) \
    ((subKey >> (WRERT_NODE_SUBKEY_LENGTH - (localDepth + indexBits))) & (((uint64_t)1 << indexBits) - 1))

// 调整后的主桶索引（当共享桶满时，将子键右移一位）
#define WRERT_GET_ADJUSTED_MAIN_BUCKET_INDEX(subKey, localDepth, indexBits) \
    ((subKey >> (WRERT_NODE_SUBKEY_LENGTH - (localDepth + indexBits))) & (((uint64_t)1 << indexBits) - 1))

#define WRERT_IS_SHARED_BUCKET_FULL(bucket) \
    (bucket.counters[WRERT_SLOTS_PER_BUCKET - 1].subKeyFields.validFlag == 1)

// 基本配置
constexpr uint8_t WRERT_SIZE_OF_CHAR = 8;                                                     // 字符大小（8位）
constexpr uint8_t WRERT_KEY_LENGTH = 64;                                                      // 键长度（64位）
constexpr uint8_t WRERT_KEY_MAX_BYTES = WRERT_KEY_LENGTH / WRERT_SIZE_OF_CHAR;                // 键长度（4字节）
constexpr uint8_t WRERT_NODE_SUBKEY_LENGTH = 32;                                              // 节点处理子键的长度（32位）
constexpr uint8_t WRERT_NODE_SPAN = WRERT_NODE_SUBKEY_LENGTH;                                 // 节点处理子键的跨度（32位）
constexpr uint8_t WRERT_NODE_SUBKEY_MAX_BYTES = WRERT_NODE_SPAN / WRERT_SIZE_OF_CHAR;         // 节点处理子键的最大字节数（4字节）
constexpr uint8_t WRERT_NODE_PREFIX_MAX_BYTES = 6;                                            // 节点前缀最大字节数
constexpr uint8_t WRERT_NODE_PREFIX_MAX_BITS = 48;                                            // 节点前缀最大位数（6字节）
constexpr uint8_t WRERT_NODE_PREFIX_MAX_LENGTH = WRERT_KEY_LENGTH / WRERT_NODE_SUBKEY_LENGTH; // 节点最大前缀长度（2个子键）
constexpr uint8_t WRERT_INIT_PREFIX_LENGTH = WRERT_NODE_PREFIX_MAX_BYTES;                     // 初始前缀长度
constexpr uint8_t WRERT_INIT_NODE_DEPTH = 0;                                                  // 初始节点深度
constexpr uint8_t WRERT_INIT_GLOBAL_DEPTH = 0;                                                // 初始段全局深度
constexpr uint8_t WRERT_INIT_LOCAL_DEPTH = 0;                                                 // 初始段局部深度

// 段、桶、槽配置（64个主桶+32个共享桶=96个桶，每个桶16个slot）/（32个主桶+16个共享桶=48个桶，每个桶16个slot）
constexpr uint8_t WRERT_MAIN_BUCKETS_PER_SEGMENT = 4;                                                     // 每个段的主桶数量：32（4、8、16、32、64、128）
constexpr uint8_t WRERT_MAIN_BUCKETS_PER_SEGMENT_MASK = WRERT_MAIN_BUCKETS_PER_SEGMENT - 1;               // 主桶数掩码
constexpr uint8_t WRERT_MAIN_BUCKETS_MAX_BITS = __builtin_ctz(WRERT_MAIN_BUCKETS_PER_SEGMENT);            // 每个段的主桶索引位数：6位
constexpr uint8_t WRERT_SHARED_BUCKETS_PER_SEGMENT = WRERT_MAIN_BUCKETS_PER_SEGMENT / 2;                  // 每个段的共享桶数量：32个（每2个主桶共享1个共享桶）
constexpr uint8_t WRERT_SHARED_BUCKETS_PER_SEGMENT_MASK = WRERT_SHARED_BUCKETS_PER_SEGMENT - 1;           // 共享桶数掩码
constexpr uint8_t WRERT_SHARED_BUCKETS_OFFSET = 0;                                                        // 初始共享桶偏移：0
constexpr uint8_t WRERT_SHARED_BUCKETS_MAX_BITS = __builtin_ctz(WRERT_SHARED_BUCKETS_PER_SEGMENT);        // 每个段的共享桶索引位数：5位
constexpr uint8_t WRERT_SLOTS_PER_BUCKET = 16;                                                            // 每个桶的slot数量：16个
constexpr uint8_t WRERT_SLOT_SIZE = 16;                                                                   // 每个slot大小：16字节（存储键值对）
constexpr uint8_t WRERT_SEGMENT_SIZE = WRERT_MAIN_BUCKETS_PER_SEGMENT + WRERT_SHARED_BUCKETS_PER_SEGMENT; // 每个段的桶数量：96个（64主桶 + 32共享桶）
// 计算相关常量
constexpr uint16_t WRERT_BUCKET_SIZE = WRERT_SLOTS_PER_BUCKET; // 桶大小（16个slot）

// 迁移配置
constexpr uint8_t WRERT_MIG_SHARED_BUCKET_NUM = WRERT_SHARED_BUCKETS_PER_SEGMENT / 2; // 迁移共享桶数量：16个
constexpr uint8_t WRERT_MIG_MAIN_BUCKET_NUM = WRERT_MAIN_BUCKETS_PER_SEGMENT / 2;     // 迁移主桶数量：32个

// 指纹和分裂阈值
constexpr uint8_t WRERT_BUCKET_INDEX_BIT_NUM = WRERT_MAIN_BUCKETS_MAX_BITS; // 桶索引位数：6位
constexpr uint8_t WRERT_FINGERPRINT_BITS = 16;                              // 指纹位数：16位
constexpr uint8_t WRERT_FINGERPRINT_BIT_ALIGNMENT = 8ul;                    // 指纹对齐位数：8位
constexpr uint8_t WRERT_SPLIT_THRESHOLD = 48;                               // 分裂阈值：48位

// 内存大小计算
constexpr uint32_t WRERT_BUCKET_MEMORY_SIZE = WRERT_SLOT_SIZE * WRERT_SLOTS_PER_BUCKET;       // 桶大小：256字节（16×16）
constexpr uint32_t WRERT_SEGMENT_MEMORY_SIZE = WRERT_SEGMENT_SIZE * WRERT_BUCKET_MEMORY_SIZE; // 段大小：24,576字节（96×256）

alignas(64) constexpr uint64_t offsets[8] = {0, 16, 32, 48, 64, 80, 96, 112};
constexpr __mmask8 summary_mask = 0xFF;
constexpr uint64_t WRERT_VALIDFLAG_MIGVERSION_MASK = 0x000003FF00000000ULL;        // 有效位和迁移版本号掩码
constexpr uint64_t WRERT_MIGVERSION_MASK = 0x000000FF00000000ULL;                  // 迁移版本号掩码
constexpr uint64_t WRERT_SUBKEY_MASK = 0x00000000FFFFFFFFULL;                      // 子键掩码
constexpr uint64_t WRERT_VALIDFLAG_MIGVERSION_SUBKEY_MASK = 0x000003FFFFFFFFFFULL; // 有效位和迁移版本号掩码
constexpr uint64_t WRERT_MIGVERSION_BITS = 0x1FF;                                  // 迁移版本号位数：9位

// 操作结果类型状态
enum class OperationResults : int {
    Failed = -1,   // 操作失败
    Success = 0,   // 操作成功
    NotFound = -2, // 未找到（删除操作）
};

// 键值对类：存储全局键和值
// 大小: 16字节; 内存布局:
// ┌─────────┬───────────┐
// │   key   │   value   │
// │  8字节  │   8字节   │
// └─────────┴───────────┘
class WRERTKeyValue {
public:
    uint64_t key = 0;
    uint64_t value = -1;

public:
    WRERTKeyValue() = default;

    WRERTKeyValue(uint64_t key, uint64_t value) : key(key), value(value) {}

    WRERTKeyValue& operator=(const WRERTKeyValue& other) {
        if (this != &other) {
            key = other.key;
            value = other.value;
        }
        return *this;
    }

    bool operator==(const WRERTKeyValue& other) const {
        return key == other.key && value == other.value;
    }
};

// 桶内键值对类：用于桶内存储子键和值
// 大小: 16字节; 内存布局:
// ┌─────────┬───────────┐
// │ subkey  │   value   │
// │  8字节  │   8字节   │
// └─────────┴───────────┘
class WRERTSlotKeyValue {
public:
    // subKey字段的位域定义
    struct SubKeyFields {
        uint64_t subKey : 32;   // 32位子键（用于存储子键）
        int64_t migVersion : 9; // 9位迁移版本号
        uint64_t validFlag : 1; // 1位有效位标记（0=无效, 1=有效）
        uint64_t nodeFlag : 1;  // 1位节点标记（0=键值对, 1=节点指针）
        // uint64_t fingerprint : 16; // 16位指纹（用于快速比较）
        uint64_t reserved : 21; // 21位保留位（凑足64位）
    };

    // value字段的位域定义
    struct ValueFields {
        uint64_t value : 48;        // 48位值或指针
        uint64_t shareForeseer : 6; // 未来共享位
        uint64_t reserved : 10;     // 10位保留位
    };

    union {
        uint64_t subKey;
        SubKeyFields subKeyFields;
    };

    union {
        uint64_t value;
        ValueFields valueFields;
    };

public:
    WRERTSlotKeyValue() = default;

    WRERTSlotKeyValue(uint64_t subKey, uint64_t value) : subKey(subKey), value(value) {}
};

inline uint64_t calcBucketPosition(const uint64_t offset, const uint64_t index) {
    // return ((offset + WRERT_SHARED_BUCKETS_PER_SEGMENT) % WRERT_SEGMENT_SIZE + index) % WRERT_SEGMENT_SIZE;
    if (offset == WRERT_SHARED_BUCKETS_OFFSET) {
        return WRERT_SHARED_BUCKETS_PER_SEGMENT + index;
    } else if (offset == WRERT_MAIN_BUCKETS_PER_SEGMENT) {
        return index;
    } else {
        return index < offset ? index + WRERT_MAIN_BUCKETS_PER_SEGMENT : index & WRERT_SHARED_BUCKETS_PER_SEGMENT_MASK;
    }
}

inline bool isInvalidSharedSlot(const WRERTSlotKeyValue* slot, const uint8_t version) {
    return (slot->subKeyFields.validFlag == 0 || (version != WRERT_INIT_LOCAL_DEPTH && slot->subKeyFields.migVersion != version - 1));
}

inline bool isInvalidMainSlot(const WRERTSlotKeyValue* slot, const uint8_t version) {
    return (slot->subKeyFields.validFlag == 0 || (version != WRERT_INIT_LOCAL_DEPTH && slot->subKeyFields.migVersion != version));
}

// 桶类：存储固定数量的键值对，支持范围查询优化
// 大小: 272字节; 内存布局:
// ┌─────────────────────────────────────────────────────────┐
// │ counters[0-15]: {subkey, value} (16×16字节=256字节)     │
// ├─────────────────────────────────────────────────────────┤
// │ sharedBucket指针 (8字节)                                │
// └─────────────────────────────────────────────────────────┘
class WRERTBucket {
public:
    WRERTSlotKeyValue counters[WRERT_SLOTS_PER_BUCKET]; // 16个slot，每个16字节

public:
    WRERTBucket();

    ~WRERTBucket();

    bool isFull(uint64_t segmentMSB, uint8_t localDepth);

    uint64_t getValue(uint64_t key, bool& keyValueFlag);

    int findPutPlace(uint64_t key, uint64_t keyLength, uint64_t localDepth);
};

// 段类：包含桶指针数组
// 大小: 24,576字节; 内存布局:
// ┌─────────────────────────────────────────────────────────┐
// │ buckets[0-95]: WRERTBucket (96×256字节=24,576字节)     │
// └─────────────────────────────────────────────────────────┘
class WRERTSegment {
public:
    WRERTBucket buckets[WRERT_SEGMENT_SIZE]; // 桶指针数组

public:
    WRERTSegment();  // 构造函数
    ~WRERTSegment(); // 析构函数

    void initialize(); // 初始化段
};

// 段节点类：包含桶指针、局部深度和锁状态信息
// 大小: 8字节; 内存布局:
// ┌─────────────────────────────────────────────────────────┐
// │ bucketPtr (48位) + localDepth (8位) + sharedBucketOffset│
// │ (6位) + lockState (2位) + sharedSegment指针 (8字节)     │
// └─────────────────────────────────────────────────────────┘
class WRERTSegmentNode {
public:
    uint8_t localDepth : 8;         // 局部深度（8位）
    uint8_t sharedBucketOffset : 7; // 第一个共享桶的偏移（6位，-1到63）
    uint8_t lockState : 1;          // 锁状态（2位：简单高效控制）
    uint64_t segmentPtr : 48;       // 段指针（48位）

    enum class LockState : uint8_t {
        UNLOCKED = 0,    // 无锁状态
        READ_LOCKED = 1, // 读锁定（允许多个读）
        WRITE_LOCKED = 2 // 写锁定（独占）
    };

public:
    WRERTSegmentNode();
    ~WRERTSegmentNode();

    // 简单高效的锁接口
    bool tryReadLock();
    bool tryWriteLock();
    void readUnlock();
    void writeUnlock();

    // 快速路径检查
    bool isReadLocked() const {
        return lockState == static_cast<uint8_t>(LockState::READ_LOCKED);
    }

    bool isWriteLocked() const {
        return lockState == static_cast<uint8_t>(LockState::WRITE_LOCKED);
    }

    bool isUnlocked() const {
        return lockState == static_cast<uint8_t>(LockState::UNLOCKED);
    }

private:
    // 内部原子操作
    bool casLockState(uint8_t expected, uint8_t desired);
};

// 头部信息类：存储节点的前缀信息
// 大小: 8字节; 内存布局:
// ┌─────────┬─────────┬──────────────┐
// │ nodeDepth│prefixLen│ prefixArray[6]│
// │   1B    │   1B    │      6B      │
// └─────────┴─────────┴──────────────┘
class WRERTHeader {
public:
    unsigned char nodeHeaderDepth;
    unsigned char prefixLength;
    unsigned char prefixArray[6];

public:
    void initialize(const WRERTHeader* oldHeader, unsigned char prefixLength, unsigned char nodeHeaderDepth);

    int computePrefix(uint64_t key, int startPosition, uint32_t* arrayMatchLength);

    void assignPrefix(uint64_t key, int startPosition);

    void assignPrefix(const unsigned char* key, unsigned char assignedLength = WRERT_NODE_PREFIX_MAX_BYTES);

    int computeCommonPrefix(uint64_t key1, uint64_t key2, int startPosition);
};

// 目录节点类：Directory Node结构
// 大小: 24字节 + 指针数组内存; 内存布局:
// ┌─────────────────────────────────────────┐
// │ WRERTHeader header  (8B)               │
// ├─────────────────────────────────────────┤
// │ WRERTDirectory directory (8B)          │
// ├─────────────────────────────────────────┤
// │ capacity (8B) + WRERTSegment指针 (8B)  │
// └─────────────────────────────────────────┘
// 指针数组大小: sizeof(WRERTSegmentNode*) × (1 << globalDepth)
class WRERTDirectoryNode {
public:
    WRERTHeader header;
    uint64_t capacity;

    // enum class WRERTNodeType : uint8_t {
    //     WRERT_NODE_TYPE_INTERNAL = 0,
    //     WRERT_NODE_TYPE_LEAF = 1,
    // };

    // Directory结构：包含全局段指针、深度和标志位
    // 大小: 8字节; 内存布局:
    // ┌─────────────────────────────────────────────────┐
    // │ globalSegmentPtr (48bit) + globalDepth (8bit)   │
    // │ nodeType (2bit) + isLocked (1bit) + reserved (5bit) │
    // └─────────────────────────────────────────────────┘
    struct WRERTDirectory {
        uint8_t globalDepth : 8;
        // uint8_t nodeType : 2;
        uint8_t isLocked : 8;
        // uint8_t reserved : 5;
        uint64_t globalSegmentPtr : 48;

        WRERTDirectory() {}

        WRERTDirectory(const WRERTDirectory* _directory, uint8_t newGlobalDepth) : globalDepth(newGlobalDepth),
                                                                                   //    nodeType(_directory->nodeType),
                                                                                   isLocked(_directory->isLocked),
                                                                                   //    reserved(_directory->reserved),
                                                                                   globalSegmentPtr(_directory->globalSegmentPtr) {}
    } directory;

public:
    WRERTDirectoryNode();
    ~WRERTDirectoryNode();

    void initialize(uint8_t globalDepth);
};

// 主节点类：包含目录节点，提供键值对操作接口
class WRERTNode {
private:
    // 添加缓冲池声明
    static thread_local WRERTBucket migration_buffer_pool[WRERT_MIG_SHARED_BUCKET_NUM];
    // static thread_local WRERTBucket migration_buffer_pool[WRERT_SHARED_BUCKETS_PER_SEGMENT];
public:
    WRERTDirectoryNode directoryNode;

public:
    WRERTNode();
    ~WRERTNode();

    void initialize(unsigned char nodeHeaderDepth = WRERT_INIT_NODE_DEPTH, unsigned char globalDepth = WRERT_INIT_GLOBAL_DEPTH);

    OperationResults putDirNode(WRERTKeyValue* globalSegment, uint64_t key, uint64_t value);

    uint64_t getDirNode(WRERTKeyValue* globalSegment, uint64_t key);

    OperationResults putSegNode(uint64_t subKey, uint64_t value, uint16_t _fingerprint, uint64_t* beforeAddress, uint8_t _nodeFlag);

    WRERTSlotKeyValue* getSegNode(uint64_t subKey);

    OperationResults put(uint64_t subKey, uint64_t value, WRERTSegmentNode* segmentNode, WRERTSegment* segment, WRERTSlotKeyValue* slot,
                         uint64_t segmentIndex, uint64_t _sharedBucketOffset, uint16_t _fingerprint, int8_t _isShareOrMainFlag, uint8_t _nodeFlag, uint64_t* beforeAddress);

    // 计算键的指纹信息
    uint64_t fingerprint(uint64_t key);

    uint64_t stale_fingerprint(uint64_t key, uint8_t depth);
};

// 创建函数声明
WRERTKeyValue* NewWRERTKeyValue(uint64_t key, uint64_t value);

WRERTBucket* NewWRERTBucket();

WRERTSegment* NewWRERTSegment();

WRERTNode* NewWRERTNode(unsigned char nodeHeaderDepth = WRERT_INIT_NODE_DEPTH, unsigned char globalDepth = WRERT_INIT_GLOBAL_DEPTH);

extern std::vector<uint64_t> wrert_insert_segment_split_time_;      // 插入分段时间
extern std::vector<uint64_t> wrert_insert_directory_grow_time_;     // 插入目录扩展时间
extern std::vector<uint64_t> wrert_per_insert_node_update_latency_; // 每次插入时更新节点的延迟

} // namespace wrert

#endif // NVMKV_WRERT_NODE_H
