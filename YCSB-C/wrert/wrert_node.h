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

#define WRERT_GET_SEGMENT_NUMBER(key, keyLength, globalDepth) \
    ((key >> (keyLength - globalDepth)) & (((uint64_t)1 << globalDepth) - 1))

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

#define WRERT_GET_PREFIX_ARRAY_BITS(prefixArray, startPosition, subKeyLength) \
    ({                                                                        \
        uint64_t result = 0;                                                  \
                                                                              \
        for (uint64_t i = 0; i < subKeyLength; i++) {                         \
            result = (result << 8) | prefixArray[startPosition + i];          \
        }                                                                     \
        result;                                                               \
    })

#define WRERT_GET_SHARED_BUCKET_INDEX(subKey, localDepth, indexBits) \
    ((subKey >> (WRERT_NODE_SUBKEY_LENGTH - (localDepth - 1 + indexBits))) & (((uint64_t)1 << indexBits) - 1))

#define WRERT_GET_MAIN_BUCKET_INDEX(subKey, localDepth, indexBits) \
    ((subKey >> (WRERT_NODE_SUBKEY_LENGTH - (localDepth + indexBits))) & (((uint64_t)1 << indexBits) - 1))

#define WRERT_GET_ADJUSTED_MAIN_BUCKET_INDEX(subKey, localDepth, indexBits) \
    ((subKey >> (WRERT_NODE_SUBKEY_LENGTH - (localDepth + indexBits))) & (((uint64_t)1 << indexBits) - 1))

#define WRERT_IS_SHARED_BUCKET_FULL(bucket) \
    (bucket.counters[WRERT_SLOTS_PER_BUCKET - 1].subKeyFields.validFlag == 1)

constexpr uint8_t WRERT_SIZE_OF_CHAR = 8;
constexpr uint8_t WRERT_KEY_LENGTH = 64;
constexpr uint8_t WRERT_KEY_MAX_BYTES = WRERT_KEY_LENGTH / WRERT_SIZE_OF_CHAR;
constexpr uint8_t WRERT_NODE_SUBKEY_LENGTH = 32;
constexpr uint8_t WRERT_NODE_SPAN = WRERT_NODE_SUBKEY_LENGTH;
constexpr uint8_t WRERT_NODE_SUBKEY_MAX_BYTES = WRERT_NODE_SPAN / WRERT_SIZE_OF_CHAR;
constexpr uint8_t WRERT_NODE_PREFIX_MAX_BYTES = 6;
constexpr uint8_t WRERT_NODE_PREFIX_MAX_BITS = 48;
constexpr uint8_t WRERT_NODE_PREFIX_MAX_LENGTH = WRERT_KEY_LENGTH / WRERT_NODE_SUBKEY_LENGTH;
constexpr uint8_t WRERT_INIT_PREFIX_LENGTH = WRERT_NODE_PREFIX_MAX_BYTES;
constexpr uint8_t WRERT_INIT_NODE_DEPTH = 0;
constexpr uint8_t WRERT_INIT_GLOBAL_DEPTH = 0;
constexpr uint8_t WRERT_INIT_LOCAL_DEPTH = 0;

constexpr uint8_t WRERT_MAIN_BUCKETS_PER_SEGMENT = 32;
constexpr uint8_t WRERT_MAIN_BUCKETS_PER_SEGMENT_MASK = WRERT_MAIN_BUCKETS_PER_SEGMENT - 1;
constexpr uint8_t WRERT_MAIN_BUCKETS_MAX_BITS = __builtin_ctz(WRERT_MAIN_BUCKETS_PER_SEGMENT);
constexpr uint8_t WRERT_SHARED_BUCKETS_PER_SEGMENT = WRERT_MAIN_BUCKETS_PER_SEGMENT / 2;
constexpr uint8_t WRERT_SHARED_BUCKETS_PER_SEGMENT_MASK = WRERT_SHARED_BUCKETS_PER_SEGMENT - 1;
constexpr uint8_t WRERT_SHARED_BUCKETS_OFFSET = 0;
constexpr uint8_t WRERT_SHARED_BUCKETS_MAX_BITS = __builtin_ctz(WRERT_SHARED_BUCKETS_PER_SEGMENT);
constexpr uint8_t WRERT_SLOTS_PER_BUCKET = 16;
constexpr uint8_t WRERT_SLOT_SIZE = 16;
constexpr uint8_t WRERT_SEGMENT_SIZE = WRERT_MAIN_BUCKETS_PER_SEGMENT + WRERT_SHARED_BUCKETS_PER_SEGMENT;
constexpr uint16_t WRERT_BUCKET_SIZE = WRERT_SLOTS_PER_BUCKET;

