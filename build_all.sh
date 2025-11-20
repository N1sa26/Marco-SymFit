#!/bin/bash
# Marco-SymFit 完整编译脚本
# 编译所有组件：FastGen 和 SymFit

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "=========================================="
echo "  Marco-SymFit 完整编译"
echo "=========================================="
echo ""

# 编译 FastGen
echo ">>> [1/2] 编译 FastGen..."
echo ""
if bash "$SCRIPT_DIR/build_fastgen.sh"; then
    echo ""
    echo "✅ FastGen 编译完成"
else
    echo ""
    echo "❌ FastGen 编译失败"
    exit 1
fi

echo ""
echo "----------------------------------------"
echo ""

# 编译 SymFit
echo ">>> [2/2] 编译 SymFit..."
echo ""
if bash "$SCRIPT_DIR/build_symfit.sh"; then
    echo ""
    echo "✅ SymFit 编译完成"
else
    echo ""
    echo "❌ SymFit 编译失败"
    exit 1
fi

echo ""
echo "=========================================="
echo "  ✅ 所有组件编译完成！"
echo "=========================================="
echo ""
echo "编译结果："
echo "  - FastGen: $SCRIPT_DIR/marco/fastgen/fastgen"
echo "  - SymFit fgtest: $SCRIPT_DIR/symfit/build/symsan/bin/fgtest"
echo "  - SymFit QEMU: $SCRIPT_DIR/symfit/build/symfit-symsan/x86_64-linux-user/symqemu-x86_64"
echo ""

