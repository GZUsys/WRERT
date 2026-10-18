#!/bin/bash

make clean
make

rm -rf /pmem0/wrert/*
# rm -rf ./Log/wrert/*
# rm -rf ./Log/ert/*
# rm -rf ./Log/wort/*
# rm -rf ./Log/woart/*

# indices=("wort" "roart" "ert" "wrert" "woart")
# indices=("ert" "wrert")
indices=("wrert")
# workloads=("workloada" "workloadb" "workloadc" "workloadd" "workloade" "workloadf")
workloads=("workloada" "workloadb" "workloadc")
# workloads=("workloadc")

# 单线程测试
# for idx in "${indices[@]}"; do
#     for workload in "${workloads[@]}"; do
#         for i in {1..1}; do
#             echo ">>> $idx $workload 第 $i 次 YCSB 测试 <<<" >> ./Log/"$idx"/"$idx"_"$workload"_res.txt 2>&1
#             file_name="./workloads/$workload.spec"
#             ./ycsbc -db $idx -threads 1 -P $file_name >> "./Log/"$idx"/"$idx"_"$workload"_res.txt" 2>&1
#             echo ">>> $idx $workload 第 $i 次 YCSB 测试结束 <<<" >> ./Log/"$idx"/"$idx"_"$workload"_res.txt 2>&1
#             rm -rf /pmem0/wrert/*
#             wait
#         done
#     done
# done


# threads=("1" "2" "4" "8" "12" "16" "20" "24" "28" "32" "36" "40")
# threads=("8" "12" "16" "20" "24" "28" "32" "36" "40")
threads=("2")

# rm -rf ./Log/roart/multi_thread/workloada/*
# rm -rf ./Log/roart/multi_thread/workloadb/*
# rm -rf ./Log/roart/multi_thread/workloadc/*
# rm -rf ./Log/roart/multi_thread/workloadd/*
# rm -rf ./Log/roart/multi_thread/workloade/*
# rm -rf ./Log/roart/multi_thread/workloadf/*

# rm -rf ./Log/ert/multi_thread/workloada/*
# rm -rf ./Log/ert/multi_thread/workloadb/*
# rm -rf ./Log/ert/multi_thread/workloadc/*
# rm -rf ./Log/ert/multi_thread/workloadd/*
# rm -rf ./Log/ert/multi_thread/workloade/*
# rm -rf ./Log/ert/multi_thread/workloadf/*

# rm -rf ./Log/wrert/multi_thread/workloada/*
# rm -rf ./Log/wrert/multi_thread/workloadb/*
# rm -rf ./Log/wrert/multi_thread/workloadc/*
# rm -rf ./Log/wrert/multi_thread/workloadd/*
# rm -rf ./Log/wrert/multi_thread/workloade/*
# rm -rf ./Log/wrert/multi_thread/workloadf/*

for idx in "${indices[@]}"; do
    for workload in "${workloads[@]}"; do
        for thread in "${threads[@]}"; do
            for i in {1..1}; do
                rm -rf /pmem0/wrert/*
                echo ">>> $idx $workload 第 $i 次 YCSB: 测试 $(date) <<<" >> ./Log/"$idx"/multi_thread/"$workload"/"$idx"_thread"$thread"_res.txt 2>&1
                file_name="./workloads/$workload.spec"
                ./ycsbc -db $idx -threads $thread -P $file_name >> "./Log/"$idx"/multi_thread/"$workload"/"$idx"_thread"$thread"_res.txt" 2>&1
                echo ">>> $idx $workload 第 $i 次 YCSB 测试结束: $(date) <<<" >> ./Log/"$idx"/multi_thread/"$workload"/"$idx"_thread"$thread"_res.txt 2>&1
                wait
            done
        done
    done
done

# for i in {1..2}; do
#     ./ycsbc -db woart -threads 1 -P ./workloads/workloade.spec >> "./Log/woart/woart_workloade_res.txt" 2>&1
#     rm -rf /pmem0/wrert/*
#     wait
# done
