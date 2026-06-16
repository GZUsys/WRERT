#include <atomic>
#include <iostream>
#include <fstream>
#include <unistd.h>
#include <map>
#include <sys/time.h>
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
#include "ert/ERT_int.h"
#include "roert/roert.h"
// #include "roert/roert_node.h"
// #include "fastfair/fastfair.h"
// #include "wope/wope.h"
// #include "lbtree/lbtree.h"
#include "wort/wort.h"
#include "woart/woart.h"
#include "roart/roart.h"

using namespace std;
using namespace roert;

// ==================== 全局配置参数 ====================
// 基准测试参数
static int FLAGS_test_num = 100000;               // 测试数据量
static int FLAGS_clustered_num = 100000000;       // Clustered 数据集大小
static int FLAGS_keys_per_subset = 32;            // 每个子集的键数量
static int FLAGS_per_ops = 5000000;               // 每次操作数量
static int FLAGS_range_query_size = 100;          // 范围查询大小
static int FLAGS_memory_profile_interval = 10000; // 内存分析间隔
static std::string FLAGS_file_path;               // 文件路径
static bool FLAGS_on_pm = false;                  // 是否在持久内存上运行
static std::string FLAGS_pm_path = "/pmem0/";     // PM 路径
static std::string FLAGS_index_type = "";         // 索引类型
static double FLAGS_selectivity = 1;              // 选择性
static bool FLAGS_random = true;                  // 是否随机插入键
static int FLAGS_seed = 1;                        // 随机种子

// 数据集测试配置
static bool FLAGS_test_dense = false;     // 是否测试 Dense 数据集
static bool FLAGS_test_sparse = true;     // 是否测试 Sparse 数据集
static bool FLAGS_test_clustered = false; // 是否测试 Clustered 数据集

static bool FLAGS_test_variable = false;   // 是否测试 Variable 数据集
static int FLAGS_test_variable_min = 8;    // 最小键长度
static int FLAGS_test_variable_max = 1024; // 最大键长度

// YCSB 测试配置
static bool FLAGS_test_ycsb = false;          // 是否测试 YCSB 数据集
static int FLAGS_ycsb_threads = 1;            // YCSB 并发线程数
static std::string FLAGS_ycsb_workload = "";  // YCSB 工作负载类型
static std::string FLAGS_ycsb_data_file = ""; // YCSB 数据文件路径

// YCSB 数据文件路径（LOAD 阶段）
static std::map<std::string, std::string> FLAGS_ycsb_data_file_load = {
  {"Amazon", "./YCSB_Data/Amazon/workload_load.csv"},
  {"Wiki", "./YCSB_Data/Wiki/workload_load.csv"},
  {"AIiTianChi", "./YCSB_Data/AIiTianChi/workload_load.csv"}};
//   {"Wiki", "./YCSB_Data/Wiki/load.csv"}};

static std::map<std::string, int> FLAGS_ycsb_operations = {
  {"Load", 100000},
  {"Run", 100000}};

static std::map<std::string, std::string> YCSB_WORKLOAD_DESCRIPTIONS = {
  {"a", "写密集型: 50% 读操作, 50% 更新操作"},
  {"b", "读密集型: 95% 读操作, 5% 更新操作"},
  {"c", "只读: 100% 读操作"},
  {"d", "读取最近插入的数据: 95% 读操作, 5% 插入操作"},
  {"e", "短范围扫描: 95% 扫描操作, 5% 插入操作"},
  {"f", "读-修改-写: 50% 读操作, 50% 更新操作"}};

// 获取YCSB工作负载描述
std::string get_ycsb_workload_description(const std::string& workload) {
    auto it = YCSB_WORKLOAD_DESCRIPTIONS.find(workload);
    if (it != YCSB_WORKLOAD_DESCRIPTIONS.end()) {
        return it->second;
    }
    return "未知工作负载类型";
}

// 输出配置
static bool FLAGS_verbose = false;    // 详细输出模式
static bool FLAGS_log_to_file = true; // 是否记录到文件

enum class YCSBPhase {
    LOAD,
    RUN,
    NULL_PHASE
};

struct Config {
    int test_num = FLAGS_test_num;
    int clustered_num = FLAGS_clustered_num;
    int keys_per_subset = FLAGS_keys_per_subset;
    int per_ops = FLAGS_per_ops;
    int range_query_size = FLAGS_range_query_size;
    int memory_profile_interval = FLAGS_memory_profile_interval;

    string pm_path = FLAGS_pm_path;
    string index_type = FLAGS_index_type;
    double selectivity = FLAGS_selectivity;
    bool on_pm = FLAGS_on_pm;
    bool random = FLAGS_random;
    int seed = FLAGS_seed;

    bool test_dense = FLAGS_test_dense;
    bool test_sparse = FLAGS_test_sparse;
    bool test_clustered = FLAGS_test_clustered;
    bool test_variable = FLAGS_test_variable;
    int test_variable_min = FLAGS_test_variable_min;
    int test_variable_max = FLAGS_test_variable_max;

    bool test_ycsb = FLAGS_test_ycsb;
    string ycsb_workload = FLAGS_ycsb_workload;
    string ycsb_data_file = FLAGS_ycsb_data_file;
    string ycsb_data_file_load = "";
    int ycsb_threads = FLAGS_ycsb_threads;
    std::map<std::string, int> ycsb_operations = FLAGS_ycsb_operations;
} g_config;

struct PerformanceMetrics {
    struct OperationMetrics {
        double throughput = 0.0;       // MOPS 或 KQPS
        double latency = 0.0;          // 秒
        double avg_time_us = 0.0;      // 微秒/操作
        double total_time_us = 0.0;    // 总时间（微秒）
        uint64_t operations_count = 0; // 操作数量
        uint64_t bytes_processed = 0;  // 处理字节数

        // 计算吞吐量（MOPS）
        void calculate_throughput(uint64_t num_ops) {
            operations_count = num_ops;
            if (latency > 0) {
                throughput = num_ops / latency / 1000000.0; // MOPS
            }
        }

        // 计算范围查询吞吐量（KQPS）
        void calculate_range_throughput(uint64_t num_results) {
            operations_count = num_results;
            if (latency > 0) {
                throughput = num_results / latency / 1000.0; // KQPS
            }
        }
    };

    enum DatasetType {
        DENSE,
        SPARSE,
        CLUSTERED,
        VARIABLE,
        YCSB_WORKLOAD_A,
        YCSB_WORKLOAD_B,
        YCSB_WORKLOAD_C,
        YCSB_WORKLOAD_D,
        YCSB_WORKLOAD_E,
        YCSB_WORKLOAD_F
    };

    enum OperationType {
        INSERT,
        SEARCH,
        RANGE_001,
        RANGE_005,
        RANGE_01,
        RANGE_1,
        YCSB_LOAD,
        YCSB_RUN
    };

    std::map<DatasetType, std::map<OperationType, OperationMetrics>> operations;

    struct MemoryUsage {
        uint64_t bytes = 0;
        double utilization_percent = 0.0;
        double gb() const {
            return static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
        }
        double mb() const {
            return static_cast<double>(bytes) / (1024.0 * 1024.0);
        }
    };

    std::map<DatasetType, MemoryUsage> memory;
    double total_time_seconds = 0.0;

    OperationMetrics& get(DatasetType dataset, OperationType op) {
        return operations[dataset][op];
    }

    MemoryUsage& get_memory(DatasetType dataset) {
        return memory[dataset];
    }
};

struct OperationRecord {
    string operation_type; // 操作类型："INSERT", "READ", "UPDATE", "SCAN", "DELETE"
    uint64_t key;          // 键
    uint64_t value;        // 值（自增ID）
    char op_char;          // 操作类型字符标识
    YCSBPhase phase;       // 操作阶段：LOAD 或 RUN

    OperationRecord(const string& op, uint64_t k, uint64_t v, char oc, YCSBPhase p) : operation_type(op), key(k), value(v), op_char(oc), phase(p) {}
};

static string dataset_display_name(PerformanceMetrics::DatasetType type) {
    switch (type) {
        case PerformanceMetrics::DENSE:
            return "Dense";
        case PerformanceMetrics::SPARSE:
            return "Sparse";
        case PerformanceMetrics::CLUSTERED:
            return "Clustered";
        case PerformanceMetrics::VARIABLE:
            return "Variable";
        case PerformanceMetrics::YCSB_WORKLOAD_A:
            return "YCSB_A";
        case PerformanceMetrics::YCSB_WORKLOAD_B:
            return "YCSB_B";
        case PerformanceMetrics::YCSB_WORKLOAD_C:
            return "YCSB_C";
        case PerformanceMetrics::YCSB_WORKLOAD_D:
            return "YCSB_D";
        case PerformanceMetrics::YCSB_WORKLOAD_E:
            return "YCSB_E";
        case PerformanceMetrics::YCSB_WORKLOAD_F:
            return "YCSB_F";
        default:
            return "invalid";
    }
}

struct VariableKey {
    std::unique_ptr<char[]> data; // 动态分配的字符数组
    uint64_t length;              // 键的实际长度

    // 默认构造函数
    VariableKey() : length(0) {
        data = nullptr;
    }

    // 构造函数
    VariableKey(uint64_t len) : length(len) {
        data = std::make_unique<char[]>(length);
        static const std::string charset =
          "0123456789"
          "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
          "abcdefghijklmnopqrstuvwxyz"
          "!@#$%^&*()_+-=[]{}|;:,.<>?";
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, charset.size() - 1);

        for (uint64_t i = 0; i < length; ++i) {
            data[i] = charset[dis(gen)];
        }
    }

    // 拷贝构造函数 - 添加这一部分
    VariableKey(const VariableKey& other) : length(other.length) {
        if (other.data) {
            data = std::make_unique<char[]>(length);
            std::copy(other.data.get(), other.data.get() + length, data.get());
        } else {
            data = nullptr;
        }
    }

    // 移动构造函数
    VariableKey(VariableKey&& other) noexcept : length(other.length), data(std::move(other.data)) {
        other.length = 0;
    }

    // 拷贝赋值运算符
    VariableKey& operator=(const VariableKey& other) {
        if (this != &other) {
            length = other.length;
            if (other.data) {
                data = std::make_unique<char[]>(length);
                std::copy(other.data.get(), other.data.get() + length, data.get());
            } else {
                data = nullptr;
            }
        }
        return *this;
    }

    // 移动赋值运算符
    VariableKey& operator=(VariableKey&& other) noexcept {
        if (this != &other) {
            length = other.length;
            data = std::move(other.data);
            other.length = 0;
        }
        return *this;
    }
};

class LogManager {
private:
    ofstream log_file_;
    bool verbose_mode_;

public:
    LogManager(const string& filename, bool verbose = false) : verbose_mode_(verbose) {
        if (FLAGS_log_to_file && !filename.empty()) {
            // log_file_.open(filename, ios::out | ios::trunc);
            log_file_.open(filename, ios::out | ios::app);
            if (!log_file_.is_open()) {
                cerr << "WARNING: 无法打开日志文件: " << filename << "，将输出到标准输出" << endl;
            } else {
                // 记录开始时间
                time_t now = time(nullptr);
                char time_str[100];
                strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&now));

                log_file_ << "=====================================================================================\n";
                if (g_config.test_ycsb) {
                    log_file_ << "YCSB 工作负载测试开始时间: " << time_str << "\n";
                    log_file_ << "YCSB 测试: workload=" << g_config.ycsb_workload
                              << " (" << get_ycsb_workload_description(g_config.ycsb_workload) << ")"
                              << ", 线程数=" << g_config.ycsb_threads
                              << ", 数据文件=" << g_config.ycsb_data_file << "\n";
                } else {
                    log_file_ << "基准测试开始时间: " << time_str << "\n";
                    log_file_ << "测试数据集: Dense and Sparse testNum=" << g_config.test_num
                              << ", Clustered testNum=" << g_config.clustered_num
                              << ", seed=" << g_config.seed << "\n";
                }
                log_file_ << "=====================================================================================\n";
                log_file_.flush();
            }
        }
    }

    ~LogManager() {
        if (log_file_.is_open()) {
            log_file_.close();
        }
    }

    template <typename... Args>
    void log(Args&&... args) {
        string message = format_message(std::forward<Args>(args)...);

        if (log_file_.is_open()) {
            log_file_ << message << endl;
            log_file_.flush();
        }
        if (verbose_mode_) {
            cout << message << endl;
        }
    }

    void log_Line_break() {
        log_raw("\n");
    }

    void log_raw(const string& message) {
        if (log_file_.is_open()) {
            log_file_ << message;
            log_file_.flush();
        }
        if (verbose_mode_) {
            cout << message;
        }
    }

    void log_section(const string& title) {
        const int box_width = 85;

        // 构建方框
        string message = "";
        message += "+" + string(box_width - 2, '-') + "+\n";
        message += "|" + title + "\n";
        message += "+" + string(box_width - 2, '-') + "+\n";

        log_raw(message);
    }

    void log_info(const string& key, const string& value = "") {
        char buf[200];
        snprintf(buf, sizeof(buf), "%s: %s", key.c_str(), value.c_str());
        log(buf);
    }

    void log_warning(const string& message) {
        string warning_msg = "WARNING: " + message;
        log(warning_msg);
        cerr << warning_msg << endl; // 同时输出到标准错误
    }

    void log_error(const string& message) {
        string error_msg = "ERROR: " + message;
        log(error_msg);
        cerr << error_msg << endl; // 同时输出到标准错误
    }

    bool is_open() const {
        return log_file_.is_open();
    }

private:
    template <typename T, typename... Args>
    string format_message(T&& first, Args&&... rest) {
        stringstream ss;
        ss << std::forward<T>(first);
        if constexpr (sizeof...(rest) > 0) {
            ss << format_message(std::forward<Args>(rest)...);
        }
        return ss.str();
    }

    string format_message() {
        return "";
    }
};

class YCSBLoader {
private:
    vector<OperationRecord> load_records; // Load 阶段的操作记录
    vector<OperationRecord> run_records;  // Run 阶段的操作记录
    LogManager* log_manager_;             // 日志管理器指针

public:
    YCSBLoader(const string& data_file, LogManager* log_manager = nullptr) : log_manager_(log_manager) {
        // 加载 YCSB 数据文件（LOAD 阶段）
        load_ycsb_data(g_config.ycsb_data_file_load, YCSBPhase::LOAD);
        // 加载 YCSB 数据文件（RUN 阶段）
        // load_ycsb_data(data_file, YCSBPhase::RUN);
    }

