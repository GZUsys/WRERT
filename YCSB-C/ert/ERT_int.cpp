#include <fstream>
#include <iomanip>
#include <sstream>
#include <chrono>
#include <algorithm>
#include <vector>

#include "ERT_int.h"

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

void ERTInt::Insert(uint64_t key, uint64_t value, ERTIntNode* _node, int keyLength) {
restart:
    ERTIntNode* currentNode = _node ? _node : root;
    locks().directory(currentNode).read_lock();

    if (_node == nullptr && currentNode != root) {
        locks().directory(currentNode).read_unlock();
        goto restart;
    }

    unsigned char headerDepth = currentNode->header.depth;
    uint64_t beforeAddress = (uint64_t)&root;
    uint8_t kvFLAG;
    uint32_t len = keyLength;
    uint32_t j = 0;

    while (len < ERT_KEY_LENGTH / SIZE_OF_CHAR) {
        int matchedPrefixLen;

        if (currentNode->header.len > ERT_NODE_PREFIX_MAX_BYTES) {
            locks().directory(currentNode).read_unlock();
            locks().directory(currentNode).write_lock();
            if (!(currentNode->header.len > ERT_NODE_PREFIX_MAX_BYTES)) {
                locks().directory(currentNode).write_unlock();
                goto restart;
            }

            int size = ERT_NODE_PREFIX_MAX_BITS / ERT_NODE_LENGTH * ERT_NODE_LENGTH;
            if (len == 0) {
                currentNode->header.len =
                  (ERT_KEY_LENGTH - len * SIZE_OF_CHAR) <= size ? (ERT_KEY_LENGTH - len * SIZE_OF_CHAR) / SIZE_OF_CHAR : size / SIZE_OF_CHAR;
            } else {
                currentNode->header.len = 0;
            }
            currentNode->header.assign(key, len);

            locks().directory(currentNode).write_unlock();
            locks().directory(currentNode).read_lock();
            continue;
        } else {
            matchedPrefixLen = currentNode->header.computePrefix(key, len * SIZE_OF_CHAR);
        }

        if (len + matchedPrefixLen == (ERT_KEY_LENGTH / SIZE_OF_CHAR)) {
            locks().directory(currentNode).read_unlock();
            locks().directory(currentNode).write_lock();
            ERTIntKeyValue* kv = NewERTIntKeyValue(key, value);
            clflush((char*)kv, sizeof(ERTIntKeyValue));
            currentNode->nodePut(matchedPrefixLen, kv);
            locks().directory(currentNode).write_unlock();
            return;
        }

        if ((matchedPrefixLen / (ERT_NODE_LENGTH / SIZE_OF_CHAR))
            == (currentNode->header.len / (ERT_NODE_LENGTH / SIZE_OF_CHAR))) {
            len += currentNode->header.len % (ERT_NODE_LENGTH / SIZE_OF_CHAR) == 0 ? currentNode->header.len : currentNode->header.len / (ERT_NODE_LENGTH / SIZE_OF_CHAR);
            uint64_t subkey = GET_SUBKEY(key, len * SIZE_OF_CHAR, ERT_NODE_LENGTH);
            uint64_t next = 0;
            uint64_t dir_index = GET_SEG_NUM(subkey, ERT_NODE_LENGTH, currentNode->global_depth);
            ERTIntSegment* tmp_seg = *(ERTIntSegment**)GET_SEG_POS(currentNode, dir_index);

            locks().segment(tmp_seg).read_lock();

            uint64_t depth = tmp_seg->depth;
            uint64_t seg_index = GET_BUCKET_NUM(subkey, ERT_BUCKET_MASK_LEN);
            ERTIntBucket* tmp_bucket = &(tmp_seg->bucket[seg_index]);

            locks().bucket(tmp_bucket).read_lock();

            int i, bucket_index = -1;

            bool keyValueFlag = false, needSMO = false;
            uint64_t beforeA;

            for (i = 0; i < ERT_BUCKET_SIZE; ++i) {
                if (subkey == REMOVE_NODE_FLAG(tmp_bucket->counter[i].subkey)) {
                    next = tmp_bucket->counter[i].value;
                    keyValueFlag = GET_NODE_FLAG(tmp_bucket->counter[i].subkey);
                    beforeA = (uint64_t)&tmp_bucket->counter[i].value;
                    break;
                }
            }

            len += ERT_NODE_LENGTH / SIZE_OF_CHAR;

            if (len == 8) {
                if (next == 0) {
                    // printf("情况2.1.1: 到达键的末尾 - 子键不存在 - 直接插入\n");
                    bucket_index = tmp_bucket->findPlace(subkey, ERT_NODE_LENGTH, depth);
                    if (bucket_index != -1) {
                        locks().bucket(tmp_bucket).read_unlock();
                        locks().bucket(tmp_bucket).write_lock();
                        needSMO = false;
                        if ((REMOVE_NODE_FLAG(tmp_bucket->counter[bucket_index].subkey) != 0
                             || tmp_bucket->counter[bucket_index].value != 0)
                            && REMOVE_NODE_FLAG(tmp_bucket->counter[bucket_index].subkey) != subkey
                            && (GET_SEG_NUM(subkey, ERT_NODE_LENGTH, depth) == GET_SEG_NUM(REMOVE_NODE_FLAG(tmp_bucket->counter[bucket_index].subkey), ERT_NODE_LENGTH, depth))) {
                            locks().bucket(tmp_bucket).write_unlock();
                            locks().segment(tmp_seg).read_unlock();
                            locks().directory(currentNode).read_unlock();
                            // printf("情况2.1.1: 直接插入重试\n");
                            goto restart;
                        }
                    } else {
                        locks().bucket(tmp_bucket).read_unlock();
                        locks().segment(tmp_seg).read_unlock();
                        locks().directory(currentNode).read_unlock();
                        locks().directory(currentNode).write_lock();
                        locks().segment(tmp_seg).write_lock();
                        needSMO = true;
                        if (depth != tmp_seg->depth
                                || headerDepth != currentNode->header.depth
                                || headerDepth != 0 ?
                              currentNode != (ERTIntNode*)((ERTIntBucketKeyValue*)beforeAddress)->value :
                              currentNode != *(ERTIntNode**)beforeAddress) {
                            locks().segment(tmp_seg).write_unlock();
                            locks().directory(currentNode).write_unlock();
                            // printf("情况2.1.1: SMO重试\n");
                            goto restart;
                        }
                    }

                    currentNode->put(subkey, (uint64_t)value, tmp_seg, tmp_bucket, dir_index, seg_index, bucket_index, kvFLAG = 1, beforeAddress);

                    if (needSMO) {
                        locks().segment(tmp_seg).write_unlock();
                        locks().directory(currentNode).write_unlock();
                    } else {
                        locks().bucket(tmp_bucket).write_unlock();
                        locks().segment(tmp_seg).read_unlock();
                        locks().directory(currentNode).read_unlock();
                    }

                    return;
                } else {
                    // printf("情况2.1.2: 到达键的末尾 - 子键已存在 - 更新值\n");
                    locks().bucket(tmp_bucket).read_unlock();
                    locks().bucket(tmp_bucket).write_lock();
                    if ((REMOVE_NODE_FLAG(tmp_bucket->counter[i].subkey) != 0
                         || tmp_bucket->counter[i].value != 0)
                        && tmp_bucket->counter[i].value != next
                        && (GET_SEG_NUM(subkey, ERT_NODE_LENGTH, depth) == GET_SEG_NUM(REMOVE_NODE_FLAG(tmp_bucket->counter[i].subkey), ERT_NODE_LENGTH, depth))) {
                        locks().bucket(tmp_bucket).write_unlock();
                        locks().segment(tmp_seg).read_unlock();
                        locks().directory(currentNode).read_unlock();
                        goto restart;
                    }

                    tmp_bucket->counter[i].value = value;
                    clflush((char*)&tmp_bucket->counter[i].value, 8);

                    locks().bucket(tmp_bucket).write_unlock();
                    locks().segment(tmp_seg).read_unlock();
                    locks().directory(currentNode).read_unlock();
                    return;
                }
            } else {
                if (next == 0) {
                    // printf("情况2.2.1: 未到达键的末尾 - 子键不存在 - 创建新的键值对\n");
                    bucket_index = tmp_bucket->findPlace(subkey, ERT_NODE_LENGTH, depth);
                    if (bucket_index != -1) {
                        locks().bucket(tmp_bucket).read_unlock();
                        locks().bucket(tmp_bucket).write_lock();
                        needSMO = false;
                        if ((REMOVE_NODE_FLAG(tmp_bucket->counter[bucket_index].subkey) != 0
                             || tmp_bucket->counter[bucket_index].value != 0)
                            && REMOVE_NODE_FLAG(tmp_bucket->counter[bucket_index].subkey) != subkey
                            && (GET_SEG_NUM(subkey, ERT_NODE_LENGTH, depth) == GET_SEG_NUM(REMOVE_NODE_FLAG(tmp_bucket->counter[bucket_index].subkey), ERT_NODE_LENGTH, depth))) {
                            locks().bucket(tmp_bucket).write_unlock();
                            locks().segment(tmp_seg).read_unlock();
                            locks().directory(currentNode).read_unlock();
                            // printf("情况2.2.1: 直接插入重试\n");
                            goto restart;
                        }
                    } else {
                        locks().bucket(tmp_bucket).read_unlock();
                        locks().segment(tmp_seg).read_unlock();
                        locks().directory(currentNode).read_unlock();
                        locks().directory(currentNode).write_lock();
                        locks().segment(tmp_seg).write_lock();
                        needSMO = true;
                        if (depth != tmp_seg->depth
                                || headerDepth != currentNode->header.depth
                                || headerDepth != 0 ?
                              currentNode != (ERTIntNode*)((ERTIntBucketKeyValue*)beforeAddress)->value :
                              currentNode != *(ERTIntNode**)beforeAddress) {
                            locks().segment(tmp_seg).write_unlock();
                            locks().directory(currentNode).write_unlock();
                            // printf("情况2.2.1: SMO重试\n");
                            goto restart;
                        }
                    }

                    ERTIntKeyValue* kv = NewERTIntKeyValue(key, value);
                    clflush((char*)kv, sizeof(ERTIntKeyValue));
                    currentNode->put(subkey, (uint64_t)kv, tmp_seg, tmp_bucket, dir_index, seg_index, bucket_index, kvFLAG = 1, beforeAddress);

                    if (needSMO) {
                        locks().segment(tmp_seg).write_unlock();
                        locks().directory(currentNode).write_unlock();
                    } else {
                        locks().bucket(tmp_bucket).write_unlock();
                        locks().segment(tmp_seg).read_unlock();
                        locks().directory(currentNode).read_unlock();
                    }

                    return;
                } else {
                    if (keyValueFlag) {
                        uint64_t prekey = ((ERTIntKeyValue*)next)->key;
                        uint64_t prevalue = ((ERTIntKeyValue*)next)->value;
                        if (unlikely(key == prekey)) {
                            // printf("情况2.2.2: 未到达键的末尾 - 子键存在 - 找到的是键值对 - 键相同 - 更新键值对\n");
                            locks().bucket(tmp_bucket).read_unlock();
                            locks().bucket(tmp_bucket).write_lock();
                            if ((REMOVE_NODE_FLAG(tmp_bucket->counter[bucket_index].subkey) != 0
                                 || tmp_bucket->counter[bucket_index].value != 0)
                                && tmp_bucket->counter[i].value != next
                                && (GET_SEG_NUM(subkey, ERT_NODE_LENGTH, depth) == GET_SEG_NUM(REMOVE_NODE_FLAG(tmp_bucket->counter[bucket_index].subkey), ERT_NODE_LENGTH, depth))) {
                                locks().bucket(tmp_bucket).write_unlock();
                                locks().segment(tmp_seg).read_unlock();
                                locks().directory(currentNode).read_unlock();
                                goto restart;
                            }

                            ((ERTIntKeyValue*)next)->value = value;
                            clflush((char*)&(((ERTIntKeyValue*)next)->value), 8);

                            locks().bucket(tmp_bucket).write_unlock();
                            locks().segment(tmp_seg).read_unlock();
                            locks().directory(currentNode).read_unlock();
                            return;
                        } else {
                            // printf("情况2.2.3: 未到达键的末尾 - 子键存在 - 找到的是键值对 - 键不同 - 创建新节点处理冲突\n");
                            locks().bucket(tmp_bucket).read_unlock();
                            locks().bucket(tmp_bucket).write_lock();
                            needSMO = true;
                            if ((REMOVE_NODE_FLAG(tmp_bucket->counter[i].subkey) != 0
                                 || tmp_bucket->counter[i].value != 0)
                                && REMOVE_NODE_FLAG(tmp_bucket->counter[i].subkey) != subkey
                                && (GET_SEG_NUM(subkey, ERT_NODE_LENGTH, depth) == GET_SEG_NUM(REMOVE_NODE_FLAG(tmp_bucket->counter[i].subkey), ERT_NODE_LENGTH, depth))) {
                                locks().bucket(tmp_bucket).write_unlock();
                                locks().segment(tmp_seg).read_unlock();
                                locks().directory(currentNode).read_unlock();
                                goto restart;
                            }

                            ERTIntNode* newNode = NewERTIntNode(ERT_NODE_LENGTH, headerDepth + 1);

                            InsertNewNode(prekey, prevalue, newNode, len);
                            InsertNewNode(key, value, newNode, len);

                            clflush((char*)newNode, sizeof(ERTIntNode));

                            tmp_bucket->counter[i].subkey = REMOVE_NODE_FLAG(tmp_bucket->counter[i].subkey);
                            tmp_bucket->counter[i].value = (uint64_t)newNode;

                            clflush((char*)&tmp_bucket->counter[i].value, 8);

                            locks().bucket(tmp_bucket).write_unlock();
                            locks().segment(tmp_seg).read_unlock();
                            locks().directory(currentNode).read_unlock();
                            return;
                        }
                    } else {
                        // printf("情况2.2.4: 未到达键的末尾 - 子键存在 - 找到的是节点指针，继续向下层处理\n");
                        ERTIntNode* subNode = (ERTIntNode*)next;
                        locks().directory(subNode).read_lock();
                        if (subNode != (ERTIntNode*)tmp_bucket->counter[i].value) {
                            locks().directory(subNode).read_unlock();
                            locks().bucket(tmp_bucket).read_unlock();
                            locks().segment(tmp_seg).read_unlock();
                            locks().directory(currentNode).read_unlock();
                            goto restart;
                        }
                        locks().bucket(tmp_bucket).read_unlock();
                        locks().segment(tmp_seg).read_unlock();
                        locks().directory(currentNode).read_unlock();

                        currentNode = subNode;
                        headerDepth = currentNode->header.depth;
                        beforeAddress = beforeA;
                    }
                }
            }
        } else {
            // printf("情况3: 前缀不匹配，需要节点路径压缩的分裂\n");
            locks().directory(currentNode).read_unlock();
            locks().directory(currentNode).write_lock();
            if ((matchedPrefixLen / (ERT_NODE_LENGTH / SIZE_OF_CHAR))
                == (currentNode->header.len / (ERT_NODE_LENGTH / SIZE_OF_CHAR))) {
                locks().directory(currentNode).write_unlock();
                goto restart;
            }

            ERTIntNode* newNode = NewERTIntNode(ERT_NODE_LENGTH, headerDepth);
            newNode->header.init(&currentNode->header, matchedPrefixLen, currentNode->header.depth);
            for (int j = currentNode->header.len - matchedPrefixLen; j <= currentNode->header.len; j++) {
                newNode->treeNodeValues[j - currentNode->header.len + matchedPrefixLen] = currentNode->treeNodeValues[j];
            }

            uint64_t subkey = GET_SUBKEY(key, (len + matchedPrefixLen % (ERT_NODE_LENGTH / SIZE_OF_CHAR) != 0 ? 0 : matchedPrefixLen), ERT_NODE_LENGTH);
            ERTIntKeyValue* kv = NewERTIntKeyValue(key, value);

            clflush((char*)kv, sizeof(ERTIntKeyValue));

            newNode->put(subkey, (uint64_t)kv, kvFLAG = 1, (uint64_t)&newNode);
            newNode->put(GET_32BITS(currentNode->header.array, matchedPrefixLen), (uint64_t)currentNode, kvFLAG = 0, (uint64_t)&newNode);

            currentNode->header.depth =
              newNode->header.depth + newNode->header.len / (ERT_NODE_LENGTH / SIZE_OF_CHAR) + 1; // 更新深度
            currentNode->header.len -= (matchedPrefixLen + newNode->header.depth + 1);
            for (int i = 0; i < ERT_NODE_PREFIX_MAX_BYTES - matchedPrefixLen; i++) {
                currentNode->header.array[i] = currentNode->header.array[i + matchedPrefixLen];
            }

            clflush((char*)&(currentNode->header), 8);
            clflush((char*)newNode, sizeof(ERTIntNode));

            if (newNode->header.depth != 0) {
                ((ERTIntBucketKeyValue*)beforeAddress)->value = (uint64_t)newNode;
                clflush((char*)(ERTIntBucketKeyValue*)beforeAddress, sizeof(ERTIntBucketKeyValue));
            } else {
                *(ERTIntNode**)beforeAddress = newNode;
                clflush((char*)beforeAddress, sizeof(ERTIntNode*));
            }

            locks().directory(currentNode).write_unlock();
            return;
        }
    }

    locks().directory(currentNode).read_unlock();
    return;
}

