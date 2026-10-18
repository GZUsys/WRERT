//
//  core_workload.cc
//  YCSB-C
//
//  Created by Jinglei Ren on 12/9/14.
//  Copyright (c) 2014 Jinglei Ren <jinglei@ren.systems>.
//

#include "uniform_generator.h"
#include "zipfian_generator.h"
#include "scrambled_zipfian_generator.h"
#include "skewed_latest_generator.h"
#include "const_generator.h"
#include "core_workload.h"

#include <string>
#include <random>
#include <unordered_set>
#include <algorithm>

using ycsbc::CoreWorkload;
using std::string;

const string CoreWorkload::TABLENAME_PROPERTY = "table";
const string CoreWorkload::TABLENAME_DEFAULT = "usertable";

const string CoreWorkload::FIELD_COUNT_PROPERTY = "fieldcount";
const string CoreWorkload::FIELD_COUNT_DEFAULT = "10";

const string CoreWorkload::FIELD_LENGTH_DISTRIBUTION_PROPERTY =
  "field_len_dist";
const string CoreWorkload::FIELD_LENGTH_DISTRIBUTION_DEFAULT = "constant";

const string CoreWorkload::FIELD_LENGTH_PROPERTY = "fieldlength";
const string CoreWorkload::FIELD_LENGTH_DEFAULT = "100";

const string CoreWorkload::READ_ALL_FIELDS_PROPERTY = "readallfields";
const string CoreWorkload::READ_ALL_FIELDS_DEFAULT = "true";

const string CoreWorkload::WRITE_ALL_FIELDS_PROPERTY = "writeallfields";
const string CoreWorkload::WRITE_ALL_FIELDS_DEFAULT = "false";

const string CoreWorkload::READ_PROPORTION_PROPERTY = "readproportion";
const string CoreWorkload::READ_PROPORTION_DEFAULT = "0.95";

const string CoreWorkload::UPDATE_PROPORTION_PROPERTY = "updateproportion";
const string CoreWorkload::UPDATE_PROPORTION_DEFAULT = "0.05";

const string CoreWorkload::INSERT_PROPORTION_PROPERTY = "insertproportion";
const string CoreWorkload::INSERT_PROPORTION_DEFAULT = "0.0";

const string CoreWorkload::SCAN_PROPORTION_PROPERTY = "scanproportion";
const string CoreWorkload::SCAN_PROPORTION_DEFAULT = "0.0";

const string CoreWorkload::READMODIFYWRITE_PROPORTION_PROPERTY =
  "readmodifywriteproportion";
const string CoreWorkload::READMODIFYWRITE_PROPORTION_DEFAULT = "0.0";

const string CoreWorkload::REQUEST_DISTRIBUTION_PROPERTY =
  "requestdistribution";
const string CoreWorkload::REQUEST_DISTRIBUTION_DEFAULT = "uniform";

const string CoreWorkload::ZERO_PADDING_PROPERTY = "zeropadding";
const string CoreWorkload::ZERO_PADDING_DEFAULT = "1";

const string CoreWorkload::MAX_SCAN_LENGTH_PROPERTY = "maxscanlength";
const string CoreWorkload::MAX_SCAN_LENGTH_DEFAULT = "1000";

const string CoreWorkload::SCAN_LENGTH_DISTRIBUTION_PROPERTY =
  "scanlengthdistribution";
const string CoreWorkload::SCAN_LENGTH_DISTRIBUTION_DEFAULT = "uniform";

const string CoreWorkload::INSERT_ORDER_PROPERTY = "insertorder";
const string CoreWorkload::INSERT_ORDER_DEFAULT = "hashed";

const string CoreWorkload::INSERT_START_PROPERTY = "insertstart";
const string CoreWorkload::INSERT_START_DEFAULT = "0";

const string CoreWorkload::RECORD_COUNT_PROPERTY = "recordcount";
const string CoreWorkload::OPERATION_COUNT_PROPERTY = "operationcount";

