# Marco-SymFit 编译指南

## 目录结构

```
Marco-SymFit/
├── marco/
│   ├── fastgen/          # FastGen 二进制（运行使用）
│   ├── scheduler/        # 调度器脚本（运行使用）
│   └── src/              # FastGen 源代码（修改和编译）
│       ├── CE/           # FastGen 核心代码
│       └── scheduler/    # 调度器源代码
├── symfit/
│   └── build/            # SymFit 二进制（运行使用）
└── symfit-source/        # SymFit 源代码（修改和编译）
```

## 完整工作流程

### 1. 修改源代码

- **FastGen 源代码**：`marco/src/CE/`
- **Scheduler 源代码**：`marco/src/scheduler/`
- **SymFit 源代码**：`symfit-source/`

### 2. 重新编译 FastGen

```bash
cd Marco-SymFit/marco/src/CE

# 方法1: 使用构建脚本（推荐）
bash build/build.sh

# 方法2: 手动编译
cd fuzzer/cpp_core
rm -rf build && mkdir build && cd build
cmake .. && make -j
cd ../../..
cargo build --release

# 复制编译后的二进制到运行目录
cp target/release/fastgen ../../fastgen/fastgen
```

### 3. 重新编译 SymFit

```bash
cd Marco-SymFit/symfit-source

# 初始化子模块（如果需要）
git submodule update --init --recursive

# 运行构建脚本
./build.sh

# 或者手动编译
./configure --target-list=x86_64-linux-user
make -j

# 复制编译后的二进制到运行目录
# fgtest
cp build/symsan/bin/fgtest ../symfit/build/symsan/bin/fgtest
# symqemu-x86_64
cp build/symfit-symsan/x86_64-linux-user/symqemu-x86_64 \
   ../symfit/build/symfit-symsan/x86_64-linux-user/symqemu-x86_64
```

### 4. 运行测试

```bash
cd Marco-SymFit/test
bash run_marco_symfit_integration.sh
```

## 快速编译脚本

项目提供了便捷的编译脚本：

### 编译所有组件（推荐）

```bash
cd Marco-SymFit
./build_all.sh
```

这会依次编译 FastGen 和 SymFit。

### 分别编译各个组件

```bash
# 只编译 FastGen
./build_fastgen.sh

# 只编译 SymFit
./build_symfit.sh
```

## 注意事项

1. **源代码目录** (`marco/src/`, `symfit-source/`) 用于开发和修改
2. **二进制目录** (`marco/fastgen/`, `symfit/build/`) 用于运行
3. **修改源代码后必须重新编译**并复制二进制文件到运行目录
4. **测试脚本会自动使用运行目录中的二进制**，无需修改路径
5. 所有路径都是相对于 `Marco-SymFit` 根目录的，项目可以独立运行