    // 加载 YCSB 数据文件
    void load_ycsb_data(const string& data_file, YCSBPhase phase) {
        ifstream file(data_file);
        if (!file.is_open()) {
            throw runtime_error("无法打开 YCSB 数据文件: " + data_file);
        }

        string line;
        int line_num = 0;

        log_manager_->log_info("  |-> YCSB 数据文件", data_file);

        static uint64_t auto_increment_id = 1;
        uint64_t value;

        // 分别记录插入、读取、更新的操作数
        int insert_count = 0;
        int read_count = 0;
        int update_count = 0;

        uint64_t min_key = UINT64_MAX; // 初始化为最大可能值
        uint64_t max_key = 0;

        while (getline(file, line)) {
            line_num++;

            // 跳过空行
            if (line.empty()) {
                continue;
            }

            // 解析 YCSB 数据文件. 格式格式: operation key
            istringstream iss(line);
            string operation;
            uint64_t key;

            // if (iss >> operation >> key) {
            if (iss >> key) {
                // 更新最小值和最大值
                max_key = max(max_key, key);
                min_key = min(min_key, key);

                // 根据 operation 映射操作类型
                char op_char = ' ';
                operation = "insert";
                // if (operation == "INSERT" || operation == "insert") {
                op_char = 'i';
                value = auto_increment_id++;
                insert_count++;
                // } else if (operation == "READ" || operation == "read") {
                //     op_char = 'r';
                //     value = 0;
                //     read_count++;
                // } else if (operation == "UPDATE" || operation == "update") {
                //     op_char = 'u';
                //     value = auto_increment_id++;
                //     update_count++;
                // } else if (operation == "SCAN" || operation == "scan") {
                //     op_char = 's';
                // }

                // 如果是 load 阶段的操作，记录到 load_records；如果是 run 阶段的操作，记录到 run_records
                if (phase == YCSBPhase::LOAD) {
                    load_records.push_back(OperationRecord(operation, key, value, op_char, phase));
                } else {
                    run_records.push_back(OperationRecord(operation, key, value, op_char, phase));
                }
            } else {
                cerr << "警告: 跳过无法解析的行 " << line_num << ": " << line << endl;
            }
        }

        // 在填充 load_records 后添加以下代码：
        std::random_device rd; // 用于生成随机种子
        std::mt19937 g(rd());  // 使用梅森旋转算法的随机数生成器
        std::shuffle(load_records.begin(), load_records.end(), g);

        log_manager_->log_info("  |-> LOAD 阶段 Key 取值区间", to_string(min_key) + " - " + to_string(max_key));

        // 打印前几行数据
        int print_count = 0;
        while (print_count < 100) {
            if (phase == YCSBPhase::LOAD) {
                string key_info = to_string(load_records[print_count].key) + ", value: " + to_string(load_records[print_count].value) + " (operation: " + load_records[print_count].operation_type + ")";
                log_manager_->log_info("  |-> key " + to_string(print_count + 1), key_info);
            } else {
                string key_info = to_string(run_records[print_count].key) + ", value: " + to_string(run_records[print_count].value) + " (operation: " + run_records[print_count].operation_type + ")";
                log_manager_->log_info("  |-> key " + to_string(print_count + 1), key_info);
            }

            print_count++;
        }

        // 提取所有键值
        // std::vector<uint64_t> keys;
        // keys.reserve(load_records.size());
        // for (const auto& record : load_records) {
        //     keys.push_back(record.key);
        // }

        // // 排序键值以便分析
        // std::sort(keys.begin(), keys.end());

        // // 获取最小值和最大值
        // min_key = keys.front();
        // max_key = keys.back();

        // // 计算稀疏度
        // uint64_t range_size = max_key - min_key + 1;                                 // 整个范围的大小
        // uint64_t unique_keys = std::unique(keys.begin(), keys.end()) - keys.begin(); // 唯一键的数量
        // double sparsity = static_cast<double>(unique_keys) / range_size;             // 稀疏度 = 唯一键数量 / 范围大小

        // printf("数据集稀疏度分析:\n");
        // printf("键范围: [%llu, %llu]\n", min_key, max_key);
        // printf("范围大小: %llu\n", range_size);
        // printf("唯一键数量: %llu\n", unique_keys);
        // printf("稀疏度: %.4f (%.2f%%)\n", sparsity, sparsity * 100);

        // if (sparsity < 0.1) {
        //     printf("数据集类型: 稀疏 (Sparsity > 90% empty slots)\n");
        // } else if (sparsity < 0.5) {
        //     printf("数据集类型: 中等稀疏 (Sparsity > 50% empty slots)\n");
        // } else {
        //     printf("数据集类型: 密集 (Sparsity <= 50% empty slots)\n");
        // }

        if (phase == YCSBPhase::LOAD) {
            g_config.ycsb_operations["Load"] = static_cast<int>(load_records.size());
            log_manager_->log_info("  |-> LOAD 阶段操作数", to_string(load_records.size()));
        } else {
            g_config.ycsb_operations["Run"] = static_cast<int>(run_records.size());
            log_manager_->log_info("  |-> RUN 阶段操作数", to_string(run_records.size()));
            // 分别打印插入、读取、更新的操作数，以及占比
            log_manager_->log_info("  |-> insert 操作数: " + to_string(insert_count) + ", 占比", to_string(static_cast<float>(insert_count) / static_cast<float>(run_records.size()) * 100) + "%");
            log_manager_->log_info("  |-> read 操作数: " + to_string(read_count) + ", 占比", to_string(static_cast<float>(read_count) / static_cast<float>(run_records.size()) * 100) + "%");
            log_manager_->log_info("  |-> update 操作数: " + to_string(update_count) + ", 占比", to_string(static_cast<float>(update_count) / static_cast<float>(run_records.size()) * 100) + "%");
        }

        file.close();
    }

    // 获取操作记录列表
    const vector<OperationRecord>& get_operation_records(YCSBPhase phase) const {
        return phase == YCSBPhase::LOAD ? load_records : run_records;
    }

    // 获取操作记录数量（根据阶段）
    size_t size(YCSBPhase phase) const {
        return phase == YCSBPhase::LOAD ? load_records.size() : run_records.size();
    }
};

class ErrorHandler {
public:
    static void check_memory_allocation(void* ptr, const string& allocation_type) {
        if (ptr == nullptr) {
            throw runtime_error("内存分配失败: " + allocation_type);
        }
    }

    static void validate_positive(int value, const string& param_name) {
        if (value <= 0) {
            throw invalid_argument("参数必须为正数: " + param_name + " = " + to_string(value));
        }
    }
};

class DatasetManager {
private:
    Config config_;
    uint64_t* keys_dense_ = nullptr;
    uint64_t* keys_sparse_ = nullptr;
    uint64_t* keys_clustered_ = nullptr;
    // 在 DatasetManager private 区域：
    std::vector<VariableKey> variable_keys_ = {}; // 用于存储变长键数据集
    unique_ptr<YCSBLoader> ycsb_loader_ = nullptr;
    uint64_t max_dense_key_ = 0;
    uint64_t max_sparse_key_ = 0;
    uint64_t max_clustered_key_ = 0;

    uint64_t min_dense_key_ = 0;
    uint64_t min_sparse_key_ = 0;
    uint64_t min_clustered_key_ = 1;

    void generate_dense_dataset(LogManager& log) {
        keys_dense_ = new uint64_t[config_.test_num];

        ErrorHandler::validate_positive(config_.test_num, "test_num");
        ErrorHandler::check_memory_allocation(keys_dense_, "Dense 数据集");

        log.log_section("   >>> 生成 Dense 数据集...");

        std::random_device rd;
        rng r;
        // rng_init(&r, rd(), 2);
        rng_init(&r, config_.seed, 2);

        unordered_set<uint64_t> unique_dense_keys;

        for (int i = 0; i < config_.test_num; i++) {
            keys_dense_[i] = rng_next(&r) % config_.test_num;
            max_dense_key_ = max(max_dense_key_, keys_dense_[i]);
            min_dense_key_ = min(min_dense_key_, keys_dense_[i]);
            // 记录不重复的密集数据集
            unique_dense_keys.insert(keys_dense_[i]);
        }

        // std::sort(keys_dense_, keys_dense_ + config_.test_num);
        printf("  |-> 不重复的 Dense 数据集数量: %lu\n", unique_dense_keys.size());

        log.log_info("  |-> Dense 数据集总量", to_string(config_.test_num));
        log.log_info("  |-> Dense 数据集 Key 取值区间", "[" + to_string(min_dense_key_) + ", " + to_string(max_dense_key_) + "]");

        // 打印前几行数据
        int print_count = 0;
        while (print_count < 100) {
            log.log_info("  |-> Dense 数据集 Key " + to_string(print_count), to_string(keys_dense_[print_count]));
            print_count++;
        }

        log.log_section("   >>> Dense 数据集生成完成");
    }

    void generate_sparse_dataset(LogManager& log) {
        keys_sparse_ = new uint64_t[config_.test_num];

        ErrorHandler::validate_positive(config_.test_num, "test_num");
        ErrorHandler::check_memory_allocation(keys_sparse_, "Sparse 数据集");

        log.log_section("   >>> 生成 Sparse 数据集...");

        std::random_device rd;
        rng r;
        // rng_init(&r, rd(), 2);
        rng_init(&r, config_.seed, 2);

        unordered_set<uint64_t> unique_sparse_keys;

        for (int i = 0; i < config_.test_num; i++) {
            keys_sparse_[i] = rng_next(&r);
            max_sparse_key_ = max(max_sparse_key_, keys_sparse_[i]);
            min_sparse_key_ = min(min_sparse_key_, keys_sparse_[i]);
            // 记录不重复的稀疏数据集
            unique_sparse_keys.insert(keys_sparse_[i]);
        }

        if (!config_.random) {
            std::sort(keys_sparse_, keys_sparse_ + config_.test_num);
        }

        printf("  |-> 不重复的 Sparse 数据集数量: %lu\n", unique_sparse_keys.size());

        log.log_info("  |-> Sparse 数据集总量", to_string(config_.test_num));
        log.log_info("  |-> Sparse 数据集 Key 取值区间", "[" + to_string(min_sparse_key_) + ", " + to_string(max_sparse_key_) + "]");

        // 打印前几行数据
        int print_count = 0;
        // while (print_count < config_.test_num) {
        while (print_count < 100) {
            log.log_info("  |-> Sparse 数据集 Key " + to_string(print_count), to_string(keys_sparse_[print_count]));
            print_count++;
        }

        log.log_section("   >>> Sparse 数据集生成完成");
    }

    void generate_clustered_dataset(LogManager& log) {
        keys_clustered_ = new uint64_t[config_.clustered_num];
        const uint64_t subsetCount = config_.clustered_num / config_.keys_per_subset;

        ErrorHandler::validate_positive(config_.clustered_num, "clustered_num");
        ErrorHandler::check_memory_allocation(keys_clustered_, "Clustered 数据集");

        log.log_section("   >>> 生成 Clustered 数据集...");

        rng r;
        rng_init(&r, config_.seed, 2);

        // 生成子集起始键（确保不重叠）
        vector<uint64_t> subsetStarts(subsetCount);
        const uint64_t maxStartKey = UINT64_MAX - config_.keys_per_subset;
        const uint64_t subsetSpace = maxStartKey / subsetCount;

        // 存储子集映射信息
        vector<pair<uint64_t, uint64_t>> subsetMapping;
        subsetMapping.reserve(subsetCount);

        for (uint64_t i = 0; i < subsetCount; i++) {
            uint64_t base = i * subsetSpace;
            uint64_t max_offset = subsetSpace - config_.keys_per_subset;

            if (max_offset > 0) {
                uint64_t offset = rng_next(&r) % max_offset;
                uint64_t start_key = base + offset;
                subsetStarts[i] = start_key;
                subsetMapping.push_back({start_key, start_key + config_.keys_per_subset - 1});
            } else {
                // 如果空间不足，使用最小偏移
                uint64_t start_key = base;
                subsetStarts[i] = start_key;
                subsetMapping.push_back({start_key, start_key + config_.keys_per_subset - 1});
            }
        }

        unordered_set<uint64_t> unique_clustered_keys;

        // 生成所有键（随机化聚集分布）
        std::vector<uint64_t> all_keys;

        // 先收集所有键
        for (uint64_t i = 0; i < subsetCount; i++) {
            uint64_t start_key = subsetStarts[i];
            for (uint64_t j = 0; j < config_.keys_per_subset; j++) {
                all_keys.push_back(start_key + j);
            }
        }

        // 随机打乱所有键
        std::random_device rd;
        std::mt19937 g(rd());
        std::shuffle(all_keys.begin(), all_keys.end(), g);

        // 将打乱后的键分配给keys_clustered_
        uint64_t key_index = 0;
        for (uint64_t key : all_keys) {
            keys_clustered_[key_index++] = key;
            max_clustered_key_ = max(max_clustered_key_, key);
            min_clustered_key_ = min(min_clustered_key_, key);
            // 记录不重复的Clustered数据集
            unique_clustered_keys.insert(key);
        }

        printf("  |-> 不重复的 Clustered 数据集数量: %lu\n", unique_clustered_keys.size());

        // 输出前5个子集的映射信息
        log.log_info("  |-> Clustered 数据集总量", to_string(config_.clustered_num));
        log.log_info("  |-> Clustered 数据集子集数量", to_string(config_.keys_per_subset));
        log.log_info("  |-> Clustered 数据集每个子集数据量", to_string(subsetCount));
        log.log_info("  |-> Clustered 数据集最大 Key", to_string(max_clustered_key_));
        log.log_info("  |-> Clustered 数据集 Key 取值区间", "[" + to_string(min_clustered_key_) + ", " + to_string(max_clustered_key_) + "]");
        log.log_info("  |-> Clustered 数据集子集映射区间", "");
        // for (uint64_t i = 0; i < subsetCount; i++) {
        //     log.log("  |--> 子集 " + to_string(i) + ": [" + to_string(subsetMapping[i].first) + " - " + to_string(subsetMapping[i].second) + "]");
        // }

        // 打印前几行数据
        // int print_count = 0;
        // while (print_count < config_.clustered_num) {
        //     log.log_info("  |-> Clustered 数据集 Key " + to_string(print_count), to_string(keys_clustered_[print_count]));
        //     print_count++;
        // }

        log.log_section("   >>> Clustered 数据集生成完成");
    }

    // 新增成员函数：生成变长键数据集
    void generate_variable_sized_dataset(LogManager& log) {
        log.log_section("   >>> 生成 Variable-sized 数据集...");
        log.log_info("  |-> 变长键范围", "[" + to_string(config_.test_variable_min) + ", " + to_string(config_.test_variable_max) + "]");

        // 清空旧数据
        variable_keys_.clear();
        // 预分配内存，避免频繁扩容
        variable_keys_.reserve(config_.test_num);

        std::random_device rd;
        std::mt19937 gen(rd());
        // 随机长度分布：在 [8, 1024] 范围内均匀分布
        std::uniform_int_distribution<> len_dis(8, 1024);

        for (int i = 0; i < config_.test_num; ++i) {
            uint64_t random_length = len_dis(gen);
            // 直接在 vector 的预留空间中构造对象 (完美转发)
            // 这里会调用 VariableKey(uint64_t len) 构造函数
            variable_keys_.emplace_back(random_length);
        }

        // 根据键长度排序
        std::sort(variable_keys_.begin(), variable_keys_.end(), [](const VariableKey& a, const VariableKey& b) {
            return a.length < b.length;
        });

        // 打印前10条变长键
        for (int i = 0; i < 100; ++i) {
            char buf[128];

            // 获取当前 Key 的指针和长度
            const char* key_data = variable_keys_[i].data.get();
            uint64_t key_len = variable_keys_[i].length;

            if (key_data == nullptr) {
                snprintf(buf, sizeof(buf), "Len = %llu B, Content = NULL", (unsigned long long)key_len);
            } else if (key_len <= 30) {
                // 构造一个临时截断字符串（加 \0 结尾以便 %s 打印）
                std::string temp_str(key_data, key_len);
                snprintf(buf, sizeof(buf), "Len = %llu B, Content = %s", (unsigned long long)key_len, temp_str.c_str());
            } else {
                // 长度大于 30，只打印前 30 个字符
                char preview[32];
                std::string temp_preview(key_data, 30); // 取前30个字符
                snprintf(buf, sizeof(buf), "Len = %llu B, Content = %.30s...", (unsigned long long)key_len, temp_preview.c_str());
            }
            log.log_info("  |-> Variable-sized 数据集 Key " + to_string(i), buf);
        }

        log.log_info("  |-> Variable-sized 数据集总量", to_string(variable_keys_.size()));
        log.log_section("   >>> Variable-sized 数据集生成完成");
    }