void CoreWorkload::Init(const utils::Properties& p) {
    table_name_ = p.GetProperty(TABLENAME_PROPERTY, TABLENAME_DEFAULT);

    field_count_ = std::stoi(p.GetProperty(FIELD_COUNT_PROPERTY,
                                           FIELD_COUNT_DEFAULT));
    field_len_generator_ = GetFieldLenGenerator(p);

    double read_proportion = std::stod(p.GetProperty(READ_PROPORTION_PROPERTY,
                                                     READ_PROPORTION_DEFAULT));
    double update_proportion = std::stod(p.GetProperty(UPDATE_PROPORTION_PROPERTY,
                                                       UPDATE_PROPORTION_DEFAULT));
    double insert_proportion = std::stod(p.GetProperty(INSERT_PROPORTION_PROPERTY,
                                                       INSERT_PROPORTION_DEFAULT));
    double scan_proportion = std::stod(p.GetProperty(SCAN_PROPORTION_PROPERTY,
                                                     SCAN_PROPORTION_DEFAULT));
    double readmodifywrite_proportion = std::stod(p.GetProperty(
      READMODIFYWRITE_PROPORTION_PROPERTY, READMODIFYWRITE_PROPORTION_DEFAULT));

    record_count_ = std::stoi(p.GetProperty(RECORD_COUNT_PROPERTY));
    std::string request_dist = p.GetProperty(REQUEST_DISTRIBUTION_PROPERTY,
                                             REQUEST_DISTRIBUTION_DEFAULT);
    zero_padding_ = std::stoi(p.GetProperty(ZERO_PADDING_PROPERTY, ZERO_PADDING_DEFAULT));
    int max_scan_len = std::stoi(p.GetProperty(MAX_SCAN_LENGTH_PROPERTY,
                                               MAX_SCAN_LENGTH_DEFAULT));
    std::string scan_len_dist = p.GetProperty(SCAN_LENGTH_DISTRIBUTION_PROPERTY,
                                              SCAN_LENGTH_DISTRIBUTION_DEFAULT);
    int insert_start = std::stoi(p.GetProperty(INSERT_START_PROPERTY,
                                               INSERT_START_DEFAULT));

    read_all_fields_ = utils::StrToBool(p.GetProperty(READ_ALL_FIELDS_PROPERTY,
                                                      READ_ALL_FIELDS_DEFAULT));
    write_all_fields_ = utils::StrToBool(p.GetProperty(WRITE_ALL_FIELDS_PROPERTY,
                                                       WRITE_ALL_FIELDS_DEFAULT));

    if (p.GetProperty(INSERT_ORDER_PROPERTY, INSERT_ORDER_DEFAULT) == "hashed") {
        ordered_inserts_ = false;
    } else {
        ordered_inserts_ = true;
    }

    key_generator_ = new CounterGenerator(insert_start);

    if (read_proportion > 0) {
        op_chooser_.AddValue(READ, read_proportion);
    }
    if (update_proportion > 0) {
        op_chooser_.AddValue(UPDATE, update_proportion);
    }
    if (insert_proportion > 0) {
        op_chooser_.AddValue(INSERT, insert_proportion);
    }
    if (scan_proportion > 0) {
        op_chooser_.AddValue(SCAN, scan_proportion);
    }
    if (readmodifywrite_proportion > 0) {
        op_chooser_.AddValue(READMODIFYWRITE, readmodifywrite_proportion);
    }

    insert_key_sequence_.Set(record_count_);

    if (request_dist == "uniform") {
        key_chooser_ = new UniformGenerator(0, record_count_ - 1);

    } else if (request_dist == "zipfian") {
        // If the number of keys changes, we don't want to change popular keys.
        // So we construct the scrambled zipfian generator with a keyspace
        // that is larger than what exists at the beginning of the test.
        // If the generator picks a key that is not inserted yet, we just ignore it
        // and pick another key.
        int op_count = std::stoi(p.GetProperty(OPERATION_COUNT_PROPERTY));
        int new_keys = (int)(op_count * insert_proportion * 2); // a fudge factor
        key_chooser_ = new ScrambledZipfianGenerator(record_count_ + new_keys);

    } else if (request_dist == "latest") {
        key_chooser_ = new SkewedLatestGenerator(insert_key_sequence_);

    } else {
        throw utils::Exception("Unknown request distribution: " + request_dist);
    }

    field_chooser_ = new UniformGenerator(0, field_count_ - 1);

    if (scan_len_dist == "uniform") {
        scan_len_chooser_ = new UniformGenerator(1, max_scan_len);
    } else if (scan_len_dist == "zipfian") {
        scan_len_chooser_ = new ZipfianGenerator(1, max_scan_len);
    } else {
        throw utils::Exception("Distribution not allowed for scan length: " + scan_len_dist);
    }
}

