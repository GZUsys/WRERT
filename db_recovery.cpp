#include <sys/time.h>
#include <atomic>
#include <iostream>
#include <fstream>
#include <unistd.h>
#include <map>
#include <vector>
#include <chrono>
#include <random>
#include <iomanip>
#include <algorithm>
#include <functional>
#include <memory>
#include <sstream>
#include <cstring>
#include <thread>
#include <mutex>
#include <unordered_set>

#include "rng/rng.h"
#include "wrert/wrert.h"

using namespace std;
using namespace wrert;

static int FLAGS_test_num = 100000000;
static int FLAGS_abort_num = 10000000;
static bool FLAGS_on_pm = false;
static std::string FLAGS_pm_path = "/pmem0/";

struct Config {
    int test_num = FLAGS_test_num;
    int abort_num = FLAGS_abort_num;
    string pm_path = FLAGS_pm_path;
    bool on_pm = FLAGS_on_pm;
} g_config;

void run_crash_recovery_test(int argc, char* argv[]) {
    uint64_t* keys_sparse = new uint64_t[g_config.test_num];
    uint64_t max_sparse_key_ = 0;
    uint64_t min_sparse_key_ = 0;

    std::random_device rd;
    rng r;
    rng_init(&r, 1, 2);
    // rng_init(&r, rd(), 2);
    unordered_set<uint64_t> unique_sparse_keys;
    for (int i = 0; i < g_config.test_num; i++) {
        keys_sparse[i] = rng_next(&r);
        max_sparse_key_ = max(max_sparse_key_, keys_sparse[i]);
        min_sparse_key_ = min(min_sparse_key_, keys_sparse[i]);
    }

    // std::sort(keys_sparse, keys_sparse + g_config.test_num);

    int print_count = 0;
    // while (print_count < g_config.test_num) {
    while (print_count < 10) {
        printf("  |-> Sparse 数据集 Key %d: %llu\n", print_count, keys_sparse[print_count++]);
    }

    fflush(stdout);

    WRERT* wrert = nullptr;
    unordered_set<uint64_t> unique_WRERT_keys;

    if (access("/pmem0/wrert/0", F_OK) == 0) {
        init_fast_allocator(true, true, "/pmem0/wrert/");
        auto start_time = std::chrono::high_resolution_clock::now();
        wrert = wrert->recoveryTree();
        auto end_time = std::chrono::high_resolution_clock::now();
        printf("  |-> WRERT 恢复时长: %llu ns\n", std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count());
        printf("  |-> WRERT %d 个键恢复时长: %.6f s\n", g_config.abort_num, std::chrono::duration<double>(end_time - start_time).count());

        // uint64_t result_count = wrert->lookupRange(min_sparse_key_, max_sparse_key_).size();

        for (int i = 0; i < g_config.abort_num + 1; i++) {
            uint64_t result = wrert->search(keys_sparse[i]);
            if (result != (uint64_t)-1) {
                unique_WRERT_keys.insert(result);
            }
        }
        printf("  |-> WRERT search 操作返回的唯一值数量: %llu\n", unique_WRERT_keys.size());

        start_time = std::chrono::high_resolution_clock::now();
        uint64_t memory_bytes = wrert->memory_profile(nullptr);
        end_time = std::chrono::high_resolution_clock::now();
        double memory_gb = static_cast<double>(memory_bytes) / (1024.0 * 1024.0 * 1024.0);
        printf("  |-> WRERT 内存占用: %lu bytes (%.6f GB)\n", memory_bytes, memory_gb);
        printf("  |-> WRERT 内存恢复遍历时长: %llu ns\n", std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count());
        printf("  |-> WRERT %d 个键内存恢复遍历时长: %.6f s\n", g_config.abort_num, std::chrono::duration<double>(end_time - start_time).count());

        // for (int i = 0; i < g_config.abort_num + 1; i++) {
        //     wrert->insert(keys_sparse[i], i + 10);
        // }

        // unique_WRERT_keys.clear();
        // for (int i = 0; i < g_config.abort_num + 5; i++) {
        //     uint64_t result = wrert->search(keys_sparse[i]);
        //     if (result != (uint64_t)-1) {
        //         unique_WRERT_keys.insert(result);
        //     }
        // }
        // printf("  |-> WRERT search 操作返回的唯一值数量: %llu\n", unique_WRERT_keys.size());

    } else {
        init_fast_allocator(true, true, "/pmem0/wrert/");
        wrert = NewWRERT();

        for (int i = 0, j = 1; i < g_config.test_num; i++) {
            wrert->insert(keys_sparse[i], i);
            if (j++ == g_config.abort_num) {
                abort();
            }
        }

        // uint64_t result_count = wrert->lookupRange(min_sparse_key_, max_sparse_key_).size();
        // printf("  |-> WRERT 搜索范围 [%llu, %llu] 共 %llu 个键\n", min_sparse_key_, max_sparse_key_, result_count);

        // auto start_time = std::chrono::high_resolution_clock::now();
        // uint64_t memory_bytes = wrert->memory_profile(nullptr);
        // auto end_time = std::chrono::high_resolution_clock::now();
        // double memory_gb = static_cast<double>(memory_bytes) / (1024.0 * 1024.0 * 1024.0);
        // printf("  |-> WRERT 内存占用: %lu bytes (%.6f GB)\n", memory_bytes, memory_gb);
        // printf("  |-> WRERT 内存遍历时长: %llu ns\n", std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count());
        // printf("  |-> WRERT %d 个键内存遍历时长: %.6f s\n", g_config.test_num, std::chrono::duration<double>(end_time - start_time).count());
    }
}

int main(int argc, char* argv[]) {
    g_config.on_pm = true;
    g_config.abort_num = atoi(argv[1]);
    g_config.pm_path = argv[2];
    cout << "测试数据量: " << g_config.test_num << " 个键" << endl;
    cout << "PM 模式启用, PM 路径: " << g_config.pm_path << endl;

    // init_fast_allocator(true, g_config.on_pm, g_config.pm_path.c_str());

    run_crash_recovery_test(argc, argv);

    fast_free();

    return 0;
}