void ERTInt::InsertNewNode(uint64_t key, uint64_t value, ERTIntNode* _node, int len) {
    ERTIntNode* currentNode = _node;
    if (_node == NULL) {
        currentNode = root;
    }

    unsigned char headerDepth = currentNode->header.depth;
    uint64_t beforeAddress = (uint64_t)&root;
    uint8_t kvFLAG;
    uint32_t j = 0;

    while (len < ERT_KEY_LENGTH / SIZE_OF_CHAR) {
        int matchedPrefixLen;

        if (currentNode->header.len > ERT_NODE_PREFIX_MAX_BYTES) {
            int size = ERT_NODE_PREFIX_MAX_BITS / ERT_NODE_LENGTH * ERT_NODE_LENGTH;
            if (len == 0) {
                currentNode->header.len =
                  (ERT_KEY_LENGTH - len * SIZE_OF_CHAR) <= size ? (ERT_KEY_LENGTH - len * SIZE_OF_CHAR) / SIZE_OF_CHAR : size / SIZE_OF_CHAR;
            } else {
                currentNode->header.len = 0;
            }
            currentNode->header.assign(key, len);
            continue;
        } else {
            matchedPrefixLen = currentNode->header.computePrefix(key, len * SIZE_OF_CHAR);
        }

        if (len + matchedPrefixLen == (ERT_KEY_LENGTH / SIZE_OF_CHAR)) {
            ERTIntKeyValue* kv = NewERTIntKeyValue(key, value);
            clflush((char*)kv, sizeof(ERTIntKeyValue));
            currentNode->nodePut(matchedPrefixLen, kv);
            return;
        }

        if ((matchedPrefixLen / (ERT_NODE_LENGTH / SIZE_OF_CHAR))
            == (currentNode->header.len / (ERT_NODE_LENGTH / SIZE_OF_CHAR))) {
            len += currentNode->header.len % (ERT_NODE_LENGTH / SIZE_OF_CHAR) == 0 ? currentNode->header.len : currentNode->header.len / (ERT_NODE_LENGTH / SIZE_OF_CHAR);
            uint64_t subkey = GET_SUBKEY(key, len * SIZE_OF_CHAR, ERT_NODE_LENGTH);
            uint64_t next = 0;
            uint64_t dir_index = GET_SEG_NUM(subkey, ERT_NODE_LENGTH, currentNode->global_depth);
            ERTIntSegment* tmp_seg = *(ERTIntSegment**)GET_SEG_POS(currentNode, dir_index);
            uint64_t seg_index = GET_BUCKET_NUM(subkey, ERT_BUCKET_MASK_LEN);
            ERTIntBucket* tmp_bucket = &(tmp_seg->bucket[seg_index]);

            int i, bucket_index = -1;
            bool keyValueFlag = false;
            uint64_t beforeA;

            for (i = 0; i < ERT_BUCKET_SIZE; ++i) {
                if (subkey == REMOVE_NODE_FLAG(tmp_bucket->counter[i].subkey)) {
                    next = tmp_bucket->counter[i].value;
                    keyValueFlag = GET_NODE_FLAG(tmp_bucket->counter[i].subkey);
                    beforeA = (uint64_t)&tmp_bucket->counter[i].value;
                    break;
                }
            }

            len += ERT_NODE_LENGTH / SIZE_OF_CHAR;

            if (len == 8) {
                if (next == 0) {
                    bucket_index = tmp_bucket->findPlace(subkey, ERT_NODE_LENGTH, tmp_seg->depth);
                    currentNode->put(subkey, (uint64_t)value, tmp_seg, tmp_bucket, dir_index, seg_index, bucket_index, kvFLAG = 1, beforeAddress);
                    return;
                } else {
                    tmp_bucket->counter[i].value = value;
                    clflush((char*)&tmp_bucket->counter[i].value, 8);
                    return;
                }
            } else {
                if (next == 0) {
                    ERTIntKeyValue* kv = NewERTIntKeyValue(key, value);
                    clflush((char*)kv, sizeof(ERTIntKeyValue));
                    bucket_index = tmp_bucket->findPlace(subkey, ERT_NODE_LENGTH, tmp_seg->depth);
                    currentNode->put(subkey, (uint64_t)kv, tmp_seg, tmp_bucket, dir_index, seg_index, bucket_index, kvFLAG = 1, beforeAddress);
                    return;
                } else {
                    if (keyValueFlag) {
                        uint64_t prekey = ((ERTIntKeyValue*)next)->key;
                        uint64_t prevalue = ((ERTIntKeyValue*)next)->value;
                        if (unlikely(key == prekey)) {
                            ((ERTIntKeyValue*)next)->value = value;
                            clflush((char*)&(((ERTIntKeyValue*)next)->value), 8);
                            return;
                        } else {
                            ERTIntNode* newNode = NewERTIntNode(ERT_NODE_LENGTH, headerDepth + 1);

                            InsertNewNode(prekey, prevalue, newNode, len);
                            InsertNewNode(key, value, newNode, len);

                            clflush((char*)newNode, sizeof(ERTIntNode));

                            tmp_bucket->counter[i].subkey = REMOVE_NODE_FLAG(tmp_bucket->counter[i].subkey);
                            tmp_bucket->counter[i].value = (uint64_t)newNode;

                            clflush((char*)&tmp_bucket->counter[i].value, 8);

                            return;
                        }
                    } else {
                        currentNode = (ERTIntNode*)next;
                        beforeAddress = beforeA;
                        headerDepth = currentNode->header.depth;
                    }
                }
            }
        } else {
            ERTIntNode* newNode = NewERTIntNode(ERT_NODE_LENGTH, headerDepth);

            newNode->header.init(&currentNode->header, matchedPrefixLen, currentNode->header.depth);
            for (int j = currentNode->header.len - matchedPrefixLen; j <= currentNode->header.len; j++) {
                newNode->treeNodeValues[j - currentNode->header.len + matchedPrefixLen] = currentNode->treeNodeValues[j];
            }

            uint64_t subkey = GET_SUBKEY(key, (len + matchedPrefixLen % (ERT_NODE_LENGTH / SIZE_OF_CHAR) != 0 ? 0 : matchedPrefixLen), ERT_NODE_LENGTH);

            ERTIntKeyValue* kv = NewERTIntKeyValue(key, value);
            clflush((char*)kv, sizeof(ERTIntKeyValue));

            newNode->put(subkey, (uint64_t)kv, kvFLAG = 1, (uint64_t)&newNode);
            newNode->put(GET_32BITS(currentNode->header.array, matchedPrefixLen), (uint64_t)currentNode, kvFLAG = 0, (uint64_t)&newNode);

            currentNode->header.depth =
              newNode->header.depth + newNode->header.len / (ERT_NODE_LENGTH / SIZE_OF_CHAR) + 1; // 更新深度
            currentNode->header.len -= (matchedPrefixLen + newNode->header.depth + 1);
            for (int i = 0; i < ERT_NODE_PREFIX_MAX_BYTES - matchedPrefixLen; i++) {
                currentNode->header.array[i] = currentNode->header.array[i + matchedPrefixLen];
            }

            clflush((char*)&(currentNode->header), 8);
            clflush((char*)newNode, sizeof(ERTIntNode));

            if (newNode->header.depth != 0) {
                ((ERTIntBucketKeyValue*)beforeAddress)->value = (uint64_t)newNode;
                clflush((char*)(ERTIntBucketKeyValue*)beforeAddress, sizeof(ERTIntBucketKeyValue));
            } else {
                *(ERTIntNode**)beforeAddress = newNode;
                clflush((char*)beforeAddress, sizeof(ERTIntNode*));
            }
            return;
        }
    }
}

