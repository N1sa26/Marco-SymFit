#!/bin/bash
# SymFit 快速编译脚本

# 不使用 set -e，因为 make install 可能失败但 make 阶段已成功
# set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR/symfit-source"

echo "=== 编译 SymFit ==="
echo "工作目录: $(pwd)"
echo ""

# 编译 SymFit
echo "[1/3] 编译 SymFit..."
# 明确指定编译目标为 symfit-symsan（包含 symqemu-x86_64）
# 即使 make install 失败，make 阶段可能已经成功，所以不设置 set -e
if ! ./build.sh symfit-symsan; then
    echo "⚠️  build.sh 执行有错误，但继续检查编译输出..."
fi

# 复制 fgtest
echo "[2/3] 复制 fgtest..."
mkdir -p "$SCRIPT_DIR/symfit/build/symsan/bin"
if [ -f build/symsan/bin/fgtest ]; then
    cp build/symsan/bin/fgtest "$SCRIPT_DIR/symfit/build/symsan/bin/fgtest"
    echo "✅ fgtest 已复制"
else
    echo "❌ fgtest 未找到，编译可能失败"
    exit 1
fi

# 复制 symqemu-x86_64
echo "[3/3] 复制 symqemu-x86_64..."
mkdir -p "$SCRIPT_DIR/symfit/build/symfit-symsan/x86_64-linux-user"
if [ -f build/symfit-symsan/x86_64-linux-user/symqemu-x86_64 ]; then
    cp build/symfit-symsan/x86_64-linux-user/symqemu-x86_64 \
       "$SCRIPT_DIR/symfit/build/symfit-symsan/x86_64-linux-user/symqemu-x86_64"
    echo "✅ symqemu-x86_64 已复制"
else
    echo "⚠️  symqemu-x86_64 未在 build 目录中找到"
    echo "   检查其他可能的位置..."
    # 尝试在其他位置查找
    SYMQEMU_PATH=$(find build -name "symqemu-x86_64" -type f 2>/dev/null | head -1)
    if [ -n "$SYMQEMU_PATH" ]; then
        echo "   在 $SYMQEMU_PATH 找到，复制..."
        cp "$SYMQEMU_PATH" "$SCRIPT_DIR/symfit/build/symfit-symsan/x86_64-linux-user/symqemu-x86_64"
        echo "✅ symqemu-x86_64 已从 $SYMQEMU_PATH 复制"
    else
        echo "❌ symqemu-x86_64 未找到，编译可能失败"
        exit 1
    fi
fi

echo ""
echo "✅ SymFit 编译完成！"
echo "二进制位置:"
echo "  - $SCRIPT_DIR/symfit/build/symsan/bin/fgtest"
echo "  - $SCRIPT_DIR/symfit/build/symfit-symsan/x86_64-linux-user/symqemu-x86_64"