    void load_ycsb_dataset(LogManager& log) {
        if (config_.ycsb_data_file.empty()) {
            return;
        }

        log.log_section("   >>> 加载 YCSB 数据集...");

        try {
            ycsb_loader_ = make_unique<YCSBLoader>(config_.ycsb_data_file, &log);
        } catch (const exception& e) {
            log.log_error("  |-> 加载 YCSB 数据集失败: " + string(e.what()));
            throw;
        }

        log.log_section("   >>> YCSB 数据集加载完成");
    }

public:
    DatasetManager(const Config& config, LogManager& log) : config_(config) {
        log.log_section("   >>> 准备测试数据集...");

        if (config_.test_ycsb) {
            load_ycsb_dataset(log);
        } else {
            if (config_.test_dense) {
                generate_dense_dataset(log);
            }
            if (config_.test_sparse) {
                generate_sparse_dataset(log);
            }
            if (config_.test_clustered) {
                generate_clustered_dataset(log);
            }
            if (config_.test_variable) {
                generate_variable_sized_dataset(log);
            }
        }

        log.log_section("   >>> 数据集准备完成");
        log.log_Line_break();
    }

    ~DatasetManager() {
        delete[] keys_dense_;
        delete[] keys_sparse_;
        delete[] keys_clustered_;
    }

    uint64_t* dense_keys() {
        return keys_dense_;
    }
    uint64_t* sparse_keys() {
        return keys_sparse_;
    }
    uint64_t* clustered_keys() {
        return keys_clustered_;
    }
    std::vector<VariableKey>& variable_keys() {
        return variable_keys_;
    }

    YCSBLoader* ycsb_loader() {
        return ycsb_loader_.get();
    }

    uint64_t max_dense_key() const {
        return max_dense_key_;
    }
    uint64_t max_sparse_key() const {
        return max_sparse_key_;
    }
    uint64_t max_clustered_key() const {
        return max_clustered_key_;
    }

    uint64_t min_dense_key() const {
        return min_dense_key_;
    }
    uint64_t min_sparse_key() const {
        return min_sparse_key_;
    }
    uint64_t min_clustered_key() const {
        return min_clustered_key_;
    }

    int dense_count() const {
        return config_.test_dense ? config_.test_num : 0;
    }
    int sparse_count() const {
        return config_.test_sparse ? config_.test_num : 0;
    }
    int clustered_count() const {
        return config_.test_clustered ? config_.clustered_num : 0;
    }
    int variable_count() const {
        return config_.test_variable ? variable_keys_.size() : 0;
    }
    int ycsb_count(YCSBPhase phase) const {
        return config_.test_ycsb && ycsb_loader_ ? static_cast<int>(ycsb_loader_->size(phase)) : 0;
    }
};

class TestTimer {
private:
    chrono::steady_clock::time_point start_time_;
    string test_name_;
    LogManager& log_;
    PerformanceMetrics::OperationMetrics& metrics_;

public:
    TestTimer(const string& name, LogManager& log, PerformanceMetrics::OperationMetrics& metrics) : test_name_(name), log_(log), metrics_(metrics) {
        log_.log_section("   >>> 开始测试索引结构: " + test_name_);
        start_time_ = chrono::steady_clock::now();
    }

    ~TestTimer() {
        auto end_time = chrono::steady_clock::now();
        auto duration = chrono::duration_cast<chrono::microseconds>(end_time - start_time_);

        metrics_.latency = duration.count() / 1000000.0; // 转换为秒
        metrics_.total_time_us = duration.count();

        log_.log_info("  |-> " + test_name_ + " 测试完成，总耗时", to_string(metrics_.latency) + " 秒");
    }
};

class DataStructureTester {
public:
    struct TestResult {
        string name;
        PerformanceMetrics metrics;
    };

    DataStructureTester(const string& name, LogManager& log) : name_(name), log_(log) {}

    virtual ~DataStructureTester() {}

    // 获取测试器名称
    const string& get_name() const {
        return name_;
    }

    virtual TestResult run_test(DatasetManager& dataset) {
        TestResult result{name_, PerformanceMetrics()};
        auto& metrics = result.metrics;

        // 创建一个临时的 OperationMetrics 用于整体测试计时
        PerformanceMetrics::OperationMetrics overall_metrics;
        TestTimer timer(name_, log_, overall_metrics);

        // 初始化数据结构
        log_.log_section("   >>> 初始化索引结构: " + name_);
        if (!initialize()) {
            log_.log_section("   >>> 初始化索引结构: " + name_ + " 失败");
            return result;
        }
        log_.log_section("   >>> 初始化索引结构完成 ");

        if (g_config.test_ycsb) {
            test_ycsb_dataset(dataset.ycsb_loader(), dataset.ycsb_count(YCSBPhase::LOAD), dataset.ycsb_count(YCSBPhase::RUN), metrics);
        } else {
            if (g_config.test_dense) {
                test_dataset(dataset.dense_keys(), dataset.dense_count(),
                             dataset.min_dense_key(), dataset.max_dense_key(), PerformanceMetrics::DENSE, metrics);
            }

            if (g_config.test_sparse) {
                test_dataset(dataset.sparse_keys(), dataset.sparse_count(),
                             dataset.min_sparse_key(), dataset.max_sparse_key(), PerformanceMetrics::SPARSE, metrics);
            }

            if (g_config.test_clustered) {
                test_dataset(dataset.clustered_keys(), dataset.clustered_count(),
                             dataset.min_clustered_key(), dataset.max_clustered_key(), PerformanceMetrics::CLUSTERED, metrics);
            }

            if (g_config.test_variable) {
                test_variable_dataset(dataset.variable_keys(), dataset.variable_count(), PerformanceMetrics::VARIABLE, metrics);
            }
        }

        log_.log_section("   >>> 索引结构 " + name_ + " 测试完成");

        return result;
    }

protected:
    string name_;
    LogManager& log_;

    virtual bool initialize() = 0;
    virtual void test_insert(uint64_t* keys, int count, PerformanceMetrics::DatasetType type, double& time_us) = 0;
    virtual void test_search(uint64_t* keys, int count, PerformanceMetrics::DatasetType type, double& time_us) = 0;
    virtual void test_variable_insert(std::vector<VariableKey>& keys, int count, PerformanceMetrics::DatasetType type, double& time_us) = 0;
    virtual void test_variable_search(std::vector<VariableKey>& keys, int count, PerformanceMetrics::DatasetType type, double& time_us) = 0;
    virtual void test_range(uint64_t start, uint64_t end, PerformanceMetrics::DatasetType type, double& time_us, uint64_t& result_count) = 0;
    virtual void test_ycsb_operations(YCSBLoader* loader, int count, YCSBPhase phase, PerformanceMetrics::DatasetType ycsb_type, double& time_us) = 0;
    virtual uint64_t get_memory_usage(PerformanceMetrics::DatasetType type) = 0;
    virtual void* get_instance(PerformanceMetrics::DatasetType type) = 0;

    void test_dataset(uint64_t* keys, int count, uint64_t min_key, uint64_t max_key,
                      PerformanceMetrics::DatasetType type, PerformanceMetrics& metrics) {
        if (!keys || count <= 0)
            return;

        string display_name = dataset_display_name(type);

        log_.log_section("   >>> 开始测试 " + display_name + " 数据集");
        log_.log_info("  |-> " + display_name + " 数据集总量 ", to_string(count));
        log_.log_info("  |-> " + display_name + " 数据集最大 key ", to_string(max_key));

        // 测试插入;
        test_operation("insert", keys, nullptr, count, type, YCSBPhase::NULL_PHASE, PerformanceMetrics::INSERT, metrics);

        // 测试范围查询
        // test_range_queries(min_key, max_key, type, metrics);

        // 测试查询
        // test_operation("search", keys, nullptr, count, type, YCSBPhase::NULL_PHASE, PerformanceMetrics::SEARCH, metrics);

        // 分析内存使用
        // analyze_memory(type, metrics);
    }

    void test_variable_dataset(std::vector<VariableKey>& keys, int count, PerformanceMetrics::DatasetType type, PerformanceMetrics& metrics) {
        if (keys.empty() || count <= 0)
            return;

        string display_name = dataset_display_name(type);

        log_.log_section("   >>> 开始测试 " + display_name + " 数据集");
        log_.log_info("  |-> " + display_name + " 数据集总量 ", to_string(count));

        // 测试插入;
        test_variable_operation("insert", keys, count, type, PerformanceMetrics::INSERT, metrics);

        // 测试查询
        test_variable_operation("search", keys, count, type, PerformanceMetrics::SEARCH, metrics);
    }

    void test_variable_operation(const string& op_name, std::vector<VariableKey>& keys, int count, PerformanceMetrics::DatasetType dataset_type, PerformanceMetrics::OperationType op_type, PerformanceMetrics& metrics) {
        string dataset_name = dataset_display_name(dataset_type);
        log_.log_section("   >>> " + dataset_name + " 数据集 " + op_name + " 测试");

        uint64_t result_count = 0;
        double time_us = 0.0;

        // 根据是插入还是搜索，调用不同的测试函数
        if (op_type == PerformanceMetrics::INSERT) {
            test_variable_insert(keys, count, dataset_type, time_us);
        } else if (op_type == PerformanceMetrics::SEARCH) {
            test_variable_search(keys, count, dataset_type, time_us);
        }

        auto& op_metrics = metrics.get(dataset_type, op_type);

        op_metrics.total_time_us = time_us;
        op_metrics.latency = time_us / 1000000.0;

        if (count > 0) {
            op_metrics.avg_time_us = time_us / count;
        } else {
            op_metrics.avg_time_us = 0.0;
            log_.log_warning("操作数量为0, 无法计算平均时间");
        }
        op_metrics.operations_count = count;
        op_metrics.calculate_throughput(count);

        log_.log_info("  |-> 总延迟", to_string_fixed(op_metrics.latency, 6) + " s");
        log_.log_info("  |-> 平均每操作时间", to_string_fixed(op_metrics.avg_time_us, 6) + " μs/op");
        log_.log_info("  |-> 吞吐量", to_string_fixed(op_metrics.throughput, 6) + " MOPS");
    }

    void test_ycsb_dataset(YCSBLoader* loader, int load_count, int run_count, PerformanceMetrics& metrics) {
        // if (!loader || load_count <= 0 || run_count <= 0) {
        if (!loader || load_count <= 0) {
            log_.log_warning("YCSB 数据集为空或操作数无效");
            return;
        }

        PerformanceMetrics::DatasetType ycsb_type = get_ycsb_dataset_type(g_config.ycsb_workload);
        string workload_name = "YCSB_" + g_config.ycsb_workload;

        log_.log_section("   >>> 开始测试 " + workload_name + " 数据集...");
        log_.log_info("  |-> " + workload_name + " 数据集总量 Load", to_string(loader->size(YCSBPhase::LOAD)));
        log_.log_info("  |-> " + workload_name + " 数据集总量 Run", to_string(loader->size(YCSBPhase::RUN)));
        log_.log_info("  |-> " + workload_name + " 线程数", to_string(g_config.ycsb_threads));

        // 预加载阶段 Load 阶段 - 计算整个工作负载的总体吞吐量
        test_operation(workload_name + " Load", nullptr, loader, load_count, ycsb_type, YCSBPhase::LOAD, PerformanceMetrics::YCSB_LOAD, metrics);

        // 测试 YCSB 测试 - 计算整个工作负载的总体吞吐量
        // test_operation(g_config.ycsb_workload + " workload", nullptr, loader, run_count, ycsb_type, YCSBPhase::RUN, PerformanceMetrics::YCSB_RUN, metrics);

        // 分析内存使用
        analyze_memory(ycsb_type, metrics);
    }

    void test_operation(const string& op_name, uint64_t* keys, YCSBLoader* loader, int count,
                        PerformanceMetrics::DatasetType dataset_type, YCSBPhase phase,
                        PerformanceMetrics::OperationType op_type, PerformanceMetrics& metrics) {
        string dataset_name = dataset_display_name(dataset_type);

        if (op_type == PerformanceMetrics::YCSB_LOAD) {
            log_.log_section("   >>> " + dataset_name + " 数据集预加载阶段(Load)");
        } else {
            log_.log_section("   >>> " + dataset_name + " 数据集 " + op_name + " 测试");
        }

        uint64_t result_count = 0;
        double time_us = 0.0;

        // 根据是插入还是搜索，调用不同的测试函数
        if (op_type == PerformanceMetrics::INSERT) {
            test_insert(keys, count, dataset_type, time_us);
        } else if (op_type == PerformanceMetrics::SEARCH) {
            test_search(keys, count, dataset_type, time_us);
        } else {
            test_ycsb_operations(loader, count, phase, dataset_type, time_us);
        }

        auto& op_metrics = metrics.get(dataset_type, op_type);

        op_metrics.total_time_us = time_us;
        op_metrics.latency = time_us / 1000000.0;

        if (count > 0) {
            op_metrics.avg_time_us = time_us / count;
        } else {
            op_metrics.avg_time_us = 0.0;
            log_.log_warning("操作数量为0, 无法计算平均时间");
        }
        op_metrics.operations_count = count;
        op_metrics.calculate_throughput(count);

        log_.log_info("  |-> 总延迟", to_string_fixed(op_metrics.latency, 6) + " s");
        log_.log_info("  |-> 平均每操作时间", to_string_fixed(op_metrics.avg_time_us, 6) + " μs/op");
        log_.log_info("  |-> 吞吐量", to_string_fixed(op_metrics.throughput, 6) + " MOPS");
    }

    void test_range_queries(uint64_t min_key, uint64_t max_key, PerformanceMetrics::DatasetType type,
                            PerformanceMetrics& metrics) {
        string dataset_name = dataset_display_name(type);
        vector<pair<double, PerformanceMetrics::OperationType>> selectivities = {
          //   {0.001, PerformanceMetrics::RANGE_001},
          //   {0.005, PerformanceMetrics::RANGE_005},
          //   {0.01, PerformanceMetrics::RANGE_01},

          //   {0.1, PerformanceMetrics::RANGE_001},
          //   {0.25, PerformanceMetrics::RANGE_001},
          //   {0.5, PerformanceMetrics::RANGE_005},
          //   {0.75, PerformanceMetrics::RANGE_01},
          //   {1.0, PerformanceMetrics::RANGE_1},
        };

        selectivities.push_back({g_config.selectivity, PerformanceMetrics::RANGE_001});

        for (auto& [selectivity, op_type] : selectivities) {
            uint64_t end = 0;
            if (selectivity == 1.0) {
                end = max_key;
            } else {
                end = static_cast<uint64_t>(max_key * selectivity);
            }

            if (end < 1)
                end = 1;

            string selectivity_str = to_string_fixed(selectivity * 100, 1);
            log_.log_section("   >>> " + dataset_name + " 数据集 " + selectivity_str + "% 选择性范围查询");

            uint64_t result_count = 0;
            double time_us = 0.0;

            test_range(min_key, end, type, time_us, result_count);

            log_.log_info("  |-> 查询时间 time_us", to_string_fixed(time_us, 6) + " μs");

            if (result_count == 0) {
                result_count = static_cast<uint64_t>(g_config.test_num * selectivity);
                if (result_count < 1)
                    result_count = 1;
            }

            auto& op_metrics = metrics.get(type, op_type);
            op_metrics.total_time_us = time_us;
            op_metrics.latency = time_us / 1000000.0;

            if (result_count > 0) {
                op_metrics.avg_time_us = time_us / result_count;
            } else {
                op_metrics.avg_time_us = 0.0;
                log_.log_warning("查询结果数量为0，无法计算平均时间");
            }
            op_metrics.calculate_range_throughput(result_count);

            log_.log_info("  |-> 查询范围", "[1, " + to_string(end) + "]");
            log_.log_info("  |-> 查询结果数量", to_string(result_count));
            log_.log_info("  |-> 总延迟", to_string_fixed(op_metrics.latency, 6) + "s");
            log_.log_info("  |-> 平均每结果时间", to_string_fixed(op_metrics.avg_time_us, 6) + " μs/op");
            log_.log_info("  |-> 吞吐量", to_string_fixed(op_metrics.throughput, 6) + " KQPS");
        }
    }

