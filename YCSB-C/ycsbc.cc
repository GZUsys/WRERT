//
//  ycsbc.cc
//  YCSB-C
//

#include <cstring>
#include <string>
#include <iostream>
#include <vector>
#include <future>
#include <typeinfo>
#include <pthread.h>
#include <unistd.h>

#include "core/utils.h"
#include "core/timer.h"
#include "core/client.h"
#include "core/core_workload.h"
#include "db/db_factory.h"

#include "db/wrert_db.h"

using namespace std;

void UsageMessage(const char* command);
bool StrStartWith(const char* str, const char* pre);
string ParseCommandLine(int argc, const char* argv[], utils::Properties& props);

int DelegateClient(ycsbc::DB* db, ycsbc::CoreWorkload* wl, const int num_ops, const string& nvm_filename,
                   bool is_loading) {
    init_fast_allocator(true, true, nvm_filename.c_str());

    ycsbc::Client client(*db, *wl);
    int oks = 0;
    for (int i = 0; i < num_ops; ++i) {
        if (is_loading) {
            oks += client.DoInsert();
        } else {
            oks += client.DoTransaction();
        }
    }

    fast_free();
    return oks;
}

int main(const int argc, const char* argv[]) {
    utils::Properties props;
    string file_name = ParseCommandLine(argc, argv, props);

    string pm_path = "/pmem0/wrert/";

    string nvm_filename = pm_path + to_string(0) + "/";
    printf("Main thread nvm_filename: %s\n", nvm_filename.c_str());
    system(("mkdir -p " + nvm_filename).c_str());
    init_fast_allocator(true, true, nvm_filename.c_str());

    ycsbc::DB* db = ycsbc::DBFactory::CreateDB(props);

    if (!db) {
        cout << "Unknown database name " << props["dbname"] << endl;
        exit(0);
    }

    ycsbc::CoreWorkload wl;
    wl.Init(props);

    const int num_threads = stoi(props.GetProperty("threadcount", "1"));
    std::cout << "num_threads: " << num_threads << std::endl;

    vector<future<int>> actual_ops;
    int total_ops = stoi(props[ycsbc::CoreWorkload::RECORD_COUNT_PROPERTY]);
    std::cout << "total_ops: " << total_ops << std::endl;

    wl.GenerateSparseDataset(total_ops);
    db->Init();

    printf("  >>> Loading data...\n");
    int ops_per_thread = total_ops / num_threads;
    int my_ops = 0;
    utils::Timer<double> timer;

    for (int i = 0; i < num_threads; ++i) {
        nvm_filename = pm_path + to_string(i + 1) + "/";
        system(("mkdir -p " + nvm_filename).c_str());
    }

    timer.Start();
    for (int i = 0; i < num_threads; ++i) {
        // actual_ops.emplace_back(async(launch::async,
        //   DelegateClient, db, &wl, total_ops / num_threads, true));

        nvm_filename = pm_path + to_string(i + 1) + "/";
        // system(("mkdir -p " + nvm_filename).c_str());
        my_ops = (i == num_threads - 1) ? (total_ops - ops_per_thread * (num_threads - 1)) : ops_per_thread;
        actual_ops.emplace_back(async(launch::async,
                                      DelegateClient, db, &wl, my_ops, nvm_filename, true));
        // printf("Thread %d, my_ops = %d, ops_per_thread = %d, actual_ops.size() = %d\n", i + 1, my_ops, ops_per_thread, actual_ops.size());
        // DelegateClient(db, &wl, my_ops, nvm_filename, true);
    }
    assert((int)actual_ops.size() == num_threads);
    int sum = 0;
    for (auto& n : actual_ops) {
        assert(n.valid());
        sum += n.get();
    }
    double duration = timer.End();

    printf("Multi-thread Load Total latency: %.6f s, Throughput: %.6f MOPS, Loading records: %d\n",
           duration, static_cast<double>(total_ops) / duration / 1000000.0, sum);
    printf("Load Total latency: %.6f s, Throughput: %.6f MOPS, OpCount: %d\n",
           db->GetTotalLatencyS(), db->GetThroughputMops(), db->GetOpCount());

    // db->ScanUint64(wl.NextTable(), 0, 9223371979975689957);
    db->ResetStats();
    actual_ops.clear();

    printf("  >>> Performing transactions...\n");
    total_ops = stoi(props[ycsbc::CoreWorkload::OPERATION_COUNT_PROPERTY]);
    ops_per_thread = total_ops / num_threads;

    timer.Start();
    for (int i = 0; i < num_threads; ++i) {
        // actual_ops.emplace_back(async(launch::async,
        //                               DelegateClient, db, &wl, total_ops / num_threads, false));
        nvm_filename = pm_path + to_string(i + 1) + "/";
        my_ops = (i == num_threads - 1) ? (total_ops - ops_per_thread * (num_threads - 1)) : ops_per_thread;
        actual_ops.emplace_back(async(launch::async,
                                      DelegateClient, db, &wl, my_ops, nvm_filename, false));
        // printf("Thread %d, my_ops = %d, ops_per_thread = %d, actual_ops.size() = %d\n", i + 1, my_ops, ops_per_thread, actual_ops.size());
        // DelegateClient(db, &wl, my_ops, nvm_filename, false);
    }
    assert((int)actual_ops.size() == num_threads);
    sum = 0;
    for (auto& n : actual_ops) {
        assert(n.valid());
        sum += n.get();
    }
    duration = timer.End();

    printf("Multi-thread Transaction Total latency: %.6f s, Throughput: %.6f MOPS, Transaction records: %d\n",
           duration, static_cast<double>(total_ops) / duration / 1000000.0, sum);
    printf("Transaction Total latency: %.6f s, Throughput: %.6f MOPS, OpCount: %d\n",
           db->GetTotalLatencyS(), db->GetThroughputMops(), db->GetOpCount());

    // db->ScanUint64(wl.NextTable(), 0, 9223371979975689957);

    db->Close();
    delete db;
    fast_free();

    // cerr << "# Transaction throughput (KTPS)" << endl;
    // cerr << props["dbname"] << '\t' << file_name << '\t' << num_threads << '\t';
    // cerr << total_ops / duration / 1000 << endl;
}

