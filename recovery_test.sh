#!/bin/bash

cmake .
make clean
make

rm -rf /pmem0/wrert/*
rm -rf ./Log/wrert/wrert_recovery.txt

data_sizes=(10 20 30 40 50 60 70 80 90 100)
# data_sizes=(100)

echo "==========================================" >> ./Log/wrert/wrert_recovery.txt 2>&1
echo "  WRERT 崩溃恢复实验 (Crash Recovery Test)" >> ./Log/wrert/wrert_recovery.txt 2>&1
echo "==========================================" >> ./Log/wrert/wrert_recovery.txt 2>&1

for i in {1..5}; do
    for size in "${data_sizes[@]}"; do
        test_num="${size}000000"
        
        echo "开始测试 第 $i 次 数据量: ${size}M (${test_num} keys)" >> ./Log/wrert/wrert_recovery.txt 2>&1

        echo -e "\033[31m[${size}M] 首次运行，正在插入数据并注入崩溃...\033[0m"
        echo "[${size}M] 首次运行，正在插入数据并注入崩溃..." >> ./Log/wrert/wrert_recovery.txt 2>&1
        ./nvmkv $test_num /pmem0/wrert/ >> ./Log/wrert/wrert_recovery.txt 2>&1

        sleep 3

        echo -e "\033[32m[${size}M] 重启程序，正在执行崩溃恢复...\033[0m" 
        echo "[${size}M] 重启程序，正在执行崩溃恢复..." >> ./Log/wrert/wrert_recovery.txt 2>&1
        ./nvmkv $test_num /pmem0/wrert/ >> ./Log/wrert/wrert_recovery.txt 2>&1
        
        echo "第 $i 次 测试完成: ${size}M" >> ./Log/wrert/wrert_recovery.txt 2>&1
        echo "-------------------------------------------------------------" >> ./Log/wrert/wrert_recovery.txt 2>&1

        rm -rf /pmem0/wrert/*

    done
done