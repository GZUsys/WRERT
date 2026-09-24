# WRERT
WRERT: Write and Range-Query Optimized Extendible RadixTree for Persistent Memory

### Dependence 

#### Hardware
1. [Intel® Xeon® Platinum Processors](https://www.intel.com/content/www/us/en/products/details/processors/xeon/scalable/platinum.html)
2. [Intel Optane DCPMM](https://www.intel.com/content/www/us/en/products/docs/memory-storage/optane-persistent-memory/overview.html)                                                                                                             

#### Software
- **OS**: Linux (Ubuntu 18.04/20.04+ recommended, requires `ext4` or `xfs` filesystem mounted with `DAX` mode)
- **Compiler**: GCC 7+ or Clang 5+ (must support C++11/14 and `-march=native` instruction set optimizations)
- **Build System**: CMake (Version requirement: `3.10...3.19`)
- **Libraries**: `pthread`
  
### Build and Run

#### 1. Environment Setup (PMem Path & Log Directories)
Before compiling and running, ensure that the Intel Optane DCPMM is mounted to `/pmem0` with **DAX (Direct Access)** mode enabled. Then, execute the following commands to create the required PMem directory, assign proper permissions, and clean up historical logs:

```bash
# 1. Create the PMem test directory and grant read/write/execute permissions to the current user
sudo mkdir -p /pmem0/wrert/
sudo chown -R $USER:$USER /pmem0/wrert/
sudo chmod -R 755 /pmem0/wrert/

# 2. Create log directories and clean up old test logs
mkdir -p ./Log
```

#### 2. Micro-Benchmark running
```bash
./run.sh
```
### Contacts
- **Hai Yang**: gs.haiyang24@gzu.edu.cn

### Reference

