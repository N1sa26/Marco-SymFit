#!/bin/bash
# FastGen 快速编译脚本
# Marco-SymFit 版本：跳过 llvm_mode 编译（使用 SymFit 进行符号执行）

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR/marco/src/CE"

echo "=== 编译 FastGen (Marco-SymFit 版本) ==="
echo "工作目录: $(pwd)"
echo ""
echo "注意：跳过 llvm_mode 编译（Marco-SymFit 使用 SymFit 进行符号执行）"
echo ""

# 编译 C++ core
echo "[1/3] 编译 C++ core..."
cd fuzzer/cpp_core
rm -rf build
mkdir -p build
cd build
cmake .. && make -j
cd ../../..

# 编译 Rust 部分
echo "[2/3] 编译 Rust 部分..."
cargo build --release

# 复制二进制
echo "[3/3] 复制二进制到运行目录..."
cp target/release/fastgen "$SCRIPT_DIR/marco/fastgen/fastgen"

echo ""
echo "✅ FastGen 编译完成！"
echo "二进制位置: $SCRIPT_DIR/marco/fastgen/fastgen"
echo ""
echo "⚠️  注意：已跳过 llvm_mode 和 llvm_mode_angora 编译"
echo "   这些组件在 Marco-SymFit 中不需要（使用 SymFit 进行符号执行）"

