#!/usr/bin/env bash

# 编译项目
cmake .
make clean
make

rm -rf /pmem0/roert/*

# rm -rf ./Log/roert/*
# rm -rf ./Log/ert/*
# rm -rf ./Log/wort/*
# rm -rf ./Log/woart/*
# rm -rf ./Log/roart/*

# rm -rf ./Log/roert/*_res.txt
# rm -rf ./Log/ert/*_res.txt
# rm -rf ./Log/wort/*_res.txt
# rm -rf ./Log/woart/*_res.txt
# rm -rf ./Log/roart/*_res.txt
# rm -rf ./Log/roert/*_latencyBreakdown.txt
# rm -rf ./Log/ert/*_latencyBreakdown.txt
# rm -rf ./Log/wort/*_latencyBreakdown.txt
# rm -rf ./Log/woart/*_latencyBreakdown.txt
# rm -rf ./Log/roart/*_latencyBreakdown.txt

# rm -rf ./Log/roert/*base*.txt
# rm -rf ./Log/ert/*base*.txt
# rm -rf ./Log/wort/*base*.txt
# rm -rf ./Log/woart/*base*.txt
# rm -rf ./Log/roart/*base*.txt

# rm -rf ./Log/*_test_log.txt
rm -rf ./Log/res.txt
# rm -rf ./Log/ranInsetPer10M.txt

./nvmkv 300 /pmem0/roert/ >> ./Log/res.txt 2>&1

# for i in {1..2}; do
#     ./nvmkv 10000000 /pmem0/roert/ >> ./Log/res.txt 2>&1
# done

# indices=("wort" "roart" "ert" "roert" "woart")
indices=("ert")
# test_types=("seq" "ran")
test_types=("ran")

# for type in "${test_types[@]}"; do
#     for i in {1..5}; do
#         ./nvmkv 100000000 /pmem0/roert/ "$type" >> ./Log/"$type"_res.txt 2>&1
#         rm -rf /pmem0/roert/*
#         wait
#     done
# done

# 基准性能读写测试
# for idx in "${indices[@]}"; do
#     for type in "${test_types[@]}"; do
#         for i in {1..5}; do
#             echo ">>> $idx $type 1 第 $i 次基准性能读写测试 <<<" >> ./Log/"$idx"/"$idx"_"$type"_base_res.txt 2>&1
#             ./nvmkv 100000000 /pmem0/roert/ "$idx" "$type" >> ./Log/"$idx"/"$idx"_"$type"_base_res.txt 2>&1
#             rm -rf /pmem0/roert/*
#             wait
#             echo ">>> $idx $type 1 第 $i 次测试循环结束 <<<" >> ./Log/"$idx"/"$idx"_"$type"_base_res.txt 2>&1
#         done
#     done
# done

# 段大小对性能的影响测试
# for idx in "${indices[@]}"; do
#     for type in "${test_types[@]}"; do
#         for i in {1..1}; do
#             echo ">>> $idx $type 1 第 $i 次段大小对性能的影响测试 <<<" >> ./Log/"$idx"/"$idx"_"$type"_segment_res.txt 2>&1
#             ./nvmkv 100000000 /pmem0/roert/ "$idx" "$type" >> ./Log/"$idx"/"$idx"_"$type"_segment_res.txt 2>&1
#             rm -rf /pmem0/roert/*
#             wait
#             echo ">>> $idx $type 1 第 $i 次测试循环结束 <<<" >> ./Log/"$idx"/"$idx"_"$type"_segment_res.txt 2>&1
#         done
#     done
# done

# 各个操作明细测试
# for idx in "${indices[@]}"; do
#     for type in "${test_types[@]}"; do
#         for i in {1..5}; do
#             echo ">>> $idx $type 1 第 $i 次各个操作明细测试 <<<" >> ./Log/"$idx"/"$idx"_"$type"_res.txt 2>&1
#             ./nvmkv 100000000 /pmem0/roert/ "$idx" "$type" >> ./Log/"$idx"/"$idx"_"$type"_res.txt 2>&1
#             rm -rf /pmem0/roert/*
#             wait
#             echo ">>> $idx $type 1 第 $i 次测试循环结束 <<<" >> ./Log/"$idx"/"$idx"_"$type"_res.txt 2>&1
#         done
#     done
# done

# 每随机插入10M键测试
# for i in {1..5}; do
#     echo ">>> 第 $i 次每随机插入10M键测试 <<<" >> ./Log/ranInsetPer10M.txt 2>&1
#     ./nvmkv 100000000 /pmem0/roert/ >> ./Log/ranInsetPer10M.txt 2>&1
#     rm -rf /pmem0/roert/*
#     wait
#     echo ">>> 第 $i 次测试循环结束 <<<" >> ./Log/ranInsetPer10M.txt 2>&1
# done

# 范围查询测试
selectivities=(1)
# selectivities=(0.001 0.005 0.01 0.02)
# selectivities=(0.2 0.4 0.6 0.8)
# selectivities=(0.2)
# for idx in "${indices[@]}"; do
#     for type in "${test_types[@]}"; do
#         for sel in "${selectivities[@]}"; do
#             for i in {1..1}; do
#                 echo ">>> $idx $type $sel 第 $i 次范围查询测试 <<<" >> ./Log/res.txt 2>&1
#                 ./nvmkv 100000000 /pmem0/roert/ "$idx" "$sel" "$type" >> ./Log/res.txt 2>&1
#                 rm -rf /pmem0/roert/*
#                 wait
#                 echo ">>> $idx $type $sel 第 $i 次测试循环结束 <<<" >> ./Log/res.txt 2>&1
#             done
#         done
#     done
# done
