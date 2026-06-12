#ifndef NVMKV_ERT_NODE_INT_H
#define NVMKV_ERT_NODE_INT_H

// 头文件包含
#include <cstdint>                  // 标准整数类型
#include <sys/time.h>               // 时间相关函数
#include <vector>                   // 向量容器
#include <chrono>                   // 时间相关函数
#include <map>                      // 映射容器
#include <math.h>                   // 数学函数
#include <cstdint>                  // 标准整数类型（重复包含，但无害）
#include "../fastalloc/fastalloc.h" // 快速内存分配器

// 分支预测宏，用于优化条件判断
#define likely(x) (__builtin_expect(!!(x), 1))   // 很可能为真
#define unlikely(x) (__builtin_expect(!!(x), 0)) // 很可能为假

// 段号计算宏：根据key、key长度和深度计算段号
#define GET_SEG_NUM(key, key_len, depth) ((key >> (key_len - depth)) & (((uint64_t)1 << depth) - 1))
// 桶号计算宏：根据key和桶掩码长度计算桶号
#define GET_BUCKET_NUM(key, bucket_mask_len) ((key) & (((uint64_t)1 << bucket_mask_len) - 1))

// 段位置计算宏：计算指定目录索引处的段指针位置
#define GET_SEG_POS(currentNode, dir_index) (((uint64_t)(currentNode) + sizeof(ERTIntNode) + dir_index * sizeof(ERTIntNode*)))

// 子键提取宏：从key中提取指定位置和长度的子键
#define GET_SUBKEY(key, start, length) ((key >> (64 - start - length) & (((uint64_t)1 << length) - 1)))

// 节点标志操作宏
#define REMOVE_NODE_FLAG(key) (key & (((uint64_t)1 << 56) - 1)) // 移除节点标志位
#define PUT_KEY_VALUE_FLAG(key) (key | ((uint64_t)1 << 56))     // 设置键值对标志位
#define GET_NODE_FLAG(key) (key >> 56)                          // 获取节点标志位

// 可扩展基数树常量定义
#define ERT_INIT_GLOBAL_DEPTH 0                       // 初始全局深度
#define ERT_BUCKET_SIZE 4                             // 桶大小（每个桶最多存储4个键值对）
#define ERT_BUCKET_MASK_LEN 8                         // 桶掩码长度（8位，支持256个桶）（2、4、6、8、10、12）（4、5、6、8、9、10）
#define ERT_MAX_BUCKET_NUM (1 << ERT_BUCKET_MASK_LEN) // 最大桶数（256）

#define SIZE_OF_CHAR 8              // 字符大小（8位）
#define ERT_NODE_LENGTH 32          // 节点长度
#define ERT_NODE_PREFIX_MAX_BYTES 6 // 节点前缀最大字节数
#define ERT_NODE_PREFIX_MAX_BITS 48 // 节点前缀最大位数（6字节）
#define ERT_KEY_LENGTH 64           // 键长度（64位）

// 键值对类：存储键和值
// 大小: 16字节; 内存布局:
// ┌─────────┬───────────┐
// │   key   │   value   │
// └─────────┴───────────┘
class ERTIntKeyValue {
public:
    uint64_t key = 0;   // 键
    uint64_t value = 0; // 值

    // 赋值操作符重载
    void operator=(ERTIntKeyValue a) {
        this->key = a.key;
        this->value = a.value;
    };
};

// 创建新键值对的函数声明
ERTIntKeyValue* NewERTIntKeyValue(uint64_t key, uint64_t value);

// 桶内键值对类：用于桶内存储
// 大小: 16字节; 内存布局:
// ┌─────────┬───────────┐
// │   key   │   value   │
// └─────────┴───────────┘
class ERTIntBucketKeyValue {
public:
    uint64_t subkey = 0; // 子键
    uint64_t value = 0;  // 值
};

// 桶类：存储固定数量的键值对
// 大小: 64字节; 内存布局:
// ┌──────────────────────────────────┐
// │ counter[0]: {subkey=0, value=0}  │
// │ counter[1]: {subkey=0, value=0}  │
// │ counter[2]: {subkey=0, value=0}  │
// │ counter[3]: {subkey=0, value=0}  │
// └──────────────────────────────────┘
class ERTIntBucket {
public:
    ERTIntBucketKeyValue counter[ERT_BUCKET_SIZE]; // 桶内键值对数组

