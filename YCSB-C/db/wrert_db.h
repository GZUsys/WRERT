//
//  wrert_db.h
//  YCSB-C
//
#ifndef YCSB_C_WRERT_DB_H_
#define YCSB_C_WRERT_DB_H_

#include <cstring>
#include <cstdlib>
#include <atomic>
#include <cstdint>
#include <chrono>

#include "core/db.h"
#include "wrert/wrert.h"

namespace ycsbc {

class WRERTDB : public DB {
private:
    wrert::WRERT* wrert_;

    std::atomic<uint64_t> insert_count_{0};
    std::atomic<uint64_t> update_count_{0};
    std::atomic<uint64_t> read_count_{0};
    std::atomic<uint64_t> scan_count_{0};
    std::atomic<uint64_t> op_count_{0};

    std::atomic<uint64_t> total_latency_ns_{0};
    std::atomic<uint64_t> insert_latency_ns_{0};
    std::atomic<uint64_t> update_latency_ns_{0};
    std::atomic<uint64_t> read_latency_ns_{0};
    std::atomic<uint64_t> scan_latency_ns_{0};

public:
    WRERTDB() {
        printf("[New] WRERTDB New\n");
        wrert_ = wrert::NewWRERT();
    }
    ~WRERTDB() override {}

    void Init() override {
        printf("[Init] WRERTDB Init\n");
    }

    void Close() override {
        printf("[Close] WRERTDB Close\n");
    }

    // 提供获取延迟的方法
    double GetTotalLatencyS() {
        // printf("total_latency_ns: %llu\n", total_latency_ns_.load(std::memory_order_relaxed));
        return static_cast<double>(total_latency_ns_.load(std::memory_order_relaxed)) / 1000000000.0;
    }
    uint64_t GetOpCount() {
        return op_count_.load(std::memory_order_relaxed);
    }
    double GetThroughputMops() {
        printf("insert_count_: %llu, update_count_: %llu, read_count_: %llu, scan_count_: %llu, op_count_: %llu\n",
               insert_count_.load(std::memory_order_relaxed),
               update_count_.load(std::memory_order_relaxed),
               read_count_.load(std::memory_order_relaxed),
               scan_count_.load(std::memory_order_relaxed),
               op_count_.load(std::memory_order_relaxed));
        uint64_t ops = op_count_.load(std::memory_order_relaxed);
        double latency_s = GetTotalLatencyS();
        return (latency_s > 0) ? (ops / latency_s) / 1000000.0 : 0.0;
    }
    void ResetStats() {
        op_count_.store(0, std::memory_order_relaxed);
        insert_count_.store(0, std::memory_order_relaxed);
        scan_count_.store(0, std::memory_order_relaxed);
        update_count_.store(0, std::memory_order_relaxed);
        read_count_.store(0, std::memory_order_relaxed);
        total_latency_ns_.store(0, std::memory_order_relaxed);
    }

    // 实现 DB 接口
    int Read(const std::string& table, const std::string& key,
             const std::vector<std::string>* fields,
             std::vector<KVPair>& result) override {
        return kOK;
    }

    int ReadUint64(const std::string& table, uint64_t key) override {
        uint64_t value = 0;
        auto start = std::chrono::high_resolution_clock::now();
        value = wrert_->search(key);
        auto end = std::chrono::high_resolution_clock::now();
        read_count_.fetch_add(1, std::memory_order_relaxed);
        op_count_.fetch_add(1, std::memory_order_relaxed);
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
        read_latency_ns_.fetch_add(ns, std::memory_order_relaxed);
        total_latency_ns_.fetch_add(ns, std::memory_order_relaxed);
        // printf("Read key %llu: value %llu, 耗时: %llu ns\n",
        //        key, value, ns);
        return kOK;
    }

    int Scan(const std::string& table, const std::string& start_key,
             int record_count, const std::vector<std::string>* fields,
             std::vector<std::vector<KVPair>>& result) override {
        return kOK;
    }

    int ScanUint64(const std::string& table, uint64_t start_key, uint64_t end_key) override {
        uint64_t count = 0;
        auto start = std::chrono::high_resolution_clock::now();
        count = wrert_->lookupRange(start_key, end_key).size();
        auto end = std::chrono::high_resolution_clock::now();
        scan_count_.fetch_add(1, std::memory_order_relaxed);
        op_count_.fetch_add(1, std::memory_order_relaxed);
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
        scan_latency_ns_.fetch_add(ns, std::memory_order_relaxed);
        total_latency_ns_.fetch_add(ns, std::memory_order_relaxed);
        printf("Scan [%llu, %llu], count: %llu, 耗时: %.6f s\n",
               start_key, end_key, count, ns / 1000000000.0);
        return kOK;
    }

    virtual int Insert(const std::string& table, const std::string& key,
                       std::vector<KVPair>& values) override {
        return kOK;
    }

    int InsertUint64(const std::string& table, uint64_t key, uint64_t value) override {
        auto start = std::chrono::high_resolution_clock::now();
        wrert_->insert(key, value);
        auto end = std::chrono::high_resolution_clock::now();
        insert_count_.fetch_add(1, std::memory_order_relaxed);
        op_count_.fetch_add(1, std::memory_order_relaxed);
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
        insert_latency_ns_.fetch_add(ns, std::memory_order_relaxed);
        total_latency_ns_.fetch_add(ns, std::memory_order_relaxed);
        // printf("Insert key %llu: value %llu, 耗时: %.6f s\n",
        //        key, value, ns / 1000000000.0);
        return kOK;
    }

    int Update(const std::string& table, const std::string& key,
               std::vector<KVPair>& values) override {
        return kOK;
    }

    int UpdateUint64(const std::string& table, uint64_t key, uint64_t value) override {
        auto start = std::chrono::high_resolution_clock::now();
        wrert_->insert(key, value);
        auto end = std::chrono::high_resolution_clock::now();
        update_count_.fetch_add(1, std::memory_order_relaxed);
        op_count_.fetch_add(1, std::memory_order_relaxed);
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
        update_latency_ns_.fetch_add(ns, std::memory_order_relaxed);
        total_latency_ns_.fetch_add(ns, std::memory_order_relaxed);
        // printf("Update key %llu: value %llu, 耗时: %llu ns\n",
        //        key, value, ns);
        return kOK;
    }

    int Delete(const std::string& table, const std::string& key) override {
        return kOK;
    }
};

} // namespace ycsbc

#endif // YCSB_C_WRERT_DB_H_