    void analyze_memory(PerformanceMetrics::DatasetType type, PerformanceMetrics& metrics) {
        string dataset_name = dataset_display_name(type);
        log_.log_section("   >>> " + dataset_name + " 数据集内存使用分析");

        // 获取内存使用量（字节）- 与main.cpp保持一致
        uint64_t memory_bytes = get_memory_usage(type);
        auto& memory = metrics.get_memory(type);
        memory.bytes = memory_bytes;

        // 计算空间利用率 - 与main.cpp保持一致
        const uint64_t key_value_size = 16; // 8字节键 + 8字节值
        uint64_t effective_data_size = 0;

        // 根据数据集类型确定数据大小
        if (type == PerformanceMetrics::DENSE || type == PerformanceMetrics::SPARSE) {
            effective_data_size = g_config.test_num * key_value_size;
        } else if (type == PerformanceMetrics::CLUSTERED) {
            effective_data_size = g_config.clustered_num * key_value_size;
        }

        // 计算空间利用率（百分比）- 与main.cpp保持一致
        double space_utilization = 0.0;
        if (memory_bytes > 0) {
            space_utilization = (static_cast<double>(effective_data_size) / memory_bytes) * 100.0;
        }

        memory.utilization_percent = space_utilization;

        double memory_gb = static_cast<double>(memory_bytes) / (1024.0 * 1024.0 * 1024.0);

        log_.log_info("  |-> 内存使用量(字节)", to_string(memory_bytes) + " bytes");
        log_.log_info("  |-> 内存使用量(MB)", to_string_fixed(memory.mb(), 6) + " MB");
        log_.log_info("  |-> 内存使用量(GB)", to_string_fixed(memory_gb, 6) + " GB");
        if (type >= PerformanceMetrics::DENSE && type <= PerformanceMetrics::CLUSTERED) {
            log_.log_info("  |-> 有效数据大小", to_string(effective_data_size) + " bytes");
            log_.log_info("  |-> 空间利用率", to_string_fixed(space_utilization, 2) + "%");
        }
    }

    template <typename Func>
    double measure_time(Func func) {
        sleep(1); // 确保系统稳定

        timeval start, end;
        gettimeofday(&start, NULL);
        func();
        gettimeofday(&end, NULL);

        return (end.tv_sec - start.tv_sec) * 1000000.0 + (end.tv_usec - start.tv_usec);
    }

    string to_string_fixed(double value, int precision) {
        ostringstream oss;
        oss << fixed << setprecision(precision) << value;
        return oss.str();
    }

    PerformanceMetrics::DatasetType get_ycsb_dataset_type(const string& workload) {
        if (workload == "a" || workload == "A") {
            return PerformanceMetrics::YCSB_WORKLOAD_A;
        } else if (workload == "b" || workload == "B") {
            return PerformanceMetrics::YCSB_WORKLOAD_B;
        } else if (workload == "c" || workload == "C") {
            return PerformanceMetrics::YCSB_WORKLOAD_C;
        } else if (workload == "d" || workload == "D") {
            return PerformanceMetrics::YCSB_WORKLOAD_D;
        } else if (workload == "e" || workload == "E") {
            return PerformanceMetrics::YCSB_WORKLOAD_E;
        } else if (workload == "f" || workload == "F") {
            return PerformanceMetrics::YCSB_WORKLOAD_F;
        }
    }

    void show_progress(int current, int total, const string& operation) {
        if (current % g_config.per_ops == 0 || current == total) {
            const int bar_width = 50;
            float progress = static_cast<float>(current) / total;
            int pos = static_cast<int>(bar_width * progress);

            ostringstream oss;
            oss << "  " << operation << "进度: [";
            for (int i = 0; i < bar_width; ++i) {
                if (i < pos)
                    oss << "█";
                else if (i == pos)
                    oss << "▶";
                else
                    oss << "░";
            }
            oss << "] " << int(progress * 100.0) << "% ("
                << current << "/" << total << ")\n";

            log_.log_raw(oss.str());
        }
    }
};

unordered_set<uint64_t> unique_ROERT_keys;
unordered_set<uint64_t> unique_ERT_keys;
unordered_set<char*> unique_FASTFAIR_keys;
unordered_set<void*> unique_LBTree_keys;
unordered_set<uint64_t> unique_WORT_keys;
unordered_set<uint64_t> unique_WOART_keys;
unordered_set<uint64_t> unique_ROART_keys;

// ==================== 具体数据结构测试器实现 ====================
class ROERTTester : public DataStructureTester {
private:
    ROERT* roert_dense_ = nullptr;
    ROERT* roert_sparse_ = nullptr;
    ROERT* roert_clustered_ = nullptr;
    ROERT* roert_variable_ = nullptr;
    ROERT* roert_ycsb_ = nullptr;

public:
    ROERTTester(LogManager& log) : DataStructureTester("ROERT", log) {}

protected:
    bool initialize() override {
        bool success = true;

        if (g_config.test_ycsb) {
            roert_ycsb_ = NewROERT();
            if (!roert_ycsb_) {
                log_.log_error("YCSB ROERT 实例创建失败");
                success = false;
            } else {
                log_.log_info("  |-> 创建 YCSB 数据集实例对象", "roert_ycsb_");
            }
        } else {
            if (g_config.test_dense) {
                roert_dense_ = NewROERT();
                if (!roert_dense_) {
                    log_.log_error("Dense ROERT 实例创建失败");
                    success = false;
                } else {
                    log_.log_info("  |-> 创建 Dense 数据集实例对象", "roert_dense_");
                }
            }

            if (g_config.test_sparse) {
                roert_sparse_ = NewROERT();
                if (!roert_sparse_) {
                    log_.log_error("Sparse ROERT 实例创建失败");
                    success = false;
                } else {
                    log_.log_info("  |-> 创建 Sparse 数据集实例对象", "roert_sparse_");
                }
            }

            if (g_config.test_clustered) {
                roert_clustered_ = NewROERT();
                if (!roert_clustered_) {
                    log_.log_error("Clustered ROERT 实例创建失败");
                    success = false;
                } else {
                    log_.log_info("  |-> 创建 Clustered 数据集实例对象", "roert_clustered_");
                }
            }

            if (g_config.test_variable) {
                roert_variable_ = NewROERT();
                if (!roert_variable_) {
                    log_.log_error("Variable ROERT 实例创建失败");
                    success = false;
                } else {
                    log_.log_info("  |-> 创建 Variable 数据集实例对象", "roert_variable_");
                }
            }
        }
        return success;
    }

    void test_insert(uint64_t* keys, int count, PerformanceMetrics::DatasetType type, double& time_us) override {
        ROERT* roert = static_cast<ROERT*>(get_instance(type));

        time_us = measure_time([&]() {
            for (int i = 0; i < count; i++) {
                roert->insert(keys[i], i);
                // show_progress(i + 1, count, "插入");
            }
        });

        // int num = count / 10;
        // for (int i = 0, j = num * i; i < 10; i++) {
        //     time_us = measure_time([&]() {
        //         for (int k = 0; k < num; k++) {
        //             roert->insert(keys[j], j++);
        //             // show_progress(j + 1, count, "插入");
        //         }
        //     });
        //     // printf("  |-> ROERT %s insert 执行第 %d 个 %llu 次操作耗时: %.f ns, 吞吐量: %.6f Mops, 目录扩展次数: %lu, 段分裂次数: %lu\n",
        //     //        dataset_display_name(type).c_str(), i + 1, num, time_us * 1000,
        //     //        num / (time_us / 1000000.0) / 1000000.0,
        //     //        roert_insert_directory_grow_time_.size(),
        //     //        roert_insert_segment_split_time_.size());
        //     // roert_insert_directory_grow_time_.clear();
        //     // roert_insert_segment_split_time_.clear();
        //     printf("  |-> ROERT %s insert 执行第 %d 个 %llu 次操作耗时: %.f ns, 吞吐量: %.6f Mops\n",
        //            dataset_display_name(type).c_str(), i + 1, num, time_us * 1000,
        //            num / (time_us / 1000000.0) / 1000000.0);
        // }

        printf("  |-> ROERT %s insert %llu 操作总耗时: %.f ns\n",
               dataset_display_name(type).c_str(), count, time_us * 1000);
        uint64_t total_update_latency = std::accumulate(roert->per_insert_node_update_latency_.begin(), roert->per_insert_node_update_latency_.end(), 0ULL)
                                        + std::accumulate(roert_per_insert_node_update_latency_.begin(), roert_per_insert_node_update_latency_.end(), 0ULL);
        uint64_t total_split_grow_latency = std::accumulate(roert->per_insert_node_split_latency_.begin(), roert->per_insert_node_split_latency_.end(), 0ULL)
                                            + std::accumulate(roert->per_insert_node_grow_latency_.begin(), roert->per_insert_node_grow_latency_.end(), 0ULL)
                                            + std::accumulate(roert_insert_segment_split_time_.begin(), roert_insert_segment_split_time_.end(), 0ULL)
                                            + std::accumulate(roert_insert_directory_grow_time_.begin(), roert_insert_directory_grow_time_.end(), 0ULL);
        uint64_t total_traversal_latency = time_us * 1000 - total_update_latency - total_split_grow_latency;
        printf("  |-> ROERT %s insert 每次插入平均耗时: %.f ns, 树遍历延迟平均延迟: %llu ns, 节点分裂+节点扩容的平均延迟: %llu ns,  节点更新的平均延迟: %llu ns\n",
               dataset_display_name(type).c_str(),
               time_us * 1000 / count,
               total_traversal_latency / count,
               total_split_grow_latency / count,
               total_update_latency / count);
        printf("  |-> ROERT 目录扩展次数: %lu - 平均耗时: %lu ns, 段分裂次数: %llu - 平均耗时: %lu ns\n",
               roert_insert_directory_grow_time_.size(),
               std::accumulate(roert_insert_directory_grow_time_.begin(), roert_insert_directory_grow_time_.end(), 0ULL) / (roert_insert_directory_grow_time_.size() ? roert_insert_directory_grow_time_.size() : 1),
               roert_insert_segment_split_time_.size(),
               std::accumulate(roert_insert_segment_split_time_.begin(), roert_insert_segment_split_time_.end(), 0ULL) / (roert_insert_segment_split_time_.size() ? roert_insert_segment_split_time_.size() : 1));

        // printf("  |-> ROERT %s insert %llu 操作总树遍历延迟: %llu ns\n",
        //        //    dataset_display_name(type).c_str(), count, std::accumulate(roert->per_insert_node_traversal_latency_.begin(), roert->per_insert_node_traversal_latency_.end(), 0ULL));
        //        dataset_display_name(type).c_str(), count, std::accumulate(roert->per_insert_node_traversal_latency_.begin(), roert->per_insert_node_traversal_latency_.end(), 0ULL) / (roert->per_insert_node_traversal_latency_.size() ? roert->per_insert_node_traversal_latency_.size() : 1));
    }

    void test_search(uint64_t* keys, int count, PerformanceMetrics::DatasetType type, double& time_us) override {
        ROERT* roert = static_cast<ROERT*>(get_instance(type));

        time_us = measure_time([&]() {
            for (int i = 0; i < count; i++) {
                roert->search(keys[i]);
                // unique_ROERT_keys.insert(roert->search(keys[i]));
                // show_progress(i + 1, count, "查询");
            }
        });

        printf("  |-> ROERT %s search 操作返回的唯一值数量: %llu, 总耗时: %.f ns\n",
               dataset_display_name(type).c_str(), unique_ROERT_keys.size(), time_us * 1000);

        uint64_t total_traversal_latency = static_cast<uint64_t>(time_us * 1000)
                                           - std::accumulate(roert->per_search_per_node_latency_.begin(), roert->per_search_per_node_latency_.end(), 0ULL);

        printf("  |-> ROERT %s search 操作访问节点 %llu, 每次查询平均耗时: %.f ns, 树遍历延迟平均延迟: %llu ns, 节点访问平均延迟: %llu ns, 访问每个节点的平均延迟: %llu ns, 平均访问节点的数量: %.3f\n",
               dataset_display_name(type).c_str(),
               roert->per_search_per_node_latency_.size(),
               time_us * 1000 / count,
               total_traversal_latency / count,
               std::accumulate(roert->per_search_per_node_latency_.begin(), roert->per_search_per_node_latency_.end(), 0ULL) / count,
               std::accumulate(roert->per_search_per_node_latency_.begin(), roert->per_search_per_node_latency_.end(), 0ULL) / (roert->per_search_per_node_latency_.size() ? roert->per_search_per_node_latency_.size() : 1),
               static_cast<double>(roert->per_search_per_node_latency_.size()) / count);
    }

    void test_variable_insert(std::vector<VariableKey>& keys, int count, PerformanceMetrics::DatasetType type, double& time_us) {
        ROERT* roert = static_cast<ROERT*>(get_instance(type));

        // time_us = measure_time([&]() {
        //     for (int i = 0; i < count; i++) {
        //         roert->insert(keys[i], i);
        //         // show_progress(i + 1, count, "插入");
        //     }
        // });
        char buf[128];

        // 获取当前 Key 的指针和长度
        const char* key_data = keys[1].data.get();
        uint64_t key_len = keys[1].length;
        std::string temp_str(key_data, key_len);
        snprintf(buf, sizeof(buf), "Len = %llu B, Content = %s", (unsigned long long)key_len, temp_str.c_str());
        printf("%s\n", buf);
    }

    void test_variable_search(std::vector<VariableKey>& keys, int count, PerformanceMetrics::DatasetType type, double& time_us) {
        ROERT* roert = static_cast<ROERT*>(get_instance(type));

        // time_us = measure_time([&]() {
        //     for (int i = 0; i < count; i++) {
        //         roert->search(keys[i]);
        //         // show_progress(i + 1, count, "查询");
        //     }
        // });
    }

    void test_range(uint64_t start, uint64_t end, PerformanceMetrics::DatasetType type, double& time_us, uint64_t& result_count) override {
        ROERT* roert = static_cast<ROERT*>(get_instance(type));

        roert->range_segment_scan_latency_.clear();

        time_us = measure_time([&]() {
            result_count = roert->lookupRange(start, end).size();
        });

        printf("  |-> ROERT %s 查询范围 [%llu, %llu] 共 %llu 个键, 总耗时: %.f ns\n",
               dataset_display_name(type).c_str(), start, end, result_count, time_us * 1000);

        printf("  |-> ROERT %s range 操作树遍历延迟: %llu ns, 段扫描总延迟: %llu ns, 平均段扫描延迟: %llu ns, 访问段的数量: %llu\n",
               dataset_display_name(type).c_str(),
               static_cast<uint64_t>(time_us * 1000) - std::accumulate(roert->range_segment_scan_latency_.begin(), roert->range_segment_scan_latency_.end(), 0ULL),
               std::accumulate(roert->range_segment_scan_latency_.begin(), roert->range_segment_scan_latency_.end(), 0ULL),
               std::accumulate(roert->range_segment_scan_latency_.begin(), roert->range_segment_scan_latency_.end(), 0ULL) / (roert->range_segment_scan_latency_.size() ? roert->range_segment_scan_latency_.size() : 1),
               roert->range_segment_scan_latency_.size());
    }

    void test_ycsb_operations(YCSBLoader* loader, int count, YCSBPhase phase, PerformanceMetrics::DatasetType ycsb_type, double& time_us) override {
        ROERT* roert = static_cast<ROERT*>(get_instance(ycsb_type));
        auto& operation_records = loader->get_operation_records(phase);

        time_us = measure_time([&]() {
            for (int i = 0; i < count; i++) {
                if (operation_records[i].op_char == 'r') {
                    roert->search(operation_records[i].key);
                } else if (operation_records[i].op_char == 'u' || operation_records[i].op_char == 'i') {
                    roert->insert(operation_records[i].key, operation_records[i].value);
                }

                // if (phase == YCSBPhase::LOAD) {
                //     show_progress(i + 1, count, "YCSB 预加载");
                // } else {
                //     show_progress(i + 1, count, "YCSB 测试");
                // }
            }
        });
    }

