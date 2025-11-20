# Marco-SymFit

Marco-SymFit 是一个集成项目，将 Marco 的 FastGen 和 Scheduler 与 SymFit 的符号执行引擎整合在一起。

## 项目结构

```
Marco-SymFit/
├── test/                          # 测试目录
│   ├── run_marco_symfit_integration.sh  # 主测试脚本
│   └── workdir/                   # 工作目录
│       ├── output/                # 输出目录
│       │   ├── tree0/             # 初始种子的 tree 文件
│       │   ├── tree1/             # 新测试用例的 tree 文件
│       │   ├── fifo/queue/        # 新生成的测试用例
│       │   └── afl-slave/queue/   # 初始种子
│       ├── logs/                  # 日志文件
│       └── targets/               # 目标程序
├── marco/                         # Marco 相关文件
│   ├── fastgen/                   # FastGen 二进制
│   │   └── fastgen
│   ├── scheduler/                 # 调度器脚本
│   │   └── main-MS.py
│   └── src/                      # Marco 源代码（用于修改和重新编译）
│       ├── CE/                   # FastGen 源代码
│       └── scheduler/            # 调度器源代码
├── symfit/                        # SymFit 相关文件
│   └── build/                     # 构建产物
│       ├── symsan/bin/
│       │   └── fgtest
│       └── symfit-symsan/x86_64-linux-user/
│           └── symqemu-x86_64
├── symfit-source/                 # SymFit 源代码（用于修改和重新编译）
│   ├── accel/tcg/                # TCG 相关代码
│   ├── external/symsan/          # SymSan 相关代码
│   └── ...
├── local-lib/                     # 本地库文件（如 libz3.so）
├── README.md                      # 本文件
└── BUILD.md                       # 编译指南
```

## 快速开始

### 1. 准备二进制文件

确保以下二进制文件已复制到对应目录：

- `marco/fastgen/fastgen` - FastGen 二进制
- `marco/scheduler/main-MS.py` - 调度器脚本
- `symfit/build/symsan/bin/fgtest` - SymFit fgtest
- `symfit/build/symfit-symsan/x86_64-linux-user/symqemu-x86_64` - SymFit QEMU

### 2. 运行测试

```bash
cd Marco-SymFit/test
bash run_marco_symfit_integration.sh
```

### 3. 查看结果

测试结果会保存在 `test/workdir/output/` 目录下：
- `tree0/` - 初始种子的 tree 文件
- `tree1/` - 新生成的测试用例的 tree 文件
- `fifo/queue/` - 新生成的测试用例

日志文件保存在 `test/workdir/logs/` 目录下。

## 修改和重新编译

### 修改源代码

- **FastGen 源代码**：`marco/src/CE/`
- **Scheduler 源代码**：`marco/src/scheduler/`
- **SymFit 源代码**：`symfit-source/`

### 重新编译

详细编译步骤请参考 `BUILD.md` 文件。

修改源代码后：
1. 重新编译对应的组件
2. 将编译后的二进制复制到对应的目录（`marco/fastgen/` 或 `symfit/build/`）
3. 运行测试验证修改

## 路径说明

所有路径都是相对于 `Marco-SymFit` 根目录的相对路径，确保项目可以完全独立运行，不依赖外部目录结构（包括 `tli-test/Marco`、`tli-test/symfit` 和 `tli-test/local-lib`）。

## 依赖关系

- **FastGen**: Marco 的约束求解器，负责接收分支信息并生成新测试用例
- **Scheduler**: Marco 的调度器，负责选择要探索的分支
- **SymFit**: 符号执行引擎，负责收集分支信息并传递给 FastGen

## 注意事项

1. 确保所有二进制文件具有执行权限
2. 确保 FIFO 管道文件（`/tmp/pcpipe`, `/tmp/myfifo`, `/tmp/wp2`）已正确创建
3. 确保目标程序已编译并位于 `test/workdir/targets/` 目录下