uint64_t ERTInt::Search(uint64_t key) {
restart:
    ERTIntNode* currentNode = root;
    if (currentNode == nullptr) {
        return 0;
    }

    locks().directory(currentNode).read_lock();

    if (currentNode != root) {
        locks().directory(currentNode).read_unlock();
        goto restart;
    }

    uint64_t result = -1;
    int pos = 0;

    while (pos < ERT_KEY_LENGTH / SIZE_OF_CHAR) {
        if (currentNode->header.len / (ERT_NODE_LENGTH / SIZE_OF_CHAR)) {
            if (ERT_KEY_LENGTH / SIZE_OF_CHAR - pos <= currentNode->header.len) {
                int index = currentNode->header.len - ERT_KEY_LENGTH + pos;
                if (currentNode->treeNodeValues[index].key == key) {
                    result = (uint64_t)currentNode->treeNodeValues[index].value;
                    locks().directory(currentNode).read_unlock();
                    return result;
                } else {
                    locks().directory(currentNode).read_unlock();
                    return 0;
                }
            }

            if (!isSame((unsigned char*)currentNode->header.array, key, pos * SIZE_OF_CHAR,
                        currentNode->header.len * SIZE_OF_CHAR)) {
                locks().directory(currentNode).read_unlock();
                return 0;
            }

            pos += currentNode->header.len;
        }

        // uint64_t subkey = GET_16_BITS(key,pos);
        uint64_t subkey = GET_SUBKEY(key, pos * SIZE_OF_CHAR, ERT_NODE_LENGTH);
        bool keyValueFlag = false;
        int i = 0;

        uint64_t dir_index = GET_SEG_NUM(subkey, ERT_NODE_LENGTH, currentNode->global_depth);
        ERTIntSegment* tmp_seg = *(ERTIntSegment**)GET_SEG_POS(currentNode, dir_index);

        locks().segment(tmp_seg).read_lock();

        uint64_t seg_index = GET_BUCKET_NUM(subkey, ERT_BUCKET_MASK_LEN);
        ERTIntBucket* tmp_bucket = &(tmp_seg->bucket[seg_index]);

        locks().bucket(tmp_bucket).read_lock();

        for (i = 0; i < ERT_BUCKET_SIZE; ++i) {
            if (subkey == REMOVE_NODE_FLAG(tmp_bucket->counter[i].subkey)) {
                keyValueFlag = GET_NODE_FLAG(tmp_bucket->counter[i].subkey);
                result = tmp_bucket->counter[i].value;
                break;
            }
        }

        locks().bucket(tmp_bucket).read_unlock();

        // auto next = currentNode->get(subkey, keyValueFlag);

        // pos+=_16_BITS_OF_BYTES;
        pos += _32_BITS_OF_BYTES;

        if (result == -1) {
            locks().segment(tmp_seg).read_unlock();
            locks().directory(currentNode).read_unlock();
            return -1;
        }

        if (keyValueFlag != GET_NODE_FLAG(tmp_bucket->counter[i].subkey)) {
            locks().segment(tmp_seg).read_unlock();
            locks().directory(currentNode).read_unlock();
            goto restart;
        } else if (keyValueFlag) {
            if (pos == 8) {
                if (result != tmp_bucket->counter[i].value) {
                    locks().segment(tmp_seg).read_unlock();
                    locks().directory(currentNode).read_unlock();
                    goto restart;
                }
                locks().segment(tmp_seg).read_unlock();
                locks().directory(currentNode).read_unlock();
                return result;
            } else {
                if (key == ((ERTIntKeyValue*)tmp_bucket->counter[i].value)->key) {
                    if (result != tmp_bucket->counter[i].value) {
                        locks().segment(tmp_seg).read_unlock();
                        locks().directory(currentNode).read_unlock();
                        goto restart;
                    }
                    result = ((ERTIntKeyValue*)tmp_bucket->counter[i].value)->value;
                    locks().segment(tmp_seg).read_unlock();
                    locks().directory(currentNode).read_unlock();
                    return result;
                } else {
                    locks().segment(tmp_seg).read_unlock();
                    locks().directory(currentNode).read_unlock();
                    return 0;
                }
            }
        } else {
            ERTIntNode* subNode = (ERTIntNode*)(tmp_bucket->counter[i].value);

            locks().directory(subNode).read_lock();
            if (subNode != (ERTIntNode*)(tmp_bucket->counter[i].value)) {
                locks().directory(subNode).read_unlock();
                locks().segment(tmp_seg).read_unlock();
                locks().directory(currentNode).read_unlock();
                goto restart;
            }
            locks().segment(tmp_seg).read_unlock();
            locks().directory(currentNode).read_unlock();

            currentNode = subNode;
        }
    }

    locks().directory(currentNode).read_unlock();
    return 0;
}