    uint64_t get_memory_usage(PerformanceMetrics::DatasetType type) override {
        ROERT* roert = static_cast<ROERT*>(get_instance(type));

        return roert ? roert->memory_profile(nullptr) : 0;
    }

    void* get_instance(PerformanceMetrics::DatasetType type) override {
        switch (type) {
            case PerformanceMetrics::DENSE:
                return roert_dense_;
            case PerformanceMetrics::SPARSE:
                return roert_sparse_;
            case PerformanceMetrics::CLUSTERED:
                return roert_clustered_;
            case PerformanceMetrics::VARIABLE:
                return roert_variable_;
            case PerformanceMetrics::YCSB_WORKLOAD_A:
            case PerformanceMetrics::YCSB_WORKLOAD_B:
            case PerformanceMetrics::YCSB_WORKLOAD_C:
            case PerformanceMetrics::YCSB_WORKLOAD_D:
            case PerformanceMetrics::YCSB_WORKLOAD_E:
            case PerformanceMetrics::YCSB_WORKLOAD_F:
                return roert_ycsb_;
            default:
                return nullptr;
        }
    }
};

class ERTTester : public DataStructureTester {
private:
    ERTInt* ert_dense_ = nullptr;
    ERTInt* ert_sparse_ = nullptr;
    ERTInt* ert_clustered_ = nullptr;
    ERTInt* ert_variable_ = nullptr;
    ERTInt* ert_ycsb_ = nullptr;

public:
    ERTTester(LogManager& log) : DataStructureTester("ERT", log) {}

protected:
    bool initialize() override {
        bool success = true;

        if (g_config.test_ycsb) {
            ert_ycsb_ = NewExtendibleRadixTreeInt();
            if (ert_ycsb_ == nullptr) {
                log_.log_error("YCSB ERT 实例创建失败");
                success = false;
            } else {
                log_.log_info("  |-> 创建 YCSB 数据集实例对象", "ert_ycsb_");
            }
        } else {
            if (g_config.test_dense) {
                ert_dense_ = NewExtendibleRadixTreeInt();
                if (ert_dense_ == nullptr) {
                    log_.log_error("Dense ERT 实例创建失败");
                    success = false;
                } else {
                    log_.log_info("  |-> 创建 Dense 数据集实例对象", "ert_dense_");
                }
            }

            if (g_config.test_sparse) {
                ert_sparse_ = NewExtendibleRadixTreeInt();
                if (ert_sparse_ == nullptr) {
                    log_.log_error("Sparse ERT 实例创建失败");
                    success = false;
                } else {
                    log_.log_info("  |-> 创建 Sparse 数据集实例对象", "ert_sparse_");
                }
            }

            if (g_config.test_clustered) {
                ert_clustered_ = NewExtendibleRadixTreeInt();
                if (ert_clustered_ == nullptr) {
                    log_.log_error("Clustered ERT 实例创建失败");
                    success = false;
                } else {
                    log_.log_info("  |-> 创建 Clustered 数据集实例对象", "ert_clustered_");
                }
            }
        }

        return success;
    }

    void test_insert(uint64_t* keys, int count, PerformanceMetrics::DatasetType type, double& time_us) override {
        ERTInt* ert = static_cast<ERTInt*>(get_instance(type));

        time_us = measure_time([&]() {
            for (int i = 0; i < count; i++) {
                ert->Insert(keys[i], i);
                // show_progress(i + 1, count, "插入");
            }
        });

        // int num = count / 10;
        // for (int i = 0, j = num * i; i < 10; i++) {
        //     time_us = measure_time([&]() {
        //         for (int k = 0; k < num; k++) {
        //             ert->Insert(keys[j], j++);
        //             // show_progress(j + 1, count, "插入");
        //         }
        //     });
        //     // printf("  |-> ERT %s insert 执行第 %d 个 %llu 次操作耗时: %.f ns, 吞吐量: %.6f Mops, 目录扩展次数: %lu, 段分裂次数: %lu\n",
        //     //        dataset_display_name(type).c_str(), i + 1, num, time_us * 1000,
        //     //        num / (time_us / 1000000.0) / 1000000.0,
        //     //        ert_insert_directory_grow_time_.size(),
        //     //        ert_insert_segment_split_time_.size());
        //     // ert_insert_directory_grow_time_.clear();
        //     // ert_insert_segment_split_time_.clear();
        //     printf("  |-> ERT %s insert 执行第 %d 个 %llu 次操作耗时: %.f ns, 吞吐量: %.6f Mops\n",
        //            dataset_display_name(type).c_str(), i + 1, num, time_us * 1000,
        //            num / (time_us / 1000000.0) / 1000000.0);
        // }

        printf("  |-> ERT %s insert %llu 操作总耗时: %.f s\n",
               dataset_display_name(type).c_str(), count, time_us * 1000);
        uint64_t total_update_latency = std::accumulate(ert->per_insert_node_update_latency_.begin(), ert->per_insert_node_update_latency_.end(), 0ULL)
                                        + std::accumulate(ert_per_insert_node_update_latency_.begin(), ert_per_insert_node_update_latency_.end(), 0ULL);
        uint64_t total_split_grow_latency = std::accumulate(ert->per_insert_node_split_latency_.begin(), ert->per_insert_node_split_latency_.end(), 0ULL)
                                            + std::accumulate(ert->per_insert_node_grow_latency_.begin(), ert->per_insert_node_grow_latency_.end(), 0ULL)
                                            + std::accumulate(ert_insert_segment_split_time_.begin(), ert_insert_segment_split_time_.end(), 0ULL)
                                            + std::accumulate(ert_insert_directory_grow_time_.begin(), ert_insert_directory_grow_time_.end(), 0ULL);
        uint64_t total_traversal_latency = time_us * 1000 - total_update_latency - total_split_grow_latency;
        printf("  |-> ERT %s insert 每次插入平均耗时: %.f ns, 树遍历延迟平均延迟: %llu ns, 节点分裂+节点扩容的平均延迟: %llu ns,  节点更新的平均延迟: %llu ns\n",
               dataset_display_name(type).c_str(),
               time_us * 1000 / count,
               total_traversal_latency / count,
               total_split_grow_latency / count,
               total_update_latency / count);
        printf("  |-> ERT 目录扩展次数: %lu - 平均耗时: %lu ns, 段分裂次数: %llu - 平均耗时: %lu ns\n",
               ert_insert_directory_grow_time_.size(),
               std::accumulate(ert_insert_directory_grow_time_.begin(), ert_insert_directory_grow_time_.end(), 0ULL) / (ert_insert_directory_grow_time_.size() ? ert_insert_directory_grow_time_.size() : 1),
               ert_insert_segment_split_time_.size() - ert_insert_directory_grow_time_.size(),
               std::accumulate(ert_insert_segment_split_time_.begin(), ert_insert_segment_split_time_.end(), 0ULL) / (ert_insert_segment_split_time_.size() ? ert_insert_segment_split_time_.size() : 1));

        // printf("  |-> ERT %s insert %llu 操作总树遍历延迟: %llu ns\n",
        //        dataset_display_name(type).c_str(), count, std::accumulate(ert->per_insert_node_traversal_latency_.begin(), ert->per_insert_node_traversal_latency_.end(), 0ULL) / (ert->per_insert_node_traversal_latency_.size() ? ert->per_insert_node_traversal_latency_.size() : 1));
    }

    void test_search(uint64_t* keys, int count, PerformanceMetrics::DatasetType type, double& time_us) override {
        ERTInt* ert = static_cast<ERTInt*>(get_instance(type));

        time_us = measure_time([&]() {
            for (int i = 0; i < count; i++) {
                ert->Search(keys[i]);
                // unique_ERT_keys.insert(ert->Search(keys[i]));
                // printf("  |-> ERT 查询 Key %llu, Value = %llu\n", keys[i], value);
                // show_progress(i + 1, count, "查询");
            }
        });

        printf("  |-> ERT %s search 操作返回的唯一值数量: %llu, 总耗时: %.f s\n",
               dataset_display_name(type).c_str(), unique_ERT_keys.size(), time_us * 1000);

        uint64_t total_traversal_latency = static_cast<uint64_t>(time_us * 1000)
                                           - std::accumulate(ert->per_search_per_node_latency_.begin(), ert->per_search_per_node_latency_.end(), 0ULL);

        printf("  |-> ERT %s search 操作访问节点 %llu, 每次查询平均耗时: %.f ns, 树遍历延迟平均延迟: %llu ns, 节点访问平均延迟: %llu ns, 访问每个节点的平均延迟: %llu ns, 平均访问节点的数量: %.3f\n",
               dataset_display_name(type).c_str(),
               ert->per_search_per_node_latency_.size(),
               time_us * 1000 / count,
               total_traversal_latency / count,
               std::accumulate(ert->per_search_per_node_latency_.begin(), ert->per_search_per_node_latency_.end(), 0ULL) / count,
               std::accumulate(ert->per_search_per_node_latency_.begin(), ert->per_search_per_node_latency_.end(), 0ULL) / (ert->per_search_per_node_latency_.size() ? ert->per_search_per_node_latency_.size() : 1),
               static_cast<double>(ert->per_search_per_node_latency_.size()) / count);
    }

    void test_range(uint64_t start, uint64_t end, PerformanceMetrics::DatasetType type, double& time_us, uint64_t& result_count) override {
        ERTInt* ert = static_cast<ERTInt*>(get_instance(type));

        ert->range_segment_scan_latency_.clear();

        time_us = measure_time([&]() {
            result_count = ert->scan(start, end).size();
        });

        printf("  |-> ERT %s 查询范围 [%llu, %llu] 共 %llu 个键, 总耗时: %.f ns\n",
               dataset_display_name(type).c_str(), start, end, result_count, time_us * 1000);

        printf("  |-> ERT %s range 操作树遍历延迟: %llu ns, 段扫描总延迟: %llu ns, 平均段扫描延迟: %llu ns, 访问段的数量: %llu\n",
               dataset_display_name(type).c_str(),
               static_cast<uint64_t>(time_us * 1000) - std::accumulate(ert->range_segment_scan_latency_.begin(), ert->range_segment_scan_latency_.end(), 0ULL),
               std::accumulate(ert->range_segment_scan_latency_.begin(), ert->range_segment_scan_latency_.end(), 0ULL),
               std::accumulate(ert->range_segment_scan_latency_.begin(), ert->range_segment_scan_latency_.end(), 0ULL) / (ert->range_segment_scan_latency_.size() ? ert->range_segment_scan_latency_.size() : 1),
               ert->range_segment_scan_latency_.size());
    }

    void test_variable_insert(std::vector<VariableKey>& keys, int count, PerformanceMetrics::DatasetType type, double& time_us) {
        ERTInt* ert = static_cast<ERTInt*>(get_instance(type));

        // time_us = measure_time([&]() {
        //     for (int i = 0; i < count; i++) {
        //         ert->Insert(keys[i], i);
        //         // show_progress(i + 1, count, "插入");
        //     }
        // });
        char buf[128];

        // 获取当前 Key 的指针和长度
        const char* key_data = keys[1].data.get();
        uint64_t key_len = keys[1].length;
        std::string temp_str(key_data, key_len);
        snprintf(buf, sizeof(buf), "Len = %llu B, Content = %s", (unsigned long long)key_len, temp_str.c_str());
        printf("%s\n", buf);
    }

    void test_variable_search(std::vector<VariableKey>& keys, int count, PerformanceMetrics::DatasetType type, double& time_us) {
        ERTInt* ert = static_cast<ERTInt*>(get_instance(type));

        // time_us = measure_time([&]() {
        //     for (int i = 0; i < count; i++) {
        //         ert->Search(keys[i]);
        //         // show_progress(i + 1, count, "查询");
        //     }
        // });
    }

    void test_ycsb_operations(YCSBLoader* loader, int count, YCSBPhase phase, PerformanceMetrics::DatasetType ycsb_type, double& time_us) override {
        ERTInt* ert = static_cast<ERTInt*>(get_instance(ycsb_type));
        auto& operation_records = loader->get_operation_records(phase);

        time_us = measure_time([&]() {
            for (int i = 0; i < count; i++) {
                if (operation_records[i].op_char == 'r') {
                    ert->Search(operation_records[i].key);
                } else if (operation_records[i].op_char == 'u' || operation_records[i].op_char == 'i') {
                    ert->Insert(operation_records[i].key, operation_records[i].value);
                }

                // if (phase == YCSBPhase::LOAD) {
                //     show_progress(i + 1, count, "YCSB 预加载");
                // } else {
                //     show_progress(i + 1, count, "YCSB 测试");
                // }
            }
        });
    }

    uint64_t get_memory_usage(PerformanceMetrics::DatasetType type) override {
        ERTInt* ert = static_cast<ERTInt*>(get_instance(type));

        return ert->memory_profile(nullptr);
    }

    void* get_instance(PerformanceMetrics::DatasetType type) override {
        switch (type) {
            case PerformanceMetrics::DENSE:
                return ert_dense_;
            case PerformanceMetrics::SPARSE:
                return ert_sparse_;
            case PerformanceMetrics::CLUSTERED:
                return ert_clustered_;
            case PerformanceMetrics::VARIABLE:
                return ert_variable_;
            case PerformanceMetrics::YCSB_WORKLOAD_A:
            case PerformanceMetrics::YCSB_WORKLOAD_B:
            case PerformanceMetrics::YCSB_WORKLOAD_C:
            case PerformanceMetrics::YCSB_WORKLOAD_D:
            case PerformanceMetrics::YCSB_WORKLOAD_E:
            case PerformanceMetrics::YCSB_WORKLOAD_F:
                return ert_ycsb_;
            default:
                return nullptr;
        }
    }
};

class WORTTester : public DataStructureTester {
private:
    wort_tree* wort_dense_ = nullptr;
    wort_tree* wort_sparse_ = nullptr;
    wort_tree* wort_clustered_ = nullptr;
    wort_tree* wort_variable_ = nullptr;
    wort_tree* wort_ycsb_ = nullptr;

public:
    WORTTester(LogManager& log) : DataStructureTester("WORT", log) {}

    ~WORTTester() {}

protected:
    bool initialize() override {
        bool success = true;

        if (g_config.test_ycsb) {
            wort_ycsb_ = new_wort_tree();
            if (wort_ycsb_ == nullptr) {
                log_.log_error("YCSB WORT 实例创建失败");
                success = false;
            } else {
                log_.log_info("  |-> 创建 YCSB 数据集实例对象", "wort_ycsb_");
            }
        } else {
            if (g_config.test_dense) {
                wort_dense_ = new_wort_tree();
                if (wort_dense_ == nullptr) {
                    log_.log_error("Dense WORT 实例创建失败");
                    success = false;
                } else {
                    log_.log_info("  |-> 创建 Dense 数据集实例对象", "wort_dense_");
                }
            }

            if (g_config.test_sparse) {
                wort_sparse_ = new_wort_tree();
                if (wort_sparse_ == nullptr) {
                    log_.log_error("Sparse WORT 实例创建失败");
                    success = false;
                } else {
                    log_.log_info("  |-> 创建 Sparse 数据集实例对象", "wort_sparse_");
                }
            }

            if (g_config.test_clustered) {
                wort_clustered_ = new_wort_tree();
                if (wort_clustered_ == nullptr) {
                    log_.log_error("Clustered WORT 实例创建失败");
                    success = false;
                } else {
                    log_.log_info("  |-> 创建 Clustered 数据集实例对象", "wort_clustered_");
                }
            }
        }

        return success;
    }