string ParseCommandLine(int argc, const char* argv[], utils::Properties& props) {
    int argindex = 1;
    string filename;
    while (argindex < argc && StrStartWith(argv[argindex], "-")) {
        if (strcmp(argv[argindex], "-threads") == 0) {
            argindex++;
            if (argindex >= argc) {
                UsageMessage(argv[0]);
                exit(0);
            }
            props.SetProperty("threadcount", argv[argindex]);
            argindex++;
        } else if (strcmp(argv[argindex], "-db") == 0) {
            argindex++;
            if (argindex >= argc) {
                UsageMessage(argv[0]);
                exit(0);
            }
            props.SetProperty("dbname", argv[argindex]);
            argindex++;
        } else if (strcmp(argv[argindex], "-host") == 0) {
            argindex++;
            if (argindex >= argc) {
                UsageMessage(argv[0]);
                exit(0);
            }
            props.SetProperty("host", argv[argindex]);
            argindex++;
        } else if (strcmp(argv[argindex], "-port") == 0) {
            argindex++;
            if (argindex >= argc) {
                UsageMessage(argv[0]);
                exit(0);
            }
            props.SetProperty("port", argv[argindex]);
            argindex++;
        } else if (strcmp(argv[argindex], "-slaves") == 0) {
            argindex++;
            if (argindex >= argc) {
                UsageMessage(argv[0]);
                exit(0);
            }
            props.SetProperty("slaves", argv[argindex]);
            argindex++;
        } else if (strcmp(argv[argindex], "-P") == 0) {
            argindex++;
            if (argindex >= argc) {
                UsageMessage(argv[0]);
                exit(0);
            }
            filename.assign(argv[argindex]);
            ifstream input(argv[argindex]);
            try {
                props.Load(input);
            } catch (const string& message) {
                cout << message << endl;
                exit(0);
            }
            input.close();
            argindex++;
        } else {
            cout << "Unknown option '" << argv[argindex] << "'" << endl;
            exit(0);
        }
    }

    if (argindex == 1 || argindex != argc) {
        UsageMessage(argv[0]);
        exit(0);
    }

    return filename;
}

void UsageMessage(const char* command) {
    cout << "Usage: " << command << " [options]" << endl;
    cout << "Options:" << endl;
    cout << "  -threads n: execute using n threads (default: 1)" << endl;
    cout << "  -db dbname: specify the name of the DB to use (default: basic)" << endl;
    cout << "  -P propertyfile: load properties from the given file. Multiple files can" << endl;
    cout << "                   be specified, and will be processed in the order specified" << endl;
}

inline bool StrStartWith(const char* str, const char* pre) {
    return strncmp(str, pre, strlen(pre)) == 0;
}