ycsbc::Generator<uint64_t>* CoreWorkload::GetFieldLenGenerator(
  const utils::Properties& p) {
    string field_len_dist = p.GetProperty(FIELD_LENGTH_DISTRIBUTION_PROPERTY,
                                          FIELD_LENGTH_DISTRIBUTION_DEFAULT);
    int field_len = std::stoi(p.GetProperty(FIELD_LENGTH_PROPERTY,
                                            FIELD_LENGTH_DEFAULT));
    if (field_len_dist == "constant") {
        return new ConstGenerator(field_len);
    } else if (field_len_dist == "uniform") {
        return new UniformGenerator(1, field_len);
    } else if (field_len_dist == "zipfian") {
        return new ZipfianGenerator(1, field_len);
    } else {
        throw utils::Exception("Unknown field length distribution: " + field_len_dist);
    }
}

void CoreWorkload::BuildValues(std::vector<ycsbc::DB::KVPair>& values) {
    for (int i = 0; i < field_count_; ++i) {
        ycsbc::DB::KVPair pair;
        pair.first.append("field").append(std::to_string(i));
        pair.second.append(field_len_generator_->Next(), utils::RandomPrintChar());
        values.push_back(pair);
    }
}

void CoreWorkload::BuildUpdate(std::vector<ycsbc::DB::KVPair>& update) {
    ycsbc::DB::KVPair pair;
    pair.first.append(NextFieldName());
    pair.second.append(field_len_generator_->Next(), utils::RandomPrintChar());
    update.push_back(pair);
}

void CoreWorkload::GenerateSparseDataset(size_t count) {
    printf("  >>> 生成 Sparse 数据集 (count = %zu)\n", count);

    std::random_device rd;
    rng r;
    // rng_init(&r, rd(), 2);
    rng_init(&r, 1, 2);

    sparse_dataset_sort_.clear();
    sparse_dataset_sort_.reserve(count);
    sparse_dataset_new_.clear();
    sparse_dataset_new_.reserve(count * 0.5);
    sparse_dataset_.clear();
    sparse_dataset_.reserve(count);

    uint64_t min_key = std::numeric_limits<uint64_t>::max();
    uint64_t max_key = 0;

    for (uint64_t i = 0; i < count; ++i) {
        // for (uint64_t i = 0; i < count + count * 0.5; ++i) {
        uint64_t key = rng_next(&r);
        // if (i < count) {
        sparse_dataset_.emplace_back(key, i);
        sparse_dataset_sort_.emplace_back(key, i);
        min_key = std::min(min_key, key);
        max_key = std::max(max_key, key);
        // } else {
        //     sparse_dataset_new_.emplace_back(key, i);
        // }
    }

    std::sort(sparse_dataset_sort_.begin(), sparse_dataset_sort_.end());

    printf("  |-> Sparse 数据集总量(请求数): %zu\n", count);
    printf("  |-> Key 取值区间: [%llu, %llu]\n",
           (unsigned long long)min_key, (unsigned long long)max_key);

    for (uint64_t i = 0; i < 50; ++i) {
        printf("  |-> [%zu] key = %llu, value = %llu\n", i,
               (unsigned long long)sparse_dataset_[i].first,
               (unsigned long long)sparse_dataset_[i].second);
    }

    txn_value_generator_ = new UniformGenerator(0, UINT64_MAX);
    txn_key_generator_ = new UniformGenerator(0, UINT64_MAX);
    txn_zipf_gen_.reset(new ZipfianGenerator(count));
    record_count_ = count;
    insert_key_sequence_.Set(record_count_);

    printf("  |-> ......\n");
    printf("  >>> Sparse 数据集生成完成\n");
}