    void test_insert(uint64_t* keys, int count, PerformanceMetrics::DatasetType type, double& time_us) override {
        wort_tree* wort = static_cast<wort_tree*>(get_instance(type));

        time_us = measure_time([&]() {
            for (int i = 0; i < count; i++) {
                wort_put(wort, keys[i], 8, (char*)&i);
                // show_progress(i + 1, count, "插入");
            }
        });

        // int num = count / 10;
        // for (int i = 0, j = num * i; i < 10; i++) {
        //     time_us = measure_time([&]() {
        //         for (int k = 0; k < num; k++, j++) {
        //             wort_put(wort, keys[j], 8, (char*)&j);
        //             // show_progress(j + 1, count, "插入");
        //         }
        //     });
        //     printf("  |-> WORT %s insert 执行第 %d 个 %llu 次操作耗时: %.f ns, 吞吐量: %.6f Mops\n",
        //            dataset_display_name(type).c_str(), i + 1, num, time_us * 1000,
        //            num / (time_us / 1000000.0) / 1000000.0);
        // }

        printf("  |-> WORT %s insert %llu 操作总耗时: %.f ns\n",
               dataset_display_name(type).c_str(), count, time_us * 1000);
        uint64_t total_update_latency = std::accumulate(wort_per_insert_node_update_latency_.begin(), wort_per_insert_node_update_latency_.end(), 0ULL);
        uint64_t total_split_grow_latency = std::accumulate(wort_per_insert_node_split_latency_.begin(), wort_per_insert_node_split_latency_.end(), 0ULL)
                                            + std::accumulate(wort_per_insert_node_grow_latency_.begin(), wort_per_insert_node_grow_latency_.end(), 0ULL);
        uint64_t total_traversal_latency = time_us * 1000 - total_update_latency - total_split_grow_latency;
        printf("  |-> WORT %s insert 每次插入平均耗时: %.f ns, 树遍历延迟平均延迟: %llu ns, 节点分裂+节点扩容的平均延迟: %llu ns,  节点更新的平均延迟: %llu ns\n",
               dataset_display_name(type).c_str(),
               time_us * 1000 / count,
               total_traversal_latency / count,
               total_split_grow_latency / count,
               total_update_latency / count);
    }

    void test_search(uint64_t* keys, int count, PerformanceMetrics::DatasetType type, double& time_us) override {
        wort_tree* wort = static_cast<wort_tree*>(get_instance(type));

        time_us = measure_time([&]() {
            for (int i = 0; i < count; i++) {
                wort_get(wort, keys[i], 8);
                // unique_WORT_keys.insert(wort_get(wort, keys[i], 8));
                // show_progress(i + 1, count, "查询");
            }
        });

        printf("  |-> WORT %s search 操作返回的唯一值数量: %llu, 总耗时: %.f ns\n",
               dataset_display_name(type).c_str(), unique_WORT_keys.size(), time_us * 1000);

        uint64_t total_traversal_latency = static_cast<uint64_t>(time_us * 1000)
                                           - std::accumulate(wort_per_search_per_node_latency_.begin(), wort_per_search_per_node_latency_.end(), 0ULL);

        printf("  |-> WORT %s search 操作访问节点 %llu, 每次查询平均耗时: %.f ns, 树遍历延迟平均延迟: %llu ns, 节点访问平均延迟: %llu ns, 访问每个节点的平均延迟: %llu ns, 平均访问节点的数量: %.3f\n",
               dataset_display_name(type).c_str(),
               wort_per_search_per_node_latency_.size(),
               time_us * 1000 / count,
               total_traversal_latency / count,
               std::accumulate(wort_per_search_per_node_latency_.begin(), wort_per_search_per_node_latency_.end(), 0ULL) / count,
               std::accumulate(wort_per_search_per_node_latency_.begin(), wort_per_search_per_node_latency_.end(), 0ULL) / (wort_per_search_per_node_latency_.size() ? wort_per_search_per_node_latency_.size() : 1),
               static_cast<double>(wort_per_search_per_node_latency_.size()) / count);
    }

    void test_range(uint64_t start, uint64_t end, PerformanceMetrics::DatasetType type, double& time_us, uint64_t& result_count) override {
        wort_tree* wort = static_cast<wort_tree*>(get_instance(type));

        wort_range_segment_scan_latency_.clear();
        wort_range_segment_leaf_latency_.clear();

        time_us = measure_time([&]() {
            result_count = wort_scan(wort, start, end + 1, 8).size();
        });

        printf("  |-> WORT %s 查询范围 [%llu, %llu] 共 %llu 个键, 总耗时: %.f ns\n",
               dataset_display_name(type).c_str(), start, end, result_count, time_us * 1000);

        printf("  |-> WORT %s range 操作树遍历延迟: %llu ns, 段扫描总延迟: %llu ns, 平均段扫描延迟: %llu ns, 访问段的数量: %llu\n",
               dataset_display_name(type).c_str(),
               static_cast<uint64_t>(time_us * 1000)
                 - std::accumulate(wort_range_segment_scan_latency_.begin(), wort_range_segment_scan_latency_.end(), 0ULL)
                 - std::accumulate(wort_range_segment_leaf_latency_.begin(), wort_range_segment_leaf_latency_.end(), 0ULL),
               std::accumulate(wort_range_segment_scan_latency_.begin(), wort_range_segment_scan_latency_.end(), 0ULL)
                 + std::accumulate(wort_range_segment_leaf_latency_.begin(), wort_range_segment_leaf_latency_.end(), 0ULL),
               (std::accumulate(wort_range_segment_scan_latency_.begin(), wort_range_segment_scan_latency_.end(), 0ULL)
                + std::accumulate(wort_range_segment_leaf_latency_.begin(), wort_range_segment_leaf_latency_.end(), 0ULL))
                 / (wort_range_segment_scan_latency_.size() ? wort_range_segment_scan_latency_.size() : 1),
               wort_range_segment_scan_latency_.size());
    }

    void test_variable_insert(std::vector<VariableKey>& keys, int count, PerformanceMetrics::DatasetType type, double& time_us) {
        wort_tree* wort = static_cast<wort_tree*>(get_instance(type));

        // time_us = measure_time([&]() {
        //     for (int i = 0; i < count; i++) {
        //         wort_put(wort, keys[i].key, 8, (char*)&keys[i].value);
        //         // show_progress(i + 1, count, "插入");
        //     }
        // });
        char buf[128];

        // 获取当前 Key 的指针和长度
        const char* key_data = keys[1].data.get();
        uint64_t key_len = keys[1].length;
        std::string temp_str(key_data, key_len);
        snprintf(buf, sizeof(buf), "Len = %llu B, Content = %s", (unsigned long long)key_len, temp_str.c_str());
        printf("%s\n", buf);
    }

    void test_variable_search(std::vector<VariableKey>& keys, int count, PerformanceMetrics::DatasetType type, double& time_us) {
        wort_tree* wort = static_cast<wort_tree*>(get_instance(type));

        // time_us = measure_time([&]() {
        //     for (int i = 0; i < count; i++) {
        //         wort_get(wort, keys[i], 8);
        //         // show_progress(i + 1, count, "查询");
        //     }
        // });
    }

    void test_ycsb_operations(YCSBLoader* loader, int count, YCSBPhase phase, PerformanceMetrics::DatasetType ycsb_type, double& time_us) override {
        wort_tree* wort = static_cast<wort_tree*>(get_instance(ycsb_type));
        auto& operation_records = loader->get_operation_records(phase);

        time_us = measure_time([&]() {
            for (int i = 0; i < count; i++) {
                if (operation_records[i].op_char == 'r') { // READ
                    wort_get(wort, operation_records[i].key, 8);
                } else if (operation_records[i].op_char == 'u' || operation_records[i].op_char == 'i') { // UPDATE or INSERT
                    wort_put(wort, operation_records[i].key, 8, (char*)&operation_records[i].value);
                }

                // if (phase == YCSBPhase::LOAD) {
                //     show_progress(i + 1, count, "YCSB 预加载");
                // } else {
                //     show_progress(i + 1, count, "YCSB 测试");
                // }
            }
        });
    }

    uint64_t get_memory_usage(PerformanceMetrics::DatasetType type) override {
        wort_tree* wort = static_cast<wort_tree*>(get_instance(type));

        return wort_memory_profile(wort->root);
    }

    void* get_instance(PerformanceMetrics::DatasetType type) override {
        switch (type) {
            case PerformanceMetrics::DENSE:
                return wort_dense_;
            case PerformanceMetrics::SPARSE:
                return wort_sparse_;
            case PerformanceMetrics::CLUSTERED:
                return wort_clustered_;
            case PerformanceMetrics::VARIABLE:
                return wort_variable_;
            case PerformanceMetrics::YCSB_WORKLOAD_A:
            case PerformanceMetrics::YCSB_WORKLOAD_B:
            case PerformanceMetrics::YCSB_WORKLOAD_C:
            case PerformanceMetrics::YCSB_WORKLOAD_D:
            case PerformanceMetrics::YCSB_WORKLOAD_E:
            case PerformanceMetrics::YCSB_WORKLOAD_F:
                return wort_ycsb_;
            default:
                return nullptr;
        }
    }
};

class WOARTTester : public DataStructureTester {
private:
    woart_tree* woart_dense_ = nullptr;
    woart_tree* woart_sparse_ = nullptr;
    woart_tree* woart_clustered_ = nullptr;
    woart_tree* woart_variable_ = nullptr;
    woart_tree* woart_ycsb_ = nullptr;

public:
    WOARTTester(LogManager& log) : DataStructureTester("WOART", log) {}

    ~WOARTTester() {}

protected:
    bool initialize() override {
        bool success = true;

        if (g_config.test_ycsb) {
            woart_ycsb_ = new_woart_tree();
            if (woart_ycsb_ == nullptr) {
                log_.log_error("YCSB WOART 实例创建失败");
                success = false;
            } else {
                log_.log_info("  |-> 创建 YCSB 数据集实例对象", "woart_ycsb_");
            }
        } else {
            if (g_config.test_dense) {
                woart_dense_ = new_woart_tree();
                if (woart_dense_ == nullptr) {
                    log_.log_error("Dense WOART 实例创建失败");
                    success = false;
                } else {
                    log_.log_info("  |-> 创建 Dense 数据集实例对象", "woart_dense_");
                }
            }

            if (g_config.test_sparse) {
                woart_sparse_ = new_woart_tree();
                if (woart_sparse_ == nullptr) {
                    log_.log_error("Sparse WOART 实例创建失败");
                    success = false;
                } else {
                    log_.log_info("  |-> 创建 Sparse 数据集实例对象", "woart_sparse_");
                }
            }

            if (g_config.test_clustered) {
                woart_clustered_ = new_woart_tree();
                if (woart_clustered_ == nullptr) {
                    log_.log_error("Clustered WOART 实例创建失败");
                    success = false;
                } else {
                    log_.log_info("  |-> 创建 Clustered 数据集实例对象", "woart_clustered_");
                }
            }
        }

        return success;
    }

    void test_insert(uint64_t* keys, int count, PerformanceMetrics::DatasetType type, double& time_us) override {
        woart_tree* woart = static_cast<woart_tree*>(get_instance(type));

        time_us = measure_time([&]() {
            for (int i = 0; i < count; i++) {
                woart_put(woart, keys[i], 8, (char*)&i);
                // show_progress(i + 1, count, "插入");
            }
        });

        // int num = count / 10;
        // for (int i = 0, j = num * i; i < 10; i++) {
        //     time_us = measure_time([&]() {
        //         for (int k = 0; k < num; k++, j++) {
        //             woart_put(woart, keys[j], 8, (char*)&j);
        //             // show_progress(j + 1, count, "插入");
        //         }
        //     });
        //     printf("  |-> WOART %s insert 执行第 %d 个 %llu 次操作耗时: %.f ns, 吞吐量: %.6f Mops\n",
        //            dataset_display_name(type).c_str(), i + 1, num, time_us * 1000,
        //            num / (time_us / 1000000.0) / 1000000.0);
        // }

        printf("  |-> WOART %s insert %llu 操作总耗时: %.f ns\n",
               dataset_display_name(type).c_str(), count, time_us * 1000);
        uint64_t total_update_latency = std::accumulate(woart_per_insert_node_update_latency_.begin(), woart_per_insert_node_update_latency_.end(), 0ULL);
        uint64_t total_split_grow_latency = std::accumulate(woart_per_insert_node_split_latency_.begin(), woart_per_insert_node_split_latency_.end(), 0ULL)
                                            + std::accumulate(woart_per_insert_node_grow_latency_.begin(), woart_per_insert_node_grow_latency_.end(), 0ULL);
        uint64_t total_traversal_latency = time_us * 1000 - total_update_latency - total_split_grow_latency;
        printf("  |-> WOART %s insert 每次插入平均耗时: %.f ns, 树遍历延迟平均延迟: %llu ns, 节点分裂+节点扩容的平均延迟: %llu ns,  节点更新的平均延迟: %llu ns\n",
               dataset_display_name(type).c_str(),
               time_us * 1000 / count,
               total_traversal_latency / count,
               total_split_grow_latency / count,
               total_update_latency / count);
    }

    void test_search(uint64_t* keys, int count, PerformanceMetrics::DatasetType type, double& time_us) override {
        woart_tree* woart = static_cast<woart_tree*>(get_instance(type));

        time_us = measure_time([&]() {
            for (int i = 0; i < count; i++) {
                woart_get(woart, keys[i], 8);
                // unique_WOART_keys.insert(woart_get(woart, keys[i], 8));
                // show_progress(i + 1, count, "查询");
            }
        });

        printf("  |-> WOART %s search 操作返回的唯一值数量: %llu, 总耗时: %.f ns\n",
               dataset_display_name(type).c_str(), unique_WOART_keys.size(), time_us * 1000);

        uint64_t total_traversal_latency = static_cast<uint64_t>(time_us * 1000)
                                           - std::accumulate(woart_per_search_per_node_latency_.begin(), woart_per_search_per_node_latency_.end(), 0ULL);

        printf("  |-> WOART %s search 操作访问节点 %llu, 每次查询平均耗时: %.f ns, 树遍历延迟平均延迟: %llu ns, 节点访问平均延迟: %llu ns, 访问每个节点的平均延迟: %llu ns, 平均访问节点的数量: %.3f\n",
               dataset_display_name(type).c_str(),
               woart_per_search_per_node_latency_.size(),
               time_us * 1000 / count,
               total_traversal_latency / count,
               std::accumulate(woart_per_search_per_node_latency_.begin(), woart_per_search_per_node_latency_.end(), 0ULL) / count,
               std::accumulate(woart_per_search_per_node_latency_.begin(), woart_per_search_per_node_latency_.end(), 0ULL) / (woart_per_search_per_node_latency_.size() ? woart_per_search_per_node_latency_.size() : 1),
               static_cast<double>(woart_per_search_per_node_latency_.size()) / count);
    }

    void test_range(uint64_t start, uint64_t end, PerformanceMetrics::DatasetType type, double& time_us, uint64_t& result_count) override {
        woart_tree* woart = static_cast<woart_tree*>(get_instance(type));

        woart_range_segment_scan_latency_.clear();
        woart_range_segment_leaf_latency_.clear();

        // auto results = woart_scan(woart, start, end, 8);
        time_us = measure_time([&]() {
            result_count = woart_scan(woart, start, end, 8).size();
        });

        printf("  |-> WOART %s 查询范围 [%llu, %llu] 共 %llu 个键\n", dataset_display_name(type).c_str(), start, end, result_count);

        printf("  |-> WOART %s range 操作树遍历延迟: %llu ns, 段扫描总延迟: %llu ns, 平均段扫描延迟: %llu ns, 访问段的数量: %llu\n",
               dataset_display_name(type).c_str(),
               static_cast<uint64_t>(time_us * 1000)
                 - std::accumulate(woart_range_segment_scan_latency_.begin(), woart_range_segment_scan_latency_.end(), 0ULL)
                 - std::accumulate(woart_range_segment_leaf_latency_.begin(), woart_range_segment_leaf_latency_.end(), 0ULL),
               std::accumulate(woart_range_segment_scan_latency_.begin(), woart_range_segment_scan_latency_.end(), 0ULL)
                 + std::accumulate(woart_range_segment_leaf_latency_.begin(), woart_range_segment_leaf_latency_.end(), 0ULL),
               (std::accumulate(woart_range_segment_scan_latency_.begin(), woart_range_segment_scan_latency_.end(), 0ULL)
                + std::accumulate(woart_range_segment_leaf_latency_.begin(), woart_range_segment_leaf_latency_.end(), 0ULL))
                 / (woart_range_segment_scan_latency_.size() ? woart_range_segment_scan_latency_.size() : 1),
               woart_range_segment_scan_latency_.size());
    }