vector<ERTIntKeyValue> ERTInt::scan(uint64_t left, uint64_t right) {
    vector<ERTIntKeyValue> results;

    nodeScan(root, left, right, results, 0);

    // std::sort(results.begin(), results.end(), [](const ERTIntKeyValue& a, const ERTIntKeyValue& b) {
    //     return a.value < b.value;
    // });
    // for (const auto& kv : results) {
    //     printf("key: %llu, value: %llu\n", kv.key, kv.value);
    // }
    // for (uint64_t i = 0, j = 0; i < 100000000; i++) {
    //     if (results[j].value != i) {
    //         printf("value: %llu\n", i);
    //     } else {
    //         j++;
    //     }
    // }

    return results;
}

void ERTInt::nodeScan(ERTIntNode* tmp, uint64_t left, uint64_t right, vector<ERTIntKeyValue>& res, int pos, uint64_t prefix) {
    if (unlikely(tmp == NULL)) {
        tmp = root;
    }

    uint64_t leftPos = UINT64_MAX, rightPos = UINT64_MAX;

    for (int i = 0; i < tmp->header.len && tmp->header.len % (ERT_NODE_LENGTH / SIZE_OF_CHAR) == 0; i++) {
        uint64_t subkey = GET_SUBKEY(left, pos + i, 8);
        if (subkey == (uint64_t)tmp->header.array[i]) {
            continue;
        } else {
            if (subkey > (uint64_t)tmp->header.array[i]) {
                return;
            } else {
                leftPos = 0;
                break;
            }
        }
    }

    for (int i = 0; i < tmp->header.len && tmp->header.len % (ERT_NODE_LENGTH / SIZE_OF_CHAR) == 0; i++) {
        uint64_t subkey = GET_SUBKEY(right, pos + i, 8);
        if (subkey == (uint64_t)tmp->header.array[i]) {
            continue;
        } else {
            if (subkey > (uint64_t)tmp->header.array[i]) {
                rightPos = tmp->dir_size - 1;
                break;
            } else {
                return;
            }
        }
    }

    if (tmp->header.len % (ERT_NODE_LENGTH / SIZE_OF_CHAR) == 0) {
        prefix = (prefix << tmp->header.len * SIZE_OF_CHAR);

        for (int i = 0; i < tmp->header.len; ++i) {
            prefix += tmp->header.array[i] << (tmp->header.len - i);
        }

        pos += tmp->header.len * SIZE_OF_CHAR;
    }

    uint64_t leftSubkey = UINT64_MAX, rightSubkey = UINT64_MAX;

    if (leftPos == UINT64_MAX) {
        leftSubkey = GET_SUBKEY(left, pos, ERT_NODE_LENGTH);
        leftPos = GET_SEG_NUM(leftSubkey, ERT_NODE_LENGTH, tmp->global_depth);
    }

    if (rightPos == UINT64_MAX) {
        rightSubkey = GET_SUBKEY(right, pos, ERT_NODE_LENGTH);
        rightPos = GET_SEG_NUM(rightSubkey, ERT_NODE_LENGTH, tmp->global_depth);
    }

    prefix = (prefix << ERT_NODE_LENGTH);
    pos += ERT_NODE_LENGTH;

    if (leftSubkey == rightSubkey) {
        bool keyValueFlag;

        uint64_t dir_index = GET_SEG_NUM(leftSubkey, ERT_NODE_LENGTH, tmp->global_depth);
        ERTIntSegment* tmp_seg = *(ERTIntSegment**)GET_SEG_POS(tmp, dir_index);

        uint64_t seg_index = GET_BUCKET_NUM(leftSubkey, ERT_BUCKET_MASK_LEN);
        ERTIntBucket* tmp_bucket = &(tmp_seg->bucket[seg_index]);

        uint64_t value = tmp_seg->bucket[seg_index].get(leftSubkey, keyValueFlag);

        if (value == 0 || (tmp_seg != *(ERTIntSegment**)GET_SEG_POS(tmp, GET_SEG_NUM(leftSubkey, ERT_NODE_LENGTH, tmp->global_depth)))) {
            return;
        }

        if (pos == 64) {
            ERTIntKeyValue tmp_kv;
            tmp_kv.key = leftSubkey + prefix;
            tmp_kv.value = value;
            res.push_back(tmp_kv);
        } else {
            if (keyValueFlag) {
                res.push_back(*(ERTIntKeyValue*)value);
            } else {
                nodeScan((ERTIntNode*)value, left, right, res, pos, prefix + leftSubkey);
            }
        }
        return;
    }

    ERTIntSegment* last_seg = NULL;
    for (uint32_t i = leftPos; i <= rightPos; i++) {
        ERTIntSegment* tmp_seg = *(ERTIntSegment**)GET_SEG_POS(tmp, i);

        if (tmp_seg == last_seg)
            continue;
        else {
            last_seg = tmp_seg;
        }

        for (auto j = 0; j < ERT_MAX_BUCKET_NUM; j++) {
            for (auto k = 0; k < ERT_BUCKET_SIZE; k++) {
                bool keyValueFlag = GET_NODE_FLAG(tmp_seg->bucket[j].counter[k].subkey);
                uint64_t curSubkey = REMOVE_NODE_FLAG(tmp_seg->bucket[j].counter[k].subkey);
                uint64_t value = tmp_seg->bucket[j].counter[k].value;
                if ((value == 0 && curSubkey == 0) || (tmp_seg != *(ERTIntSegment**)GET_SEG_POS(tmp, GET_SEG_NUM(curSubkey, ERT_NODE_LENGTH, tmp->global_depth)))) {
                    continue;
                }
                if ((leftSubkey == UINT64_MAX || curSubkey > leftSubkey) && (rightSubkey == UINT64_MAX || curSubkey < rightSubkey)) {
                    if (pos == 64) {
                        ERTIntKeyValue tmp_kv;
                        tmp_kv.key = curSubkey + prefix;
                        tmp_kv.value = value;
                        res.push_back(tmp_kv);
                    } else {
                        if (keyValueFlag) {
                            res.push_back(*(ERTIntKeyValue*)value);
                        } else {
                            getAllNodes((ERTIntNode*)value, res, pos, prefix + curSubkey);
                        }
                    }
                } else if (curSubkey == leftSubkey || curSubkey == rightSubkey) {
                    if (pos == 64) {
                        ERTIntKeyValue tmp_kv;
                        tmp_kv.key = curSubkey + prefix;
                        tmp_kv.value = value;
                        res.push_back(tmp_kv);
                    } else {
                        if (keyValueFlag) {
                            res.push_back(*(ERTIntKeyValue*)value);
                        } else {
                            nodeScan((ERTIntNode*)value, left, right, res, pos, prefix + curSubkey);
                        }
                    }
                }
            }
        }
    }
}