constexpr uint8_t WRERT_MIG_SHARED_BUCKET_NUM = WRERT_SHARED_BUCKETS_PER_SEGMENT / 2;
constexpr uint8_t WRERT_MIG_MAIN_BUCKET_NUM = WRERT_MAIN_BUCKETS_PER_SEGMENT / 2;

constexpr uint8_t WRERT_BUCKET_INDEX_BIT_NUM = WRERT_MAIN_BUCKETS_MAX_BITS;
constexpr uint8_t WRERT_FINGERPRINT_BITS = 16;
constexpr uint8_t WRERT_FINGERPRINT_BIT_ALIGNMENT = 8ul;
constexpr uint8_t WRERT_SPLIT_THRESHOLD = 48;

constexpr uint32_t WRERT_BUCKET_MEMORY_SIZE = WRERT_SLOT_SIZE * WRERT_SLOTS_PER_BUCKET;
constexpr uint32_t WRERT_SEGMENT_MEMORY_SIZE = WRERT_SEGMENT_SIZE * WRERT_BUCKET_MEMORY_SIZE;

alignas(64) constexpr uint64_t offsets[8] = {0, 16, 32, 48, 64, 80, 96, 112};
constexpr __mmask8 summary_mask = 0xFF;
constexpr uint64_t WRERT_VALIDFLAG_MIGVERSION_MASK = 0x000003FF00000000ULL;
constexpr uint64_t WRERT_MIGVERSION_MASK = 0x000000FF00000000ULL;
constexpr uint64_t WRERT_SUBKEY_MASK = 0x00000000FFFFFFFFULL;
constexpr uint64_t WRERT_VALIDFLAG_MIGVERSION_SUBKEY_MASK = 0x000003FFFFFFFFFFULL;
constexpr uint64_t WRERT_MIGVERSION_BITS = 0x1FF;
constexpr uint64_t WRERT_VALUE_PTR_MASK = 0x0000FFFFFFFFFFFFULL;

enum class OperationResults : int {
    Failed = -1,   // 操作失败
    Success = 0,   // 操作成功
    NotFound = -2, // 未找到（删除操作）
};

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

class WRERTSlotKeyValue {
public:
    struct SubKeyFields {
        uint64_t subKey : 32;
        int64_t migVersion : 9;
        uint64_t validFlag : 1;
        uint64_t nodeFlag : 1;
        uint64_t fingerprint : 16;
        uint64_t reserved : 5;
    };

    struct ValueFields {
        uint64_t value : 48;
        int64_t migVersion : 16;
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

class WRERTBucket {
public:
    WRERTSlotKeyValue counters[WRERT_SLOTS_PER_BUCKET];

public:
    WRERTBucket();
    ~WRERTBucket();

    bool isFull(uint64_t segmentMSB, uint8_t localDepth);
};

class WRERTSegment {
public:
    WRERTBucket buckets[WRERT_SEGMENT_SIZE]; // 桶指针数组

public:
    WRERTSegment();
    ~WRERTSegment();

    void initialize();
};

class WRERTSegmentNode {
public:
    uint8_t localDepth : 8;
    uint8_t sharedBucketOffset : 8;
    uint64_t segmentPtr : 48;

public:
    WRERTSegmentNode();
    ~WRERTSegmentNode();
};

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

class WRERTDirectoryNode {
public:
    WRERTHeader header;
    uint64_t capacity;

    struct WRERTDirectory {
        uint8_t globalDepth : 8;
        uint8_t isLocked : 8;
        uint64_t globalSegmentPtr : 48;

        WRERTDirectory() {}
        WRERTDirectory(const WRERTDirectory* _directory, uint8_t newGlobalDepth) : globalDepth(newGlobalDepth),
                                                                                   isLocked(_directory->isLocked),
                                                                                   globalSegmentPtr(_directory->globalSegmentPtr) {}
    } directory;

public:
    WRERTDirectoryNode();
    ~WRERTDirectoryNode();

    void initialize(uint8_t globalDepth);
};

class WRERTNode {
private:
    static thread_local WRERTBucket migration_buffer_pool[WRERT_MIG_SHARED_BUCKET_NUM];

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

    uint64_t fingerprint(uint64_t key);

    uint64_t stale_fingerprint(uint64_t key, uint8_t depth);
};

WRERTKeyValue* NewWRERTKeyValue(uint64_t key, uint64_t value);

WRERTBucket* NewWRERTBucket();

WRERTSegment* NewWRERTSegment();

WRERTNode* NewWRERTNode(unsigned char nodeHeaderDepth = WRERT_INIT_NODE_DEPTH, unsigned char globalDepth = WRERT_INIT_GLOBAL_DEPTH);

} // namespace wrert

#endif // NVMKV_WRERT_NODE_H