    void test_variable_insert(std::vector<VariableKey>& keys, int count, PerformanceMetrics::DatasetType type, double& time_us) {
        woart_tree* woart = static_cast<woart_tree*>(get_instance(type));

        // time_us = measure_time([&]() {
        //     for (int i = 0; i < count; i++) {
        //         woart_put(woart, keys[i], 8, (char*)&i);
        //         // show_progress(i + 1, count, "插入");
        //     }
        // });
        char buf[128];

        // 获取当前 Key 的指针和长度
        const char* key_data = keys[1].data.get();
        uint64_t key_len = keys[1].length;
        std::string temp_str(key_data, key_len);
        snprintf(buf, sizeof(buf), "Len = %llu B, Content = %s", (unsigned long long)key_len, temp_str.c_str());
        printf("%s\n", buf);
    }

    void test_variable_search(std::vector<VariableKey>& keys, int count, PerformanceMetrics::DatasetType type, double& time_us) {
        woart_tree* woart = static_cast<woart_tree*>(get_instance(type));

        // time_us = measure_time([&]() {
        //     for (int i = 0; i < count; i++) {
        //         woart_get(woart, keys[i], 8);
        //         // show_progress(i + 1, count, "查询");
        //     }
        // });
    }

    void test_ycsb_operations(YCSBLoader* loader, int count, YCSBPhase phase, PerformanceMetrics::DatasetType ycsb_type, double& time_us) override {
        woart_tree* woart = static_cast<woart_tree*>(get_instance(ycsb_type));
        auto& operation_records = loader->get_operation_records(phase);

        time_us = measure_time([&]() {
            for (int i = 0; i < count; i++) {
                if (operation_records[i].op_char == 'r') {
                    woart_get(woart, operation_records[i].key, 8);
                } else if (operation_records[i].op_char == 'u' || operation_records[i].op_char == 'i') {
                    woart_put(woart, operation_records[i].key, 8, (char*)&operation_records[i].value);
                }

                // if (phase == YCSBPhase::LOAD) {
                //     show_progress(i + 1, count, "YCSB 预加载");
                // } else {
                //     show_progress(i + 1, count, "YCSB 测试");
                // }
            }
        });
    }

    uint64_t get_memory_usage(PerformanceMetrics::DatasetType type) override {
        woart_tree* woart = static_cast<woart_tree*>(get_instance(type));

        return woart_memory_profile(woart->root);
    }

    void* get_instance(PerformanceMetrics::DatasetType type) override {
        switch (type) {
            case PerformanceMetrics::DENSE:
                return woart_dense_;
            case PerformanceMetrics::SPARSE:
                return woart_sparse_;
            case PerformanceMetrics::CLUSTERED:
                return woart_clustered_;
            case PerformanceMetrics::VARIABLE:
                return woart_variable_;
            case PerformanceMetrics::YCSB_WORKLOAD_A:
            case PerformanceMetrics::YCSB_WORKLOAD_B:
            case PerformanceMetrics::YCSB_WORKLOAD_C:
            case PerformanceMetrics::YCSB_WORKLOAD_D:
            case PerformanceMetrics::YCSB_WORKLOAD_E:
            case PerformanceMetrics::YCSB_WORKLOAD_F:
                return woart_ycsb_;
            default:
                return nullptr;
        }
    }
};

class ROARTTester : public DataStructureTester {
private:
    ROART* roart_dense_ = nullptr;
    ROART* roart_sparse_ = nullptr;
    ROART* roart_clustered_ = nullptr;
    ROART* roart_variable_ = nullptr;
    ROART* roart_ycsb_ = nullptr;

public:
    ROARTTester(LogManager& log) : DataStructureTester("ROART", log) {}

    ~ROARTTester() {}

protected:
    bool initialize() override {
        bool success = true;

        if (g_config.test_ycsb) {
            roart_ycsb_ = new_roart();
            if (roart_ycsb_ == nullptr) {
                log_.log_error("YCSB ROART 实例创建失败");
                success = false;
            } else {
                log_.log_info("  |-> 创建 YCSB 数据集实例对象", "roart_ycsb_");
            }
        } else {
            if (g_config.test_dense) {
                roart_dense_ = new_roart();
                if (roart_dense_ == nullptr) {
                    log_.log_error("Dense ROART 实例创建失败");
                    success = false;
                } else {
                    log_.log_info("  |-> 创建 Dense 数据集实例对象", "roart_dense_");
                }
            }

            if (g_config.test_sparse) {
                roart_sparse_ = new_roart();
                if (roart_sparse_ == nullptr) {
                    log_.log_error("Sparse ROART 实例创建失败");
                    success = false;
                } else {
                    log_.log_info("  |-> 创建 Sparse 数据集实例对象", "roart_sparse_");
                }
            }

            if (g_config.test_clustered) {
                roart_clustered_ = new_roart();
                if (roart_clustered_ == nullptr) {
                    log_.log_error("Clustered ROART 实例创建失败");
                    success = false;
                } else {
                    log_.log_info("  |-> 创建 Clustered 数据集实例对象", "roart_clustered_");
                }
            }
        }

        return success;
    }

    void test_insert(uint64_t* keys, int count, PerformanceMetrics::DatasetType type, double& time_us) override {
        ROART* roart = static_cast<ROART*>(get_instance(type));

        time_us = measure_time([&]() {
            for (int i = 0; i < count; i++) {
                roart->put(keys[i], i);
                // show_progress(i + 1, count, "插入");
            }
        });

        // int num = count / 10;
        // for (int i = 0, j = num * i; i < 10; i++) {
        //     time_us = measure_time([&]() {
        //         for (int k = 0; k < num; k++, j++) {
        //             roart->put(keys[j], j);
        //             // show_progress(j + 1, count, "插入");
        //         }
        //     });
        //     printf("  |-> ROART %s insert 执行第 %d 个 %llu 次操作耗时: %.f ns, 吞吐量: %.6f Mops\n",
        //            dataset_display_name(type).c_str(), i + 1, num, time_us * 1000,
        //            num / (time_us / 1000000.0) / 1000000.0);
        // }

        printf("  |-> ROART %s insert %llu 操作总耗时: %.f ns\n",
               dataset_display_name(type).c_str(), count, time_us * 1000);
        uint64_t total_update_latency = std::accumulate(roart_per_insert_node_update_latency_.begin(), roart_per_insert_node_update_latency_.end(), 0ULL);
        uint64_t total_split_grow_latency = std::accumulate(roart_per_insert_node_split_latency_.begin(), roart_per_insert_node_split_latency_.end(), 0ULL)
                                            + std::accumulate(roart_per_insert_node_grow_latency_.begin(), roart_per_insert_node_grow_latency_.end(), 0ULL);
        uint64_t total_traversal_latency = time_us * 1000 - total_update_latency - total_split_grow_latency;
        printf("  |-> ROART %s insert 每次插入平均耗时: %.f ns, 树遍历延迟平均延迟: %llu ns, 节点分裂+节点扩容的平均延迟: %llu ns,  节点更新的平均延迟: %llu ns\n",
               dataset_display_name(type).c_str(),
               time_us * 1000 / count,
               total_traversal_latency / count,
               total_split_grow_latency / count,
               total_update_latency / count);
    }

    void test_search(uint64_t* keys, int count, PerformanceMetrics::DatasetType type, double& time_us) override {
        ROART* roart = static_cast<ROART*>(get_instance(type));

        time_us = measure_time([&]() {
            for (int i = 0; i < count; i++) {
                roart->get(keys[i]);
                // unique_ROART_keys.insert(roart->get(keys[i]));
                // show_progress(i + 1, count, "查询");
            }
        });

        printf("  |-> ROART %s search 操作返回的唯一值数量: %llu, 总耗时: %.f ns\n",
               dataset_display_name(type).c_str(), unique_ROART_keys.size(), time_us * 1000);

        uint64_t total_traversal_latency = static_cast<uint64_t>(time_us * 1000)
                                           - std::accumulate(roart->per_search_per_node_latency_.begin(), roart->per_search_per_node_latency_.end(), 0ULL);

        printf("  |-> ROART %s search 操作访问节点 %llu, 每次查询平均耗时: %.f ns, 树遍历延迟平均延迟: %llu ns, 节点访问平均延迟: %llu ns, 访问每个节点的平均延迟: %llu ns, 平均访问节点的数量: %.3f\n",
               dataset_display_name(type).c_str(),
               roart->per_search_per_node_latency_.size(),
               time_us * 1000 / count,
               total_traversal_latency / count,
               std::accumulate(roart->per_search_per_node_latency_.begin(), roart->per_search_per_node_latency_.end(), 0ULL) / count,
               std::accumulate(roart->per_search_per_node_latency_.begin(), roart->per_search_per_node_latency_.end(), 0ULL) / (roart->per_search_per_node_latency_.size() ? roart->per_search_per_node_latency_.size() : 1),
               static_cast<double>(roart->per_search_per_node_latency_.size()) / count);
    }

    void test_range(uint64_t start, uint64_t end, PerformanceMetrics::DatasetType type, double& time_us, uint64_t& result_count) override {
        ROART* roart = static_cast<ROART*>(get_instance(type));

        roart_range_segment_scan_latency_.clear();

        time_us = measure_time([&]() {
            result_count = roart->scan(start, end + 1, g_config.test_num).size();
        });

        printf("  |-> ROART %s 查询范围 [%llu, %llu] 共 %llu 个键, 总耗时: %.f ns\n",
               dataset_display_name(type).c_str(), start, end, result_count, time_us * 1000);

        printf("  |-> ROART %s range 操作树遍历延迟: %llu ns, 段扫描总延迟: %llu ns, 平均段扫描延迟: %llu ns, 访问段的数量: %lu\n",
               dataset_display_name(type).c_str(),
               static_cast<uint64_t>(time_us * 1000)
                 - std::accumulate(roart_range_segment_scan_latency_.begin(), roart_range_segment_scan_latency_.end(), 0ULL),
               std::accumulate(roart_range_segment_scan_latency_.begin(), roart_range_segment_scan_latency_.end(), 0ULL),
               std::accumulate(roart_range_segment_scan_latency_.begin(), roart_range_segment_scan_latency_.end(), 0ULL) / (roart_range_segment_scan_latency_.size() ? roart_range_segment_scan_latency_.size() : 1),
               roart_range_segment_scan_latency_.size());
    }

    void test_variable_insert(std::vector<VariableKey>& keys, int count, PerformanceMetrics::DatasetType type, double& time_us) {
        ROART* roart = static_cast<ROART*>(get_instance(type));

        // time_us = measure_time([&]() {
        //     for (int i = 0; i < count; i++) {
        //         roart->put(keys[i], 8, (char*)&i);
        //         // show_progress(i + 1, count, "插入");
        //     }
        // });
        char buf[128];

        // 获取当前 Key 的指针和长度
        const char* key_data = keys[1].data.get();
        uint64_t key_len = keys[1].length;
        std::string temp_str(key_data, key_len);
        snprintf(buf, sizeof(buf), "Len = %llu B, Content = %s", (unsigned long long)key_len, temp_str.c_str());
        printf("%s\n", buf);
    }

    void test_variable_search(std::vector<VariableKey>& keys, int count, PerformanceMetrics::DatasetType type, double& time_us) {
        ROART* roart = static_cast<ROART*>(get_instance(type));

        // time_us = measure_time([&]() {
        //     for (int i = 0; i < count; i++) {
        //         roart->get(keys[i]);
        //         // show_progress(i + 1, count, "查询");
        //     }
        // });
    }

    void test_ycsb_operations(YCSBLoader* loader, int count, YCSBPhase phase, PerformanceMetrics::DatasetType ycsb_type, double& time_us) override {
        ROART* roart = static_cast<ROART*>(get_instance(ycsb_type));
        auto& operation_records = loader->get_operation_records(phase);

        time_us = measure_time([&]() {
            for (int i = 0; i < count; i++) {
                if (operation_records[i].op_char == 'r') {
                    roart->get(operation_records[i].key);
                } else if (operation_records[i].op_char == 'u' || operation_records[i].op_char == 'i') {
                    roart->put(operation_records[i].key, operation_records[i].value);
                }

                // if (phase == YCSBPhase::LOAD) {
                //     show_progress(i + 1, count, "YCSB 预加载");
                // } else {
                //     show_progress(i + 1, count, "YCSB 测试");
                // }
            }
        });
    }

    uint64_t get_memory_usage(PerformanceMetrics::DatasetType type) override {
        ROART* roart = static_cast<ROART*>(get_instance(type));

        return roart->memory_profile();
    }

    void* get_instance(PerformanceMetrics::DatasetType type) override {
        switch (type) {
            case PerformanceMetrics::DENSE:
                return roart_dense_;
            case PerformanceMetrics::SPARSE:
                return roart_sparse_;
            case PerformanceMetrics::CLUSTERED:
                return roart_clustered_;
            case PerformanceMetrics::VARIABLE:
                return roart_variable_;
            case PerformanceMetrics::YCSB_WORKLOAD_A:
            case PerformanceMetrics::YCSB_WORKLOAD_B:
            case PerformanceMetrics::YCSB_WORKLOAD_C:
            case PerformanceMetrics::YCSB_WORKLOAD_D:
            case PerformanceMetrics::YCSB_WORKLOAD_E:
            case PerformanceMetrics::YCSB_WORKLOAD_F:
                return roart_ycsb_;
            default:
                return nullptr;
        }
    }
};

class ResultExporter {
public:
    static void print_summary(const vector<DataStructureTester::TestResult>& results, LogManager& log) {
        log.log_section("   >>> 各测试索引结构性能对比结果...");

        print_detailed_comparison(results, log);
    }

    static void print_detailed_comparison(const vector<DataStructureTester::TestResult>& results, LogManager& log) {
        // 为每个指标创建排序输出
        print_sorted_metrics(results, log);

        // 打印每个数据结构的完整性能报告
        if (!g_config.test_ycsb) {
            for (const auto& result : results) {
                print_structure_report(result, log);
            }
        }
    }