void ERTInt::getAllNodes(ERTIntNode* tmp, vector<ERTIntKeyValue>& res, int pos, uint64_t prefix) {
    if (tmp == NULL) {
        return;
    }

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
                if (pos == 64) {
                    ERTIntKeyValue tmp;
                    tmp.key = curSubkey + prefix;
                    tmp.value = value;
                    res.push_back(tmp);
                } else {
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
    pos += ERT_NODE_LENGTH;
    ERTIntSegment* last_seg = NULL;

    for (int i = 0; i < tmp->dir_size; i++) {
        ERTIntSegment* tmp_seg = *(ERTIntSegment**)GET_SEG_POS(tmp, i);
        if (tmp_seg == last_seg)
            continue;
        else {
            last_seg = tmp_seg;
            res += ERT_MAX_BUCKET_NUM * sizeof(ERTIntBucket);
        }
        for (auto j = 0; j < ERT_MAX_BUCKET_NUM; j++) {
            for (auto k = 0; k < ERT_BUCKET_SIZE; k++) {
                bool keyValueFlag = GET_NODE_FLAG(tmp_seg->bucket[j].counter[k].subkey);
                uint64_t curSubkey = REMOVE_NODE_FLAG(tmp_seg->bucket[j].counter[k].subkey);
                uint64_t value = tmp_seg->bucket[j].counter[k].value;

                if (pos != 64) {
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

ERTInt* NewExtendibleRadixTreeInt() {
    ERTInt* _new_hash_tree = static_cast<ERTInt*>(concurrency_fast_alloc(sizeof(ERTInt)));
    _new_hash_tree->init();
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

void ERTInt::init() {
    root = NewERTIntNode(ERT_NODE_LENGTH);
}
