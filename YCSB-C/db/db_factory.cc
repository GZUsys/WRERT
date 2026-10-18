//
//  basic_db.cc
//  YCSB-C
//

#include "db/db_factory.h"

#include <string>

#include "db/basic_db.h"
#include "db/lock_stl_db.h"
#include "db/redis_db.h"
#include "db/tbb_rand_db.h"
#include "db/tbb_scan_db.h"

#include "db/wrert_db.h"
#include "db/ert_db.h"
#include "db/roart_db.h"
#include "db/wort_db.h"
#include "db/woart_db.h"

using namespace std;
using namespace wrert;
using ycsbc::DB;
using ycsbc::DBFactory;

DB* DBFactory::CreateDB(utils::Properties& props) {
    if (props["dbname"] == "basic") {
        return new BasicDB;
    } else if (props["dbname"] == "lock_stl") {
        return new LockStlDB;
    } else if (props["dbname"] == "redis") {
        int port = stoi(props["port"]);
        int slaves = stoi(props["slaves"]);
        return new RedisDB(props["host"].c_str(), port, slaves);
    } else if (props["dbname"] == "tbb_rand") {
        return new TbbRandDB;
    } else if (props["dbname"] == "tbb_scan") {
        return new TbbScanDB;
    } else if (props["dbname"] == "wrert") {
        return new WRERTDB;
    } else if (props["dbname"] == "ert") {
        return new ERTDB;
    } else if (props["dbname"] == "roart") {
        return new ROARTDB;
    } else if (props["dbname"] == "wort") {
        return new WORTDB;
    } else if (props["dbname"] == "woart") {
        return new WOARTDB;
    } else {
        return NULL;
    }
}