    static string to_string_fixed(double value, int precision) {
        ostringstream oss;
        oss << fixed << setprecision(precision) << value;
        return oss.str();
    }

private:
    static void print_sorted_metrics(const vector<DataStructureTester::TestResult>& results, LogManager& log) {
        vector<PerformanceMetrics::DatasetType> dataset_types = {
          PerformanceMetrics::DENSE,
          PerformanceMetrics::SPARSE,
          PerformanceMetrics::CLUSTERED,
          PerformanceMetrics::YCSB_WORKLOAD_A,
          PerformanceMetrics::YCSB_WORKLOAD_B,
          PerformanceMetrics::YCSB_WORKLOAD_C,
          PerformanceMetrics::YCSB_WORKLOAD_D,
          PerformanceMetrics::YCSB_WORKLOAD_E,
          PerformanceMetrics::YCSB_WORKLOAD_F};

        vector<pair<string, PerformanceMetrics::OperationType>> operations = {
          {"insert", PerformanceMetrics::INSERT},
          {"search", PerformanceMetrics::SEARCH},
          {"0.1% 范围查询", PerformanceMetrics::RANGE_001},
          {"0.5% 范围查询", PerformanceMetrics::RANGE_005},
          {"1.0% 范围查询", PerformanceMetrics::RANGE_01},
          {"100% 范围查询", PerformanceMetrics::RANGE_1},
          {"YCSB_LOAD", PerformanceMetrics::YCSB_LOAD},
          {"YCSB_RUN", PerformanceMetrics::YCSB_RUN}};

        // 对每个数据集类型和操作类型进行排序
        for (auto dataset_type : dataset_types) {
            string dataset_name = dataset_display_name(dataset_type);
            bool has_ycsb = (dataset_type >= PerformanceMetrics::YCSB_WORKLOAD_A && dataset_type <= PerformanceMetrics::YCSB_WORKLOAD_F);

            for (auto& [op_name, op_type] : operations) {
                // 跳过 YCSB 相关的非 YCSB 数据集操作
                if (has_ycsb && op_type != PerformanceMetrics::YCSB_LOAD && op_type != PerformanceMetrics::YCSB_RUN) {
                    continue;
                }
                if (!has_ycsb && (op_type == PerformanceMetrics::YCSB_LOAD || op_type == PerformanceMetrics::YCSB_RUN)) {
                    continue;
                }

                vector<tuple<string, double, double>> sorted_data;

                for (const auto& result : results) {
                    if (result.metrics.operations.count(dataset_type) && result.metrics.operations.at(dataset_type).count(op_type)) {
                        const auto& metrics = result.metrics.operations.at(dataset_type).at(op_type);
                        sorted_data.emplace_back(result.name, metrics.latency, metrics.throughput);
                    }
                }

                // 按延迟从低到高排序（性能从好到差）
                sort(sorted_data.begin(), sorted_data.end(),
                     [](const auto& a, const auto& b) { return get<1>(a) < get<1>(b); });

                if (!sorted_data.empty()) {
                    log.log_section("   >>> " + dataset_name + " 数据集 " + op_name + " 性能排序");
                    for (size_t i = 0; i < sorted_data.size(); i++) {
                        const auto& [name, latency, throughput] = sorted_data[i];
                        string throughput_unit = (op_type >= PerformanceMetrics::RANGE_001 && op_type <= PerformanceMetrics::RANGE_1) ? " KQPS" : " MOPS";

                        double avg_time_us = 0.0;
                        for (const auto& result : results) {
                            if (result.name == name && result.metrics.operations.count(dataset_type) && result.metrics.operations.at(dataset_type).count(op_type)) {
                                const auto& metrics = result.metrics.operations.at(dataset_type).at(op_type);
                                avg_time_us = metrics.avg_time_us;
                                break;
                            }
                        }

                        log.log("   |-> " + to_string(i + 1) + ". " + name + ": 延迟=" + to_string_fixed(latency, 6) + " s, 吞吐量=" + to_string_fixed(throughput, 6) + throughput_unit + ", 平均每操作时间=" + to_string_fixed(avg_time_us, 6) + " μs/op");
                    }
                }
            }

            // 内存使用排序（从低到高）
            vector<tuple<string, double, double>> memory_sorted;
            for (const auto& result : results) {
                if (result.metrics.memory.count(dataset_type)) {
                    const auto& memory = result.metrics.memory.at(dataset_type);
                    memory_sorted.emplace_back(result.name, memory.mb(), memory.utilization_percent);
                }
            }

            // 按内存使用从低到高排序
            sort(memory_sorted.begin(), memory_sorted.end(),
                 [](const auto& a, const auto& b) { return get<1>(a) < get<1>(b); });

            if (!memory_sorted.empty()) {
                log.log_section("   >>> " + dataset_name + " 数据集内存使用排序");
                for (size_t i = 0; i < memory_sorted.size(); i++) {
                    const auto& [name, memory_mb, utilization] = memory_sorted[i];
                    log.log("   |-> " + to_string(i + 1) + ". " + name + ": 内存=" + to_string_fixed(memory_mb, 2) + " MB, 利用率=" + to_string_fixed(utilization, 2) + "%");
                }
            }
        }
    }

    static void print_structure_report(const DataStructureTester::TestResult& result, LogManager& log) {
        log.log_section("   >>> " + result.name + " - 性能报告");

        vector<PerformanceMetrics::DatasetType> dataset_types = {
          PerformanceMetrics::DENSE,
          PerformanceMetrics::SPARSE,
          PerformanceMetrics::CLUSTERED};

        for (auto dataset_type : dataset_types) {
            string dataset_name = dataset_display_name(dataset_type);

            // 检查该数据集是否有数据
            if (!result.metrics.operations.count(dataset_type) && !result.metrics.memory.count(dataset_type)) {
                continue;
            }

            log.log_info("  |-> " + dataset_name + " 数据集", "");

            if (result.metrics.operations.count(dataset_type)) {
                const auto& ops = result.metrics.operations.at(dataset_type);

                // 插入性能
                if (ops.count(PerformanceMetrics::INSERT)) {
                    const auto& insert = ops.at(PerformanceMetrics::INSERT);
                    log.log("    插入: 延迟=" + to_string_fixed(insert.latency, 6) + " s, 吞吐量=" + to_string_fixed(insert.throughput, 6) + " MOPS, 平均每操作时间=" + to_string_fixed(insert.avg_time_us, 6) + " μs/op");
                }

                // 查询性能
                if (ops.count(PerformanceMetrics::SEARCH)) {
                    const auto& search = ops.at(PerformanceMetrics::SEARCH);
                    log.log("    查询: 延迟=" + to_string_fixed(search.latency, 6) + " s, 吞吐量=" + to_string_fixed(search.throughput, 6) + " MOPS, 平均每操作时间=" + to_string_fixed(search.avg_time_us, 6) + " μs/op");
                }

                // 范围查询性能
                vector<pair<string, PerformanceMetrics::OperationType>> ranges = {
                  {"0.1%", PerformanceMetrics::RANGE_001},
                  {"0.5%", PerformanceMetrics::RANGE_005},
                  {"1.0%", PerformanceMetrics::RANGE_01},
                  {"100%", PerformanceMetrics::RANGE_1}};

                for (auto& [range_name, op_type] : ranges) {
                    if (ops.count(op_type)) {
                        const auto& range = ops.at(op_type);
                        log.log("    " + range_name + " 范围查询: 延迟=" + to_string_fixed(range.latency, 6) + " s, 吞吐量=" + to_string_fixed(range.throughput, 6) + " KQPS, 平均每操作时间=" + to_string_fixed(range.avg_time_us, 6) + " μs/op");
                    }
                }

                // 内存信息
                if (result.metrics.memory.count(dataset_type)) {
                    const auto& memory = result.metrics.memory.at(dataset_type);
                    log.log("    内存: 使用量=" + to_string_fixed(memory.mb(), 2) + " MB, 空间利用率=" + to_string_fixed(memory.utilization_percent, 2) + "%");
                }
            }
        }
    }
};

int main(int argc, char* argv[]) {
    // 在内存中，不使用 PM 模式
    if (argc == 2) {
        g_config.test_num = atoi(argv[1]);
        cout << "测试数据量: " << g_config.test_num << " 个键" << endl;
    }

    // 在 PM 模式下运行
    if (argc == 3) {
        g_config.on_pm = true;
        g_config.test_num = atoi(argv[1]);
        g_config.pm_path = argv[2];
        cout << "测试数据量: " << g_config.test_num << " 个键" << endl;
        cout << "PM 模式启用, PM 路径: " << g_config.pm_path << endl;
    }

    // 在 PM 模式下运行
    if (argc == 5) {
        g_config.on_pm = true;
        g_config.test_num = atoi(argv[1]);
        g_config.pm_path = argv[2];
        g_config.index_type = argv[3];
        g_config.random = (strcmp(argv[4], "ran") == 0);
        cout << "测试数据量: " << g_config.test_num << " 个键" << endl;
        cout << "PM 模式启用, PM 路径: " << g_config.pm_path << endl;
    }

    // 在 PM 模式下运行
    if (argc == 6) {
        g_config.on_pm = true;
        g_config.test_num = atoi(argv[1]);
        g_config.pm_path = argv[2];
        g_config.index_type = argv[3];
        g_config.selectivity = atof(argv[4]);
        g_config.random = (strcmp(argv[5], "ran") == 0);
        cout << "测试数据量: " << g_config.test_num << " 个键" << endl;
        cout << "PM 模式启用, PM 路径: " << g_config.pm_path << endl;
    }

    // 在 PM 模式下运行
    if (argc == 4) {
        g_config.on_pm = true;
        g_config.test_num = atoi(argv[1]);
        g_config.pm_path = argv[2];
        g_config.random = (strcmp(argv[3], "ran") == 0);
        cout << "测试数据量: " << g_config.test_num << " 个键" << endl;
        cout << "PM 模式启用, PM 路径: " << g_config.pm_path << endl;
    }

    cout << "所有日志将保存到: ./Log/ 目录下" << endl;
    cout << "对比结果将保存到: ./Result/ 目录下的 CSV 文件" << endl;

    try {
        // 创建日志管理器
        string log_filename;
        if (g_config.test_ycsb) {
            // 解析数据集类型：从数据文件路径中提取数据集名称
            string dataset_name = "unknown";
            if (!g_config.ycsb_data_file.empty()) {
                size_t last_slash = g_config.ycsb_data_file.find_last_of('/');
                if (last_slash != string::npos) {
                    string path_before_file = g_config.ycsb_data_file.substr(0, last_slash);
                    size_t second_last_slash = path_before_file.find_last_of('/');
                    if (second_last_slash != string::npos) {
                        dataset_name = path_before_file.substr(second_last_slash + 1);
                    } else {
                        dataset_name = path_before_file;
                    }
                }
            }

            g_config.ycsb_data_file_load = FLAGS_ycsb_data_file_load.at(dataset_name);
            string log_dir = "./Log/" + dataset_name + "/";
            system(("mkdir -p " + log_dir).c_str());
            log_filename = log_dir + "ycsb_" + g_config.ycsb_workload + "_log.txt";
        } else if (argc == 4) {
            string log_dir = "./Log/";
            system(("mkdir -p " + log_dir).c_str());
            log_filename = log_dir + (g_config.random ? "ran" : "seq") + "_test_log.txt";

            printf("log_filename: %s\n", log_filename.c_str());
        } else if (argc == 5) {
            string log_dir = "./Log/" + g_config.index_type + "/";
            system(("mkdir -p " + log_dir).c_str());
            log_filename = log_dir + "test_log.txt";
            // log_filename = log_dir + g_config.index_type + "_" + (g_config.random ? "ran" : "seq") + "_latencyBreakdown" + ".txt";
            log_filename = log_dir + g_config.index_type + "_" + (g_config.random ? "ran" : "seq") + "_segment_8" + ".txt";
            // log_filename = log_dir + g_config.index_type + "_" + (g_config.random ? "ran" : "seq") + "_base" + ".txt";

            // printf("log_filename: %s\n", log_filename.c_str());
        } else if (argc == 6) {
            string log_dir = "./Log/" + g_config.index_type + "/";
            system(("mkdir -p " + log_dir).c_str());
            // log_filename = log_dir + "test_log.txt";
            log_filename = log_dir + g_config.index_type + "_" + (g_config.random ? "ran" : "seq") + "_" + to_string(static_cast<double>(g_config.selectivity * 100)) + ".txt";

            printf("log_filename: %s\n", log_filename.c_str());
        } else {
            string log_dir = "./Log/";
            system(("mkdir -p " + log_dir).c_str());
            log_filename = log_dir + "test_log.txt";

            printf("log_filename: %s\n", log_filename.c_str());
        }
        LogManager log(log_filename);

        // 初始化内存分配器
        log.log_section("   >>> 初始化内存分配器...");
        init_fast_allocator(true, g_config.on_pm, g_config.pm_path.c_str());
        log.log_section("   >>> 初始化内存分配器完成");
        log.log_Line_break();

        // 准备数据集，如果是基准测试则生成数据集，如果是 YCSB 测试则加载数据集
        DatasetManager dataset(g_config, log);

        // 创建所有测试器
        vector<unique_ptr<DataStructureTester>> testers;
        // testers.push_back(make_unique<WOARTTester>(log));
        // testers.push_back(make_unique<WORTTester>(log));
        // testers.push_back(make_unique<ROARTTester>(log));
        testers.push_back(make_unique<ROERTTester>(log));
        // testers.push_back(make_unique<ERTTester>(log));

        // testers.push_back(make_unique<FastFairTester>(log));
        // testers.push_back(make_unique<LBTreeTester>(log));
        // testers.push_back(make_unique<WOPETester>(log));

        std::string index_name = g_config.index_type;
        std::transform(index_name.begin(), index_name.end(), index_name.begin(), ::toupper);
        if (index_name == "ROERT") {
            testers.push_back(make_unique<ROERTTester>(log));
        } else if (index_name == "ERT") {
            testers.push_back(make_unique<ERTTester>(log));
        } else if (index_name == "WORT") {
            testers.push_back(make_unique<WORTTester>(log));
        } else if (index_name == "WOART") {
            testers.push_back(make_unique<WOARTTester>(log));
        } else if (index_name == "ROART") {
            testers.push_back(make_unique<ROARTTester>(log));
        }

        // 运行所有测试
        log.log_section("   >>> 开始进行性能对比测试...");
        log.log_info("  |-> 测试索引数量 ", to_string(testers.size()));

        // 循环打印每个测试器的名称与序号
        for (int i = 0; i < testers.size(); i++) {
            log.log_info("  |-> 测试索引 " + to_string(i + 1) + " ", testers[i]->get_name());
        }

        vector<DataStructureTester::TestResult> results;

        // 运行每个测试器
        for (auto& tester : testers) {
            results.push_back(tester->run_test(dataset));
            log.log_Line_break();

            std::this_thread::sleep_for(std::chrono::milliseconds(2000)); // 等待2000毫秒
        }

        // 找出在set1中但不在set2中的值
        // std::vector<uint64_t> diff_result;
        // std::vector<uint64_t> vec1(unique_ROERT_keys.begin(), unique_ROERT_keys.end());
        // std::vector<uint64_t> vec2(unique_ERT_keys.begin(), unique_ERT_keys.end());
        // std::sort(vec1.begin(), vec1.end());
        // std::sort(vec2.begin(), vec2.end());
        // std::set_symmetric_difference(vec1.begin(), vec1.end(),
        //                               vec2.begin(), vec2.end(),
        //                               std::back_inserter(diff_result));
        // std::cout << "发现 " << diff_result.size() << " 个不一样的值：" << std::endl;
        // for (const auto& val : diff_result) {
        //     std::cout << std::dec << val << std::endl;
        // }

        ResultExporter::print_summary(results, log);
        log.log_section("   >>> 各测试索引结构性能对比完成");
        log.log_Line_break();

        log.log_section("   >>> 开始释放内存...");
        fast_free();
        log.log_section("   >>> 释放内存完成");
        log.log_section("   >>> 测试完成");

        // 记录结束时间
        time_t end_now = time(nullptr);
        char end_time_str[100];
        strftime(end_time_str, sizeof(end_time_str), "%Y-%m-%d %H:%M:%S\n", localtime(&end_now));
        log.log_info("  |-> 测试索引数量", to_string(testers.size()));
        if (g_config.test_ycsb) {
            log.log_info("  |-> YCSB 测试", "workload=" + g_config.ycsb_workload + " (" + get_ycsb_workload_description(g_config.ycsb_workload) + "), Load 操作数=" + to_string(g_config.ycsb_operations["Load"]) + ", Run 操作数=" + to_string(g_config.ycsb_operations["Run"]) + ", 线程数=" + to_string(g_config.ycsb_threads));
        } else {
            log.log_info("  |-> 测试数据集", "Dense and Sparse testNum=" + to_string(g_config.test_num) + ", Clustered testNum=" + to_string(g_config.clustered_num));
        }
        log.log_info("  |-> 测试结束时间", end_time_str);

        cout << "测试完成!\n"
             << endl;
    } catch (const exception& e) {
        cerr << "测试出错: " << e.what() << endl;
        return 1;
    }

    return 0;
}