    // 根据键获取值
    uint64_t get(uint64_t key, bool& keyValueFlag);

    // 查找键的插入位置
    int findPlace(uint64_t _key, uint64_t _key_len, uint64_t _depth);
};

// 段类：包含深度信息和桶指针数组
// 大小: 16字节（结构体本身）+ 桶数组内存; 内存布局:
// ┌───────────┬─────────────────┐
// │   depth   │   bucket 指针   │
// └───────────┴─────────────────┘
// 桶数组大小（每个桶64字节，256个桶）: sizeof(ERTIntBucket) × 256 = 64 × 256 = 16,384字节（16KB）
class ERTIntSegment {
public:
    uint64_t depth = 0;   // 段深度
    ERTIntBucket* bucket; // 桶指针数组
                          //    ERTIntBucket bucket[ERT_MAX_BUCKET_NUM];  // 注释掉的固定大小桶数组

    ERTIntSegment();  // 构造函数
    ~ERTIntSegment(); // 析构函数

    void init(uint64_t _depth); // 初始化段
};

// 创建新段的函数声明
ERTIntSegment* NewERTIntSegment(uint64_t _depth = 0);

// 头部信息类：存储节点的前缀信息
// 大小: 8B; 内存布局:
// ┌─────────┬─────────┬──────────────┐
// │   len   │  depth  │   array[6]   │
// │   1B    │   1B    │      6B      │
// └─────────┴─────────┴──────────────┘
class ERTIntHeader {
public:
    unsigned char len = 7;  // 前缀长度
    unsigned char depth;    // 深度
    unsigned char array[6]; // 前缀字节数组

    // 初始化头部信息
    void init(ERTIntHeader* oldHeader, unsigned char length, unsigned char depth);

    // 计算键的前缀
    int computePrefix(uint64_t key, int pos);

    // 从键中分配前缀
    void assign(uint64_t key, int startPos);

    // 从字节数组中分配前缀
    void assign(unsigned char* key, unsigned char assignedLength = ERT_NODE_PREFIX_MAX_BYTES);
};

// 节点类：可扩展基数树的核心节点结构
// 大小: 24字节; 内存布局:
// ┌─────────────────────────────────────────┐
// │ ERTIntHeader header  (8B)               │
// ├─────────────────────────────────────────┤
// │ global_depth (1B) + 填充 (3B)           │
// ├─────────────────────────────────────────┤
// │ dir_size (4B)                           │
// ├─────────────────────────────────────────┤
// │ treeNodeValues 指针 (8B)                │
// └─────────────────────────────────────────┘
// 节点值数组大小: sizeof(ERTIntKeyValue) × (1 + 48/32) = 16 × 2 = 32字节
class ERTIntNode {
public:
    ERTIntHeader header;            // 头部信息
    unsigned char global_depth = 0; // 全局深度
    uint32_t dir_size = 1;          // 目录大小
    ERTIntKeyValue* treeNodeValues; // 节点值数组

    ERTIntNode();  // 构造函数
    ~ERTIntNode(); // 析构函数

    // 初始化节点
    void init(unsigned char headerDepth = 0, unsigned char global_depth = 0);

    // 插入键值对（简单版本）
    void put(uint64_t subkey, uint64_t value, uint64_t kvFLAG, uint64_t beforeAddress);

    // 插入键值对（完整版本，包含段和桶信息）
    void put(uint64_t subkey, uint64_t value, ERTIntSegment* tmp_seg, ERTIntBucket* tmp_bucket,
             uint64_t dir_index, uint64_t seg_index, uint64_t kvFLAG, uint64_t beforeAddress);

    // 在指定位置插入键值对
    void nodePut(int pos, ERTIntKeyValue* kv);

    // 根据子键获取值
    uint64_t get(uint64_t subkey, bool& keyValueFlag);
};

// 创建新节点的函数声明
ERTIntNode* NewERTIntNode(int _key_len, unsigned char headerDepth = 0,
                          unsigned char globalDepth = ERT_INIT_GLOBAL_DEPTH);

extern std::vector<uint64_t> ert_insert_segment_split_time_;      // 插入分段时间
extern std::vector<uint64_t> ert_insert_directory_grow_time_;     // 插入目录扩展时间
extern std::vector<uint64_t> ert_per_insert_node_update_latency_; // 每次插入时更新节点的延迟

#endif // NVMKV_ERT_NODE_INT_H