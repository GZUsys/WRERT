//
//  client.h
//  YCSB-C
//
//  Created by Jinglei Ren on 12/10/14.
//  Copyright (c) 2014 Jinglei Ren <jinglei@ren.systems>.
//

#ifndef YCSB_C_CLIENT_H_
#define YCSB_C_CLIENT_H_

#include <string>
#include "db.h"
#include "core_workload.h"
#include "utils.h"

namespace ycsbc {

class Client {
public:
    Client(DB& db, CoreWorkload& wl) : db_(db), workload_(wl) {}

    virtual bool DoInsert();
    virtual bool DoTransaction();

    virtual ~Client() {}

protected:
    virtual int TransactionRead();
    virtual int TransactionReadModifyWrite();
    virtual int TransactionScan();
    virtual int TransactionUpdate();
    virtual int TransactionInsert();

    DB& db_;
    CoreWorkload& workload_;
};

inline bool Client::DoInsert() {
    // 1. 生成主键 (Primary Key)
    // NextSequenceKey() 通常返回递增的字符串键，如 "user0", "user1"...
    // ⚠️ 注意：在多线程 Load 时，如果未加锁或使用原子变量，可能产生重复 Key
    // std::string key = workload_.NextSequenceKey();
    // // 2. 构建值 (Value / KV Pairs)
    // // BuildValues 会根据 workload 配置（fieldcount, fieldlength, datadistribution）
    // // 填充 pairs 向量。通常 pairs[0] 是 "field0" -> "随机/顺序字符串"
    // std::vector<DB::KVPair> pairs;
    // workload_.BuildValues(pairs);
    // printf("key: %s, pairs: %s, %s\n", key.c_str(), pairs[0].first.c_str(), pairs[0].second.c_str());
    // // 4. 执行插入并返回成功状态
    // // NextTable() 返回表名（YCSB 默认单表 "usertable"）
    // // DB::kOK 表示插入成功，其他值表示失败（如 key 已存在、PMEM 满等）
    // return (db_.Insert(workload_.NextTable(), key, pairs) == DB::kOK);

    // ✅ 1. KV 生成（不计入吞吐量）
    auto kv = workload_.NextSequenceKV();

    return (db_.InsertUint64(workload_.NextTable(), kv.first, kv.second) == DB::kOK);
}

inline bool Client::DoTransaction() {
    int status = -1;
    switch (workload_.NextOperation()) {
        case READ:
            status = TransactionRead();
            break;
        case UPDATE:
            status = TransactionUpdate();
            break;
        case INSERT:
            status = TransactionInsert();
            break;
        case SCAN:
            status = TransactionScan();
            break;
        case READMODIFYWRITE:
            status = TransactionReadModifyWrite();
            break;
        default:
            throw utils::Exception("Operation request is not recognized!");
    }
    assert(status >= 0);
    return (status == DB::kOK);
}

// inline int Client::TransactionRead() {
//     const std::string& table = workload_.NextTable();
//     const std::string& key = workload_.NextTransactionKey();
//     std::vector<DB::KVPair> result;
//     if (!workload_.read_all_fields()) {
//         std::vector<std::string> fields;
//         fields.push_back("field" + workload_.NextFieldName());
//         return db_.Read(table, key, &fields, result);
//     } else {
//         return db_.Read(table, key, NULL, result);
//     }
// }
// inline int Client::TransactionReadModifyWrite() {
//     const std::string& table = workload_.NextTable();
//     const std::string& key = workload_.NextTransactionKey();
//     std::vector<DB::KVPair> result;
//     if (!workload_.read_all_fields()) {
//         std::vector<std::string> fields;
//         fields.push_back("field" + workload_.NextFieldName());
//         db_.Read(table, key, &fields, result);
//     } else {
//         db_.Read(table, key, NULL, result);
//     }
//     std::vector<DB::KVPair> values;
//     if (workload_.write_all_fields()) {
//         workload_.BuildValues(values);
//     } else {
//         workload_.BuildUpdate(values);
//     }
//     return db_.Update(table, key, values);
// }
// inline int Client::TransactionScan() {
//     const std::string& table = workload_.NextTable();
//     const std::string& key = workload_.NextTransactionKey();
//     int len = workload_.NextScanLength();
//     std::vector<std::vector<DB::KVPair>> result;
//     if (!workload_.read_all_fields()) {
//         std::vector<std::string> fields;
//         fields.push_back("field" + workload_.NextFieldName());
//         return db_.Scan(table, key, len, &fields, result);
//     } else {
//         return db_.Scan(table, key, len, NULL, result);
//     }
// }
// inline int Client::TransactionUpdate() {
//     const std::string& table = workload_.NextTable();
//     const std::string& key = workload_.NextTransactionKey();
//     std::vector<DB::KVPair> values;
//     if (workload_.write_all_fields()) {
//         workload_.BuildValues(values);
//     } else {
//         workload_.BuildUpdate(values);
//     }
//     return db_.Update(table, key, values);
// }
// inline int Client::TransactionInsert() {
//     const std::string& table = workload_.NextTable();
//     const std::string& key = workload_.NextSequenceKey();
//     std::vector<DB::KVPair> values;
//     workload_.BuildValues(values);
//     return db_.Insert(table, key, values);
// }

inline int Client::TransactionRead() {
    // ⭐ 1. 用 Zipfian 生成器获取索引，而非直接用自增/随机数
    auto kv = workload_.NextTransactionKV();
    // ⭐ 2. 通过索引从稀疏数据集中获取真实 Key
    return db_.ReadUint64(workload_.NextTable(), kv.first);
}

inline int Client::TransactionUpdate() {
    auto kv = workload_.NextTransactionKV();
    uint64_t value = workload_.NextTransactionValue();
    return db_.UpdateUint64(workload_.NextTable(), kv.first, value);
}

inline int Client::TransactionInsert() {
    // ✅ 修正后：YCSB CoreWorkload 保证此处生成的 key 在 Load 阶段不存在
    // Workload D: 纯新键插入，且新键会立即被 LatestGenerator 纳入可读范围
    auto kv = workload_.NextTransactionNewKV();
    return db_.InsertUint64(workload_.NextTable(), kv.first, kv.second);
}

inline int Client::TransactionScan() {
    int start_idx = -1, end_idx = -1;
    size_t len = workload_.NextScanLength();
    // ⭐ 用 Zipfian 生成器获取索引，而非直接用自增/随机数
    auto start_kv = workload_.NextTransactionScanKV(&start_idx);
    end_idx = start_idx + len - 1;
    // ⭐ 通过索引从稀疏数据集中获取真实 Key
    auto end_kv = workload_.NextTransactionScanKV(&end_idx);
    // printf("start_idx: %d, end_idx: %d, len: %zu\n", start_idx, end_idx, len);
    return db_.ScanUint64(workload_.NextTable(), start_kv.first, end_kv.first);
}

inline int Client::TransactionReadModifyWrite() {
    auto kv = workload_.NextTransactionKV();
    db_.ReadUint64(workload_.NextTable(), kv.first);
    uint64_t value = workload_.NextTransactionValue();
    return db_.UpdateUint64(workload_.NextTable(), kv.first, value);
}

} // namespace ycsbc

#endif // YCSB_C_CLIENT_H_
