#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/exec-all.h"
#include "exec/helper-proto.h"
#include "exec/cpu_ldst.h"
#include "qemu/qemu-print.h"
#include "tcg.h"
#include "qemu/cutils.h"
#include "dfsan_interface.h"
#include <sys/stat.h>
/* Minimal dfsan declarations to query label parents without pulling C++ headers */
typedef struct dfsan_label_info {
    unsigned int l1;
    unsigned int l2;
    union { unsigned long long i; float f; double d; } op1;
    union { unsigned long long i; float f; double d; } op2;
    unsigned short op;
    unsigned short size;
    unsigned char flags;  /* Marco-compatible: flags for branch state */
    unsigned int tree_size;  /* Marco-compatible: size of expression tree */
    unsigned int hash;
    unsigned int depth;  /* Marco-compatible: depth of expression tree */
} __attribute__((aligned (8), packed)) dfsan_label_info;
extern dfsan_label_info *dfsan_get_label_info(unsigned int label);
/* dfsan_get_label_count is declared in dfsan_interface.h as size_t */

/* Marco-compatible flags */
#define B_FLIPPED 0x1  /* Branch has been flipped/processed */
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
extern CPUArchState *global_env;
#define CONST_LABEL 0

static const uint64_t kShadowMask = ~0x700000000000;
static inline void *shadow_for(uint64_t ptr) {
  return (void *) (((ptr) & kShadowMask) << 2);
}

#define UNIMPLEMENTED_HELPER(opcode)                \
        char op[] = opcode;                         \
        dfsan_unimplemented(op);                    \
        return 0;

#define BINARY_HELPER_ENSURE_EXPRESSIONS                                            \
    if (arg1_label == 0 && arg2_label == 0) {                                       \
        return 0;                                                                   \
    }

#define DECL_HELPER_BINARY(name, bit)                                                 \
    uint64_t HELPER(symsan_##name##_i##bit)(uint##bit##_t arg1, uint64_t arg1_label,  \
                                            uint##bit##_t arg2, uint64_t arg2_label)

#define DEF_HELPER_BINARY(qemu_name, symsan_name, bit)                              \
    DECL_HELPER_BINARY(qemu_name, bit) {                                            \
        BINARY_HELPER_ENSURE_EXPRESSIONS;                                           \
        return dfsan_union(arg1_label, arg2_label, symsan_name, bit, arg1, arg2);   \
    }

/* The binary helpers */
DEF_HELPER_BINARY(add, Add, 32)
DEF_HELPER_BINARY(sub, Sub, 32)
DEF_HELPER_BINARY(mul, Mul, 32)
DEF_HELPER_BINARY(div, SDiv, 32)
DEF_HELPER_BINARY(divu, UDiv, 32)
DEF_HELPER_BINARY(rem, SRem, 32)
DEF_HELPER_BINARY(remu, URem, 32)
DEF_HELPER_BINARY(and, And, 32)
DEF_HELPER_BINARY(or, Or, 32)
DEF_HELPER_BINARY(xor, Xor, 32)
DEF_HELPER_BINARY(shift_right, LShr, 32)
DEF_HELPER_BINARY(arithmetic_shift_right, AShr, 32)
DEF_HELPER_BINARY(shift_left, Shl, 32)

DEF_HELPER_BINARY(add, Add, 64)
DEF_HELPER_BINARY(sub, Sub, 64)
DEF_HELPER_BINARY(mul, Mul, 64)
DEF_HELPER_BINARY(div, SDiv, 64)
DEF_HELPER_BINARY(divu, UDiv, 64)
DEF_HELPER_BINARY(rem, SRem, 64)
DEF_HELPER_BINARY(remu, URem, 64)
DEF_HELPER_BINARY(and, And, 64)
DEF_HELPER_BINARY(or, Or, 64)
DEF_HELPER_BINARY(xor, Xor, 64)
DEF_HELPER_BINARY(shift_right, LShr, 64)
DEF_HELPER_BINARY(arithmetic_shift_right, AShr, 64)
DEF_HELPER_BINARY(shift_left, Shl, 64)

DECL_HELPER_BINARY(rotate_left, 32)
{
    BINARY_HELPER_ENSURE_EXPRESSIONS
    // UNIMPLEMENTED_HELPER("rotate_left32")
    // arg1 << arg2 | arg1 >> (32 - arg2)
    uint32_t shl = dfsan_union(arg1_label, arg2_label, Shl, 32, arg1, arg2);
    uint32_t tmp = dfsan_union(CONST_LABEL, arg2_label, Sub, 32, 32, arg2);
    uint32_t lshr = dfsan_union(arg1_label, tmp, LShr, 32, arg1, 32-arg2);
    return dfsan_union(shl, lshr, Or, 32, arg1 << arg2, arg1 >> (32 - arg2));
}
DECL_HELPER_BINARY(rotate_left, 64)
{
    BINARY_HELPER_ENSURE_EXPRESSIONS
    // UNIMPLEMENTED_HELPER("rotate_left64")
    // arg1 << arg2 | arg1 >> (64 - arg2)
    uint32_t shl = dfsan_union(arg1_label, arg2_label, Shl, 64, arg1, arg2);
    uint32_t tmp = dfsan_union(CONST_LABEL, arg2_label, Sub, 64, 64, arg2);
    uint32_t lshr = dfsan_union(arg1_label, tmp, LShr, 64, arg1, 64-arg2);
    return dfsan_union(shl, lshr, Or, 64, arg1 << arg2, arg1 >> (64 - arg2));
}
DECL_HELPER_BINARY(rotate_right, 32)
{
    BINARY_HELPER_ENSURE_EXPRESSIONS
    // UNIMPLEMENTED_HELPER("rotate_right32")
    // arg1 >> arg2 | arg1 << (32 - arg2)
    uint32_t lshr = dfsan_union(arg1_label, arg2_label, LShr, 32, arg1, arg2);
    uint32_t tmp = dfsan_union(CONST_LABEL, arg2_label, Sub, 32, 32, arg2);
    uint32_t shl = dfsan_union(arg1_label, tmp, Shl, 32, arg1, 32-arg2);
    return dfsan_union(lshr, shl, Or, 32, arg1 >> arg2, arg1 << (32 - arg2));
}
DECL_HELPER_BINARY(rotate_right, 64)
{
    BINARY_HELPER_ENSURE_EXPRESSIONS
    // UNIMPLEMENTED_HELPER("rotate_right64")
    uint32_t lshr = dfsan_union(arg1_label, arg2_label, LShr, 64, arg1, arg2);
    uint32_t tmp = dfsan_union(CONST_LABEL, arg2_label, Sub, 64, 64, arg2);
    uint32_t shl = dfsan_union(arg1_label, tmp, Shl, 64, arg1, 64-arg2);
    return dfsan_union(lshr, shl, Or, 64, arg1 >> arg2, arg1 << (64 - arg2));
}

DECL_HELPER_BINARY(nand, 32)
{
    BINARY_HELPER_ENSURE_EXPRESSIONS
    return dfsan_union(CONST_LABEL,
                        dfsan_union(arg1_label, arg2_label, And, 32, arg1, arg2),
                        Not,
                        32,
                        0, 0);
}
DECL_HELPER_BINARY(nand, 64)
{
    BINARY_HELPER_ENSURE_EXPRESSIONS
    return dfsan_union(CONST_LABEL,
                        dfsan_union(arg1_label, arg2_label, And, 64, arg1, arg2),
                        Not,
                        64,
                        0, 0);
}

DECL_HELPER_BINARY(nor, 32)
{
    BINARY_HELPER_ENSURE_EXPRESSIONS
    return dfsan_union(CONST_LABEL,
                        dfsan_union(arg1_label, arg2_label, Or, 32, arg1, arg2),
                        Not,
                        32,
                        0, 0);
}
DECL_HELPER_BINARY(nor, 64)
{
    BINARY_HELPER_ENSURE_EXPRESSIONS
    return dfsan_union(CONST_LABEL,
                        dfsan_union(arg1_label, arg2_label, Or, 64, arg1, arg2),
                        Not,
                        64,
                        0, 0);
}

DECL_HELPER_BINARY(orc, 32)
{
    BINARY_HELPER_ENSURE_EXPRESSIONS
    return dfsan_union(arg1_label,
                       dfsan_union(arg2_label, CONST_LABEL, Not, 32, arg2, 0),
                       Or,
                       32,
                       arg1, arg2);
}
DECL_HELPER_BINARY(orc, 64)
{
    BINARY_HELPER_ENSURE_EXPRESSIONS
    return dfsan_union(arg1_label,
                       dfsan_union(arg2_label, CONST_LABEL, Not, 64, arg2, 0),
                       Or,
                       64,
                       arg1, arg2);
}

/* andc support */
DECL_HELPER_BINARY(andc, 32)
{
    BINARY_HELPER_ENSURE_EXPRESSIONS
    return dfsan_union(arg1_label,
                       dfsan_union(arg2_label, CONST_LABEL, Not, 32, arg2, 0),
                       And,
                       32,
                       arg1, arg2);
}
DECL_HELPER_BINARY(andc, 64)
{
    BINARY_HELPER_ENSURE_EXPRESSIONS
    return dfsan_union(arg1_label,
                       dfsan_union(arg2_label, CONST_LABEL, Not, 64, arg2, 0),
                       And,
                       64,
                       arg1, arg2);
}
/* eqv support */
DECL_HELPER_BINARY(eqv, 32)
{
    BINARY_HELPER_ENSURE_EXPRESSIONS
    return dfsan_union(dfsan_union(arg1_label, arg2_label, Xor, 32, arg1, arg2),
                       CONST_LABEL,
                       Not,
                       32,
                       0, 0);
}

DECL_HELPER_BINARY(eqv, 64)
{
    BINARY_HELPER_ENSURE_EXPRESSIONS
    return dfsan_union(dfsan_union(arg1_label, arg2_label, Xor, 64, arg1, arg2),
                       CONST_LABEL,
                       Not,
                       64,
                       0, 0);
}

uint64_t HELPER(symsan_neg_i32)(uint32_t op1, uint64_t label)
{
    if (label == 0)
        return 0;
    /* for unary operator Neg/Not, leave the first op as 0 */
    return dfsan_union(CONST_LABEL, label, Neg, 32, 0, op1);
}
uint64_t HELPER(symsan_neg_i64)(uint64_t op1, uint64_t label)
{
    if (label == 0)
        return 0;
    return dfsan_union(CONST_LABEL, label, Neg, 64, 0, op1);
}

uint64_t HELPER(symsan_not_i32)(uint32_t op1, uint64_t label)
{
    if (label == 0)
        return 0;
    return dfsan_union(CONST_LABEL, label, Not, 32, 0, op1);
}
uint64_t HELPER(symsan_not_i64)(uint64_t op1, uint64_t label)
{
    if (label == 0)
        return 0;
    return dfsan_union(CONST_LABEL, label, Not, 64, 0, op1);
}

uint64_t HELPER(symsan_muluh_i64)(uint64_t arg1, uint64_t arg1_label,
                                  uint64_t arg2, uint64_t arg2_label)
{
    BINARY_HELPER_ENSURE_EXPRESSIONS
    uint64_t arg1_new = dfsan_union(arg1_label, CONST_LABEL, ZExt, 64, arg1, 64);
    uint64_t arg2_new = dfsan_union(arg2_label, CONST_LABEL, ZExt, 64, arg2, 64);
    uint64_t res = dfsan_union(arg1_new, arg2_new, Mul, 64, arg1, arg2);
    return dfsan_union(res,
                       CONST_LABEL,
                       Extract,
                       64,
                       127,
                       64);
}

/* z/sext_i32/64 is not real ext operation. */
uint64_t HELPER(symsan_sext_i32)(uint32_t op1, uint64_t op1_label, uint64_t ext_bit)
{
    if (op1_label == 0) return 0; /* op2 label is alway zero */
    size_t bits_to_keep = 32 - ext_bit;
    uint64_t tmp = dfsan_union(op1_label, CONST_LABEL, Shl, 32, op1, bits_to_keep);
    return dfsan_union(tmp, CONST_LABEL, AShr, 32, op1 << bits_to_keep, bits_to_keep);
}
uint64_t HELPER(symsan_sext_i64)(uint64_t op1, uint64_t op1_label, uint64_t ext_bit)
{
    if (op1_label == 0) return 0; /* op2 label is alway zero */
    size_t bits_to_keep = 64 - ext_bit;
    uint64_t tmp = dfsan_union(op1_label, CONST_LABEL, Shl, 64, op1, bits_to_keep);
    return dfsan_union(tmp, CONST_LABEL, AShr, 64, op1 << bits_to_keep, bits_to_keep);
}

uint64_t HELPER(symsan_zext_i32)(uint32_t op1, uint64_t op1_label, uint64_t ext_bit)
{
    if (op1_label == 0) return 0; /* op2 label is alway zero */
    // bitwise and
    return dfsan_union(op1_label, CONST_LABEL, And, 32, op1, (1ull << ext_bit) - 1);
}
uint64_t HELPER(symsan_zext_i64)(uint64_t op1, uint64_t op1_label, uint64_t ext_bit)
{
    if (op1_label == 0) return 0; /* op2 label is alway zero */
    return dfsan_union(op1_label, CONST_LABEL, And, 64, op1, (1ull << ext_bit) - 1);
}

/* *ext_i32_i64 equals to the ext operation in z3 */
uint64_t HELPER(symsan_zext_i32_i64)(uint32_t op1, uint64_t op1_label)
{
    if (op1_label == 0) return 0; /* op2 label is alway zero */
    return dfsan_union(op1_label, CONST_LABEL, ZExt, 64, op1, 32); // extend by 32 bits.
}
uint64_t HELPER(symsan_sext_i32_i64)(uint32_t op1, uint64_t op1_label)
{
    if (op1_label == 0) return 0; /* op2 label is alway zero */
    return dfsan_union(op1_label, CONST_LABEL, SExt, 64, op1, 32); // extend by 32 bits.
}

/* Truncate a 64-bit value to 32-bit */
uint64_t HELPER(symsan_trunc_i64_i32)(uint64_t op1, uint64_t op1_label)
{
    if (op1_label == 0) return 0;
    // Result is 32-bit.
    return dfsan_union(op1_label, CONST_LABEL, Trunc, 32, op1, 32);
}
// https://github.com/chenju2k6/symsan/commit/3392e5b1d33b8ac6e350eeefb37ae861848ba9b2
// bswap support
uint64_t HELPER(symsan_bswap_i32)(uint32_t op1, uint64_t op1_label, uint64_t length)
{
    if (op1_label == 0) return 0;
    uint64_t arg1, arg2, tmp, tmp1, tmp2, first_block, second_block;
    switch (length) {
        case 2:
            arg1 = dfsan_union(
                dfsan_union(op1_label, CONST_LABEL, ZExt, 64, op1, 1),
                CONST_LABEL,
                Shl,
                64,
                op1,
                8);
            arg2 = dfsan_union(
                op1_label,
                CONST_LABEL,
                LShr,
                64,
                op1,
                8);
            return dfsan_union(arg1, arg2, Or, 64, 0, 0);
        case 4:
            tmp = dfsan_union(op1_label, CONST_LABEL, LShr, 64, op1, 8);
            arg1 = dfsan_union(
                tmp,
                CONST_LABEL,
                And,
                64,
                op1,
                0x00ff00ff
            );
            arg2 = dfsan_union(
                dfsan_union(op1_label, CONST_LABEL, And, 64, op1, 0x00ff00ff),
                CONST_LABEL,
                Shl,
                64,
                op1,
                8
            );
            first_block = dfsan_union(arg1, arg2, Or, 64, 0, 0);
            tmp1 = dfsan_union(first_block, CONST_LABEL, LShr, 64, op1, 16);
            tmp2 = dfsan_union(first_block, CONST_LABEL, Shl, 64, op1, 16);
            return dfsan_union(tmp1, tmp2, Or, 64, 0, 0);
        case 8:
            tmp1 = dfsan_union(
                dfsan_union(op1_label, CONST_LABEL, LShr, 64, op1, 8),
                CONST_LABEL,
                And,
                64,
                op1,
                0x00ff00ff00ff00ffull
            );
            tmp2 = dfsan_union(
                dfsan_union(op1_label, CONST_LABEL, And, 64, op1, 0x00ff00ff00ff00ffull),
                CONST_LABEL,
                Shl,
                64,
                op1,
                8
            );
            first_block = dfsan_union(tmp1, tmp2, Or, 64, 0, 0);
            tmp1 = dfsan_union(
                dfsan_union(first_block, CONST_LABEL, LShr, 64, op1, 16),
                CONST_LABEL,
                And,
                64,
                op1,
                0x0000ffff0000ffffull
            );
            tmp2 = dfsan_union(
                dfsan_union(first_block, CONST_LABEL, And, 64, op1, 0x0000ffff0000ffffull),
                CONST_LABEL,
                Shl,
                64,
                op1,
                16
            );
            second_block = dfsan_union(tmp1, tmp2, Or, 64, 0, 0);
            return dfsan_union(
                dfsan_union(
                    second_block,
                    CONST_LABEL,
                    LShr,
                    64,
                    op1,
                    32
                ),
                dfsan_union(
                    second_block,
                    CONST_LABEL,
                    Shl,
                    64,
                    op1,
                    32
                ),
                Or,
                64,
                0,
                0
            );
        default:
            g_assert_not_reached();
    }
}

uint64_t HELPER(symsan_bswap_i64)(uint64_t op1, uint64_t op1_label, uint64_t length)
{
    if (op1_label == 0) return 0;
    uint64_t arg1, arg2, tmp, tmp1, tmp2, first_block, second_block;
    switch (length) {
        case 2:
            arg1 = dfsan_union(
                dfsan_union(op1_label, CONST_LABEL, ZExt, 64, op1, 1),
                CONST_LABEL,
                Shl,
                64,
                op1,
                8);
            arg2 = dfsan_union(
                op1_label,
                CONST_LABEL,
                LShr,
                64,
                op1,
                8);
            return dfsan_union(arg1, arg2, Or, 64, 0, 0);
        case 4:
            tmp = dfsan_union(op1_label, CONST_LABEL, LShr, 64, op1, 8);
            arg1 = dfsan_union(
                tmp,
                CONST_LABEL,
                And,
                64,
                op1,
                0x00ff00ff
            );
            arg2 = dfsan_union(
                dfsan_union(op1_label, CONST_LABEL, And, 64, op1, 0x00ff00ff),
                CONST_LABEL,
                Shl,
                64,
                op1,
                8
            );
            first_block = dfsan_union(arg1, arg2, Or, 64, 0, 0);
            tmp1 = dfsan_union(first_block, CONST_LABEL, LShr, 64, op1, 16);
            tmp2 = dfsan_union(
                dfsan_union(first_block, CONST_LABEL, Shl, 64, op1, 48),
                CONST_LABEL,
                LShr,
                64,
                op1,
                32
            );
            return dfsan_union(tmp1, tmp2, Or, 64, 0, 0);
        case 8:
            tmp1 = dfsan_union(
                dfsan_union(op1_label, CONST_LABEL, LShr, 64, op1, 8),
                CONST_LABEL,
                And,
                64,
                op1,
                0x00ff00ff00ff00ffull
            );
            tmp2 = dfsan_union(
                dfsan_union(op1_label, CONST_LABEL, And, 64, op1, 0x00ff00ff00ff00ffull),
                CONST_LABEL,
                Shl,
                64,
                op1,
                8
            );
            first_block = dfsan_union(tmp1, tmp2, Or, 64, 0, 0);
            tmp1 = dfsan_union(
                dfsan_union(first_block, CONST_LABEL, LShr, 64, op1, 16),
                CONST_LABEL,
                And,
                64,
                op1,
                0x0000ffff0000ffffull
            );
            tmp2 = dfsan_union(
                dfsan_union(first_block, CONST_LABEL, And, 64, op1, 0x0000ffff0000ffffull),
                CONST_LABEL,
                Shl,
                64,
                op1,
                16
            );
            second_block = dfsan_union(tmp1, tmp2, Or, 64, 0, 0);
            return dfsan_union(
                dfsan_union(
                    second_block,
                    CONST_LABEL,
                    LShr,
                    64,
                    op1,
                    32
                ),
                dfsan_union(
                    second_block,
                    CONST_LABEL,
                    Shl,
                    64,
                    op1,
                    32
                ),
                Or,
                64,
                0,
                0
            );
        default:
            g_assert_not_reached();
    }
}

/* Extract syntax
    dfsan_union(label, CONST_LABEL, Extract, 8, 0, i * 8);
    size = 8
    op2 = offset (bit-wise)
    Extract one byte (8-bit) from a 8-byte value
    extract2_i32/i64 can be handled by extract_i32/i64, the len is fixed (32 or 64).
    sextract_i32/i64 is also handled by extract_i32/i64 now. Maybe need a FIXME.
 */
uint64_t HELPER(symsan_extract_i32)(uint32_t arg, uint64_t arg_label, uint32_t ofs, uint32_t len)
{
    if (arg_label == 0) return 0;
    /* len is the extract length.
       ofs is the offset to start extract.
     */
    uint32_t out = dfsan_union(arg_label, CONST_LABEL, Extract, 32, ofs + len - 1, ofs);
    return dfsan_union(out, CONST_LABEL, ZExt, 32, 0, 32 - len);
}
uint64_t HELPER(symsan_extract_i64)(uint64_t arg, uint64_t arg_label, uint64_t ofs, uint64_t len)
{
    if (arg_label == 0) return 0;
    /* len is the extract length.
       ofs is the offset to start extract.
     */
    uint32_t out = dfsan_union(arg_label, CONST_LABEL, Extract, 64, ofs + len - 1, ofs);
    return dfsan_union(out, CONST_LABEL, ZExt, 64, 0, 64 - len);
}

uint64_t HELPER(symsan_sextract_i32)(uint32_t arg, uint64_t arg_label, uint32_t ofs, uint32_t len)
{
    if (arg_label == 0) return 0;
    /* len is the extract length.
       ofs is the offset to start extract.
     */
    uint32_t out = dfsan_union(arg_label, CONST_LABEL, Extract, 32, ofs + len - 1, ofs);
    return dfsan_union(out, CONST_LABEL, SExt, 32, 0, 32 - len);
}
uint64_t HELPER(symsan_sextract_i64)(uint64_t arg, uint64_t arg_label, uint64_t ofs, uint64_t len)
{
    if (arg_label == 0) return 0;
    /* len is the extract length.
       ofs is the offset to start extract.
     */
    uint32_t out = dfsan_union(arg_label, CONST_LABEL, Extract, 64, ofs + len - 1, ofs);
    return dfsan_union(out, CONST_LABEL, SExt, 64, 0, 64 - len);
}

uint64_t HELPER(symsan_deposit_i32)(uint32_t arg1, uint64_t arg1_label,
                              uint32_t arg2, uint64_t arg2_label,
                              uint32_t ofs, uint32_t len)
{
    BINARY_HELPER_ENSURE_EXPRESSIONS
    /* The symbolic implementation follows the alternative concrete
     * implementation of tcg_gen_deposit_i64 in tcg-op.c (which handles
     * architectures that don't support deposit directly). */

    uint64_t mask = (1ull << len) - 1;
    uint64_t arg1_new_label = dfsan_union(arg1_label, CONST_LABEL, And, 32, arg1, ~(mask << ofs));
    uint64_t arg2_new_label = dfsan_union(arg2_label, CONST_LABEL, And, 32, arg2, mask);
    arg2_new_label = dfsan_union(arg2_new_label, CONST_LABEL, Shl, 32, arg2 & mask, ofs);
    return dfsan_union(arg1_new_label, arg2_new_label, Or, 32, arg1 & ~(mask << ofs), (arg2 & mask) << ofs);
}

uint64_t HELPER(symsan_deposit_i64)(uint64_t arg1, uint64_t arg1_label,
                              uint64_t arg2, uint64_t arg2_label,
                              uint64_t ofs, uint64_t len)
{
    BINARY_HELPER_ENSURE_EXPRESSIONS
    /* The symbolic implementation follows the alternative concrete
     * implementation of tcg_gen_deposit_i64 in tcg-op.c (which handles
     * architectures that don't support deposit directly). */

    uint64_t mask = (1ull << len) - 1;
    uint64_t arg1_new_label = dfsan_union(arg1_label, CONST_LABEL, And, 64, arg1, ~(mask << ofs));
    uint64_t arg2_new_label = dfsan_union(arg2_label, CONST_LABEL, And, 64, arg2, mask);
    arg2_new_label = dfsan_union(arg2_new_label, CONST_LABEL, Shl, 64, arg2 & mask, ofs);
    return dfsan_union(arg1_new_label, arg2_new_label, Or, 64, arg1 & ~(mask << ofs), (arg2 & mask) << ofs);
}

uint64_t HELPER(symsan_extract2_i32)(uint32_t ah, uint64_t ah_label,
                                     uint32_t al, uint64_t al_label,
                                     uint64_t ofs)
{
    if (ah_label == 0 && al_label == 0)
        return 0;

    /* The implementation follows the alternative implementation of
     * tcg_gen_extract2_i32 in tcg-op.c (which handles architectures that don't
     * support extract2 directly). */

    if (ofs == 0)
        return al_label;
    if (ofs == 32)
        return ah_label;
    uint64_t al_new = dfsan_union(al_label, CONST_LABEL, LShr, 32, al, ofs);
    return HELPER(symsan_deposit_i32)(al >> ofs, al_new, ah, ah_label, 32-ofs, ofs);
}

uint64_t HELPER(symsan_extract2_i64)(uint64_t ah, uint64_t ah_label,
                                     uint64_t al, uint64_t al_label,
                                     uint64_t ofs)
{
    if (ah_label == 0 && al_label == 0)
        return 0;

    /* The implementation follows the alternative implementation of
     * tcg_gen_extract2_i64 in tcg-op.c (which handles architectures that don't
     * support extract2 directly). */

    if (ofs == 0)
        return al_label;
    if (ofs == 64)
        return ah_label;
    uint64_t al_new = dfsan_union(al_label, CONST_LABEL, LShr, 64, al, ofs);
    return HELPER(symsan_deposit_i64)(al >> ofs, al_new, ah, ah_label, 64-ofs, ofs);
}

// Marco-compatible pipe communication
static int __marco_pipe_fd = -1;
static int __marco_ack_fd = -1;
// Removed unused __marco_order variable
static uint32_t __marco_ctxh = 0; /* call-stack context hash (TaintPass.cc style) */
static uint64_t __marco_pp_state = 0; /* path-prefix rolling state */
static uint32_t __marco_max_label = 0; /* Marco-compatible: max label in current trace (reset per trace) */
/* small ring buffer to dedup pp_hash (untaken_update_ifsat) like path-prefix check */
#define SEEN_PP_CAP 256
static uint32_t __marco_seen_pp[SEEN_PP_CAP];

/* Marco-compatible: track branch order per (context, PC) pair
 * Similar to Marco's __branches map: key={__taint_trace_callstack, addr}, value=order
 * Using hash table implementation similar to Marco's std::unordered_map */

/* Hash table entry structure */
typedef struct branch_order_entry {
    uint32_t ctxh;      /* Context hash */
    uint64_t addr;      /* PC address */
    uint16_t order;     /* Access order (1-based) */
    struct branch_order_entry *next;  /* For chaining */
} branch_order_entry_t;

/* Hash table structure */
#define BRANCH_ORDER_HASH_SIZE 1024  /* Hash table size (power of 2 for fast modulo) */
#define BRANCH_ORDER_MAX_COUNT 64     /* Max order value (matching Marco's MAX_BRANCH_COUNT) */
static branch_order_entry_t *__marco_branch_orders_hash[BRANCH_ORDER_HASH_SIZE];
static branch_order_entry_t *__marco_branch_orders_pool = NULL;
static uint32_t __marco_branch_orders_pool_size = 0;
static uint32_t __marco_branch_orders_count = 0;  /* Current number of entries in hash table */
#define BRANCH_ORDER_POOL_CAP 4096   /* Pre-allocated pool size */

/* Hash function: similar to Marco's context_hash
 * Hash of (ctxh, addr) pair using XOR */
static inline uint32_t branch_order_hash(uint32_t ctxh, uint64_t addr) {
    /* Simple hash: XOR of ctxh and addr (lower 32 bits) */
    uint32_t addr_low = (uint32_t)(addr & 0xFFFFFFFF);
    uint32_t addr_high = (uint32_t)((addr >> 32) & 0xFFFFFFFF);
    return ctxh ^ addr_low ^ addr_high;
}

/* Get hash table index */
static inline uint32_t branch_order_index(uint32_t hash) {
    return hash & (BRANCH_ORDER_HASH_SIZE - 1);  /* Fast modulo (power of 2) */
}

/* Allocate a new entry from pool or malloc */
static inline branch_order_entry_t *branch_order_alloc(void) {
    branch_order_entry_t *entry;
    
    /* Try to reuse from pool if available */
    if (__marco_branch_orders_pool != NULL) {
        entry = __marco_branch_orders_pool;
        __marco_branch_orders_pool = entry->next;
        __marco_branch_orders_pool_size--;
        entry->next = NULL;
        return entry;
    }
    
    /* Allocate new entry */
    entry = (branch_order_entry_t *)malloc(sizeof(branch_order_entry_t));
    if (entry == NULL) {
        return NULL;  /* Out of memory */
    }
    entry->next = NULL;
    return entry;
}

/* Free an entry (add to pool for reuse) */
static inline void branch_order_free(branch_order_entry_t *entry) {
    if (entry == NULL) return;
    
    /* Add to pool if not full */
    if (__marco_branch_orders_pool_size < BRANCH_ORDER_POOL_CAP) {
        entry->next = __marco_branch_orders_pool;
        __marco_branch_orders_pool = entry;
        __marco_branch_orders_pool_size++;
    } else {
        free(entry);
    }
}

/* Marco-compatible: get order for (ctxh, addr) pair
 * Returns the order (1-based, increments for same (ctxh, addr))
 * Implementation matches Marco's std::unordered_map behavior */
static inline uint16_t get_branch_order(uint32_t ctxh, uint64_t addr) {
    uint32_t hash = branch_order_hash(ctxh, addr);
    uint32_t idx = branch_order_index(hash);
    branch_order_entry_t *entry, *prev;
    
    /* Search in hash bucket */
    prev = NULL;
    entry = __marco_branch_orders_hash[idx];
    
    while (entry != NULL) {
        if (entry->ctxh == ctxh && entry->addr == addr) {
            /* Found existing entry: increment order (matching Marco's behavior) */
            if (entry->order < BRANCH_ORDER_MAX_COUNT) {
                entry->order++;
            }
            return entry->order;
        }
        prev = entry;
        entry = entry->next;
    }
    
    /* Not found: insert new entry (matching Marco's insert behavior) */
    entry = branch_order_alloc();
    if (entry == NULL) {
        /* Out of memory: return 1 as fallback */
        return 1;
    }
    
    entry->ctxh = ctxh;
    entry->addr = addr;
    entry->order = 1;  /* Initial order = 1 (matching Marco) */
    entry->next = __marco_branch_orders_hash[idx];
    __marco_branch_orders_hash[idx] = entry;
    __marco_branch_orders_count++;  /* Increment entry count */
    
    return 1;
}

/* Marco-compatible: reset branch orders for new trace
 * Clears hash table and returns entries to pool */
static inline void reset_branch_orders(void) {
    branch_order_entry_t *entry, *next;
    
    /* Clear all hash buckets and return entries to pool */
    for (uint32_t i = 0; i < BRANCH_ORDER_HASH_SIZE; i++) {
        entry = __marco_branch_orders_hash[i];
        while (entry != NULL) {
            next = entry->next;
            branch_order_free(entry);
            entry = next;
        }
        __marco_branch_orders_hash[i] = NULL;
    }
    __marco_branch_orders_count = 0;  /* Reset entry count */
}
static uint32_t __marco_seen_pp_size = 0;
static uint32_t __marco_seen_pp_idx = 0;

/* Marco-compatible: bitmap and virgin_map for isInterestingBranch */
#define MARCO_BITMAP_SIZE 65536
#define MARCO_BITMAP_STRIDE 8
#define MARCO_MAP_SIZE (1 << 16)  /* 65536 */
static uint16_t __marco_bitmap[MARCO_BITMAP_SIZE];
static bool __marco_is_interesting = false;
static uint8_t __marco_virgin_map[MARCO_MAP_SIZE];
static uint8_t __marco_trace_map[MARCO_MAP_SIZE];
static uint8_t __marco_context_map[MARCO_MAP_SIZE * 8];  /* bit array */
static uint32_t __marco_prev_loc = 0;
static uint32_t __marco_visited_set[256];  /* simplified visited set */
static uint32_t __marco_visited_size = 0;

/* Call stack tracking for context-sensitive hashing (TaintPass.cc style) */
#define CALL_STACK_DEPTH 32
static uint64_t __marco_call_stack[CALL_STACK_DEPTH];  /* PC addresses in call stack */
static uint32_t __marco_call_depth = 0;  /* Current call stack depth */
static uint64_t __marco_last_pc = 0;  /* Last PC for detecting call/ret */

/* Branch dependency tracking for extra field generation (Marco style) */
#define CONST_OFFSET 0x80000000
#define MAX_BRANCH_DEPS 100000
#define MAX_EXTRA_LEN 512

/* Simple label tuple structure */
typedef struct {
    uint32_t label;
    uint32_t dir;
} label_tuple_t;

/* Branch dependency structure */
typedef struct {
    uint32_t input_deps_count;
    uint32_t input_deps[32];  /* Max 32 input dependencies */
    uint32_t label_tuples_count;
    label_tuple_t label_tuples[32];  /* Max 32 label tuples */
} branch_dep_t;

static branch_dep_t *__marco_branch_deps = NULL;
static uint32_t __marco_branch_deps_size = 0;

/* Get branch dependency for a given input offset */
static inline branch_dep_t *get_branch_dep(uint32_t offset) {
    if (__marco_branch_deps == NULL) {
        __marco_branch_deps = calloc(MAX_BRANCH_DEPS, sizeof(branch_dep_t));
        if (__marco_branch_deps == NULL) {
            return NULL;
        }
        __marco_branch_deps_size = MAX_BRANCH_DEPS;
    }
    if (offset >= __marco_branch_deps_size) {
        return NULL;
    }
    return &__marco_branch_deps[offset];
}

/* Marco-compatible: Get all input dependencies of a label (recursive)
 * This function matches Marco's get_input_deps logic exactly */
static void get_input_deps(dfsan_label label, uint32_t *deps, uint32_t *deps_count) {
    if (label == 0 || label < CONST_OFFSET) {
        return;
    }
    if (*deps_count >= 32) {
        return;  /* Too many dependencies */
    }
    
    dfsan_label_info *info = dfsan_get_label_info(label);
    if (!info) {
        return;
    }
    
    /* Marco-compatible: check depth before processing */
    if (info->depth > 500) {
        return;  /* Tree too deep, skip */
    }
    
    /* Marco-compatible: special ops - input (op == 0) */
    if (info->op == 0) {
        uint32_t offset = (uint32_t)info->op1.i;
        /* Check if already added */
        for (uint32_t i = 0; i < *deps_count; i++) {
            if (deps[i] == offset) {
                return;
            }
        }
        deps[*deps_count] = offset;
        (*deps_count)++;
        return;
    }
    
    /* Marco-compatible: special ops - Load
     * In Marco: DFSAN_LOAD is a special op, l1 contains base label, l2 contains size
     * We detect Load by checking if l1 is an input (op==0) and l2 is a reasonable size */
    if (info->l1 >= CONST_OFFSET && info->l2 > 0 && info->l2 <= 64) {
        dfsan_label_info *base_info = dfsan_get_label_info(info->l1);
        if (base_info && base_info->op == 0) {
            /* This is likely a Load operation - Marco style */
            uint32_t offset = (uint32_t)base_info->op1.i;
            /* Add base offset */
            int found = 0;
            for (uint32_t j = 0; j < *deps_count; j++) {
                if (deps[j] == offset) {
                    found = 1;
                    break;
                }
            }
            if (!found && *deps_count < 32) {
                deps[*deps_count] = offset;
                (*deps_count)++;
            }
            /* Add additional bytes (offset + 1, offset + 2, ..., offset + l2 - 1) */
            for (uint32_t i = 1; i < info->l2 && *deps_count < 32; i++) {
                uint32_t byte_offset = offset + i;
                found = 0;
                for (uint32_t j = 0; j < *deps_count; j++) {
                    if (deps[j] == byte_offset) {
                        found = 1;
                        break;
                    }
                }
                if (!found) {
                    deps[*deps_count] = byte_offset;
                    (*deps_count)++;
                }
            }
            return;
        }
    }
    
    /* Marco-compatible: special ops - ZExt, SExt, Trunc, Extract
     * These operations only depend on l1 */
    /* Note: We need to check the actual op values - for now, we'll use a heuristic */
    /* If l2 == 0 and l1 >= CONST_OFFSET, it might be a unary op on l1 */
    if (info->l2 == 0 && info->l1 >= CONST_OFFSET) {
        get_input_deps(info->l1, deps, deps_count);
        return;
    }
    
    /* Marco-compatible: special ops - Not, Neg
     * These operations only depend on l2 */
    /* If l1 == 0 and l2 >= CONST_OFFSET, it might be a unary op on l2 */
    if (info->l1 == 0 && info->l2 >= CONST_OFFSET) {
        get_input_deps(info->l2, deps, deps_count);
        return;
    }
    
    /* Marco-compatible: common ops - recursive on l1 and l2 */
    if (info->l1 >= CONST_OFFSET) {
        get_input_deps(info->l1, deps, deps_count);
    }
    if (info->l2 >= CONST_OFFSET) {
        /* Marco uses separate deps2 set for l2, then merges */
        uint32_t deps2[32];
        uint32_t deps2_count = 0;
        get_input_deps(info->l2, deps2, &deps2_count);
        /* Merge deps2 into deps */
        for (uint32_t i = 0; i < deps2_count && *deps_count < 32; i++) {
            int found = 0;
            for (uint32_t j = 0; j < *deps_count; j++) {
                if (deps[j] == deps2[i]) {
                    found = 1;
                    break;
                }
            }
            if (!found) {
                deps[*deps_count] = deps2[i];
                (*deps_count)++;
            }
        }
    }
}

/* Marco-compatible: Collect all input dependencies (including transitive ones)
 * This function matches Marco's get_extra_tuple logic for collecting inputs */
static void collect_all_input_deps(dfsan_label label, uint32_t *inputs, uint32_t *inputs_count) {
    /* First, get direct input dependencies */
    get_input_deps(label, inputs, inputs_count);
    
    /* Marco-compatible: use worklist algorithm to collect transitive dependencies
     * This matches Marco's get_extra_tuple logic exactly */
    uint32_t worklist[32];
    uint32_t worklist_count = *inputs_count;
    for (uint32_t i = 0; i < worklist_count; i++) {
        worklist[i] = inputs[i];
    }
    
    /* Marco-compatible: while (!worklist.empty()) */
    uint32_t worklist_idx = 0;
    while (worklist_idx < worklist_count && *inputs_count < 32) {
        uint32_t off = worklist[worklist_idx];
        worklist_idx++;
        
        branch_dep_t *deps = get_branch_dep(off);
        if (deps != NULL) {
            /* Marco-compatible: for (auto i : deps->input_deps) */
            for (uint32_t j = 0; j < deps->input_deps_count && *inputs_count < 32; j++) {
                uint32_t dep_off = deps->input_deps[j];
                /* Marco-compatible: if (inputs.insert(i).second) */
                int found = 0;
                for (uint32_t k = 0; k < *inputs_count; k++) {
                    if (inputs[k] == dep_off) {
                        found = 1;
                        break;
                    }
                }
                if (!found) {
                    /* Marco-compatible: inputs.insert(i) and worklist.push_back(i) */
                    inputs[*inputs_count] = dep_off;
                    (*inputs_count)++;
                    if (worklist_count < 32) {
                        worklist[worklist_count] = dep_off;
                        worklist_count++;
                    }
                }
            }
        }
    }
}

/* Marco-compatible: Generate extra field with all dependency branches
 * This function matches Marco's get_extra_tuple logic exactly
 * Note: ifmemorize parameter is not used in SymFit (always collect tuples) */
static void generate_extra_field(dfsan_label label, uint32_t tkdir, char *extra, size_t extra_size) {
    if (label == 0) {
        extra[0] = '\0';
        return;
    }
    
    /* Marco-compatible: Step 1 - collect all input dependencies */
    uint32_t inputs[32];
    uint32_t inputs_count = 0;
    collect_all_input_deps(label, inputs, &inputs_count);
    
    /* Marco-compatible: if (inputs.size() == 0) return res */
    if (inputs_count == 0) {
        extra[0] = '\0';
        return;
    }
    
    /* Marco-compatible: Step 2 - collect label_tuples (ifmemorize==1 logic)
     * In SymFit, we always collect tuples (no ifmemorize check) */
    label_tuple_t all_tuples[64];
    uint32_t all_tuples_count = 0;
    
    /* Marco-compatible: labeltuple_set_t added; */
    /* We use a simple array to track added tuples */
    uint32_t added_labels[64];
    uint32_t added_dirs[64];
    uint32_t added_count = 0;
    
    /* Marco-compatible: for (auto off : inputs) */
    for (uint32_t i = 0; i < inputs_count; i++) {
        uint32_t off = inputs[i];
        branch_dep_t *deps = get_branch_dep(off);
        if (deps != NULL) {
            /* Marco-compatible: for (auto &expr : deps->label_tuples) */
            for (uint32_t j = 0; j < deps->label_tuples_count && all_tuples_count < 64; j++) {
                label_tuple_t tuple = deps->label_tuples[j];
                /* Marco-compatible: if (added.insert(expr).second) */
                int found = 0;
                for (uint32_t k = 0; k < added_count; k++) {
                    if (added_labels[k] == tuple.label && added_dirs[k] == tuple.dir) {
                        found = 1;
                        break;
                    }
                }
                if (!found) {
                    all_tuples[all_tuples_count] = tuple;
                    all_tuples_count++;
                    if (added_count < 64) {
                        added_labels[added_count] = tuple.label;
                        added_dirs[added_count] = tuple.dir;
                        added_count++;
                    }
                }
            }
        }
    }
    
    /* Marco-compatible: Step 3 - format extra field: label1,dir1.label2,dir2. */
    size_t pos = 0;
    for (uint32_t i = 0; i < all_tuples_count && pos < extra_size - 10; i++) {
        int n = snprintf(extra + pos, extra_size - pos, "%u,%u.",
                        all_tuples[i].label, all_tuples[i].dir);
        if (n > 0 && n < (int)(extra_size - pos)) {
            pos += n;
        } else {
            break;
        }
    }
    extra[pos] = '\0';
    
    /* Marco-compatible: Step 4 - update branch_dep for all input offsets
     * This is critical for nested solving */
    for (uint32_t i = 0; i < inputs_count; i++) {
        uint32_t off = inputs[i];
        branch_dep_t *deps = get_branch_dep(off);
        if (deps == NULL) {
            /* Marco-compatible: create new branch_dep_t if not exists */
            /* Note: get_branch_dep already allocates, so this shouldn't happen */
            continue;
        }
        
        /* Marco-compatible: c->input_deps.insert(inputs.begin(), inputs.end()) */
        for (uint32_t j = 0; j < inputs_count && deps->input_deps_count < 32; j++) {
            uint32_t dep_off = inputs[j];
            /* Check if already added */
            int found = 0;
            for (uint32_t k = 0; k < deps->input_deps_count; k++) {
                if (deps->input_deps[k] == dep_off) {
                    found = 1;
                    break;
                }
            }
            if (!found) {
                deps->input_deps[deps->input_deps_count] = dep_off;
                deps->input_deps_count++;
            }
        }
        
        /* Marco-compatible: c->label_tuples.insert(std::make_tuple(label, tkdir)) */
        if (deps->label_tuples_count < 32) {
            /* Check if already added */
            int found = 0;
            for (uint32_t k = 0; k < deps->label_tuples_count; k++) {
                if (deps->label_tuples[k].label == label && deps->label_tuples[k].dir == tkdir) {
                    found = 1;
                    break;
                }
            }
            if (!found) {
                deps->label_tuples[deps->label_tuples_count].label = label;
                deps->label_tuples[deps->label_tuples_count].dir = tkdir;
                deps->label_tuples_count++;
            }
        }
    }
}

static inline int seen_pp_before(uint32_t h) {
    for (uint32_t i = 0; i < __marco_seen_pp_size; ++i) {
        if (__marco_seen_pp[i] == h) return 1;
    }
    return 0;
}

static inline void remember_pp(uint32_t h) {
    if (__marco_seen_pp_size < SEEN_PP_CAP) {
        __marco_seen_pp[__marco_seen_pp_size++] = h;
    } else {
        __marco_seen_pp[__marco_seen_pp_idx++] = h;
        if (__marco_seen_pp_idx >= SEEN_PP_CAP) __marco_seen_pp_idx = 0;
    }
}

/* simple 64-bit mix */
static inline uint64_t mix64(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

/* compute a per-branch prefix hash; taken flag participates
 * Marco-compatible: returns untaken_digest, marks taken_digest as visited */
static inline uint64_t roll_in_pp(uint64_t pc, uint32_t label, uint32_t taken) {
    /* Marco-compatible: roll in pc first */
    uint64_t v = pc;
    __marco_pp_state = mix64(__marco_pp_state ^ v);
    
    /* Marco-compatible: roll in ifconcrete and direction */
    uint8_t deter = (label == 0) ? 1 : 0;
    v = deter;
    __marco_pp_state = mix64(__marco_pp_state ^ v);
    
    if (label == 0) {
        /* concrete branch: roll in direction and return 0 */
        v = taken & 1;
        __marco_pp_state = mix64(__marco_pp_state ^ v);
        return 0;
    }
    
    /* symbolic branch: compute both taken and untaken digests */
    uint64_t taken_digest, untaken_digest;
    uint64_t tmp_state = __marco_pp_state;
    
    /* for untaken branch: roll in untaken direction */
    uint32_t untaken_dir = 1 - taken;
    v = untaken_dir & 1;
    tmp_state = mix64(tmp_state ^ v);
    untaken_digest = tmp_state;
    
    /* for taken branch: roll in taken direction */
    v = taken & 1;
    __marco_pp_state = mix64(__marco_pp_state ^ v);
    taken_digest = __marco_pp_state;
    
    /* Marco-compatible: mark taken branch as visited */
    remember_pp((uint32_t)(taken_digest & 0xffffffffu));
    
    /* return untaken digest (for untaken_update_ifsat) */
    return untaken_digest;
}

/* Marco-compatible: bitmap and virgin_map functions for isInterestingBranch
 * NOTE: These functions are no longer used since we write to wp2 and Marco's
 * solve function handles bitmap/virgin_map updates. Commented out to avoid
 * compilation errors due to missing XXH32 headers.
 */
#if 0
static inline bool isPowerOfTwoOrZero(uint32_t x) {
    return ((x & (x - 1)) == 0);
}

static inline uint32_t marco_hashPc(uint64_t pc, uint32_t taken) {
    XXH32_state_t state;
    XXH32_reset(&state, 0);
    XXH32_update(&state, &pc, sizeof(pc));
    XXH32_update(&state, &taken, sizeof(taken));
    return XXH32_digest(&state) % MARCO_MAP_SIZE;
}

static inline uint32_t marco_getIndex(uint32_t h) {
    return ((__marco_prev_loc >> 1) ^ h) % MARCO_MAP_SIZE;
}

static inline bool marco_isInterestingContext(uint32_t h, uint32_t bits) {
    bool interesting = false;
    
    /* only care power of two */
    if (!isPowerOfTwoOrZero(bits))
        return false;
    
    for (uint32_t i = 0; i < __marco_visited_size; i++) {
        uint32_t prev_h = __marco_visited_set[i];
        
        /* Calculate hash(prev_h || h) */
        XXH32_state_t state;
        XXH32_reset(&state, 0);
        XXH32_update(&state, &prev_h, sizeof(prev_h));
        XXH32_update(&state, &h, sizeof(h));
        
        uint32_t hash = XXH32_digest(&state) % (MARCO_MAP_SIZE * 8);
        uint32_t idx = hash / 8;
        uint32_t mask = 1 << (hash % 8);
        
        if ((__marco_context_map[idx] & mask) == 0) {
            __marco_context_map[idx] |= mask;
            interesting = true;
        }
    }
    
    if (bits == 0 && __marco_visited_size < 256) {
        __marco_visited_set[__marco_visited_size++] = h;
    }
    
    return interesting;
}

static inline void marco_updateBitmap(uint64_t pc, uint64_t ctx) {
    XXH32_state_t state;
    XXH32_reset(&state, 0);
    XXH32_update(&state, &pc, sizeof(pc));
    XXH32_update(&state, &ctx, sizeof(ctx));
    
    uint32_t h = XXH32_digest(&state);
    uint32_t index = h % MARCO_BITMAP_SIZE;
    
    /* Use strided exponential backoff */
    __marco_is_interesting = isPowerOfTwoOrZero(__marco_bitmap[index] / MARCO_BITMAP_STRIDE);
    __marco_bitmap[index]++;
}

/* Marco-compatible: isInterestingBranch function */
static inline bool marco_isInterestingBranch(uint64_t pc, uint32_t taken, uint64_t ctx) {
    /* update bitmap first */
    marco_updateBitmap((void *)pc, ctx);
    if (!__marco_is_interesting) return false;  /* if pruned, don't proceed */
    
    uint32_t h = marco_hashPc(pc, taken);
    uint32_t idx = marco_getIndex(h);
    bool new_context = marco_isInterestingContext(h, __marco_virgin_map[idx]);
    bool ret = false;
    
    __marco_virgin_map[idx]++;
    
    if ((__marco_virgin_map[idx] | __marco_trace_map[idx]) != __marco_trace_map[idx]) {
        uint32_t inv_h = marco_hashPc(pc, !taken);
        uint32_t inv_idx = marco_getIndex(inv_h);
        
        __marco_trace_map[idx] |= __marco_virgin_map[idx];
        
        /* mark the inverse case, because it's already covered by current testcase */
        __marco_virgin_map[inv_idx]++;
        __marco_trace_map[inv_idx] |= __marco_virgin_map[inv_idx];
        __marco_virgin_map[inv_idx]--;
        ret = true;
    } else if (new_context) {
        ret = true;
    } else {
        ret = false;
    }
    
    __marco_prev_loc = h;
    return ret;
}
#endif

/* Marco-compatible: compute label depth by calling serialize (lazy init) */
static inline uint32_t compute_label_depth(dfsan_label l) {
    if (l == 0) return 0;
    dfsan_label_info *info = dfsan_get_label_info(l);
    if (!info) return 0;
    
    /* Marco-compatible: call serialize to compute depth lazily */
    if (info->tree_size == 0) {
        serialize(l);  /* Trigger lazy initialization */
    }
    
    /* Return computed depth */
    return info->depth;
}

/* Hash a PC address to generate a call site ID (similar to random() in TaintPass.cc) */
static inline uint32_t hash_pc_to_call_site_id(uint64_t pc) {
    /* Simple hash function to generate a pseudo-random call site ID */
    uint32_t h = (uint32_t)(pc & 0xffffffffu);
    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    h *= 0xc2b2ae35;
    h ^= h >> 16;
    return h;
}

/* Update context hash on function call (TaintPass.cc style: ctx ^ call_site_id) */
static inline void update_context_on_call(uint64_t call_pc) {
    uint32_t call_site_id = hash_pc_to_call_site_id(call_pc);
    uint32_t old_ctxh = __marco_ctxh;
    
    /* Push to call stack */
    if (__marco_call_depth < CALL_STACK_DEPTH) {
        __marco_call_stack[__marco_call_depth++] = call_pc;
    } else {
        /* Stack overflow: replace oldest entry (simple FIFO) */
        memmove(__marco_call_stack, __marco_call_stack + 1, 
                (CALL_STACK_DEPTH - 1) * sizeof(uint64_t));
        __marco_call_stack[CALL_STACK_DEPTH - 1] = call_pc;
    }
    
    /* Update context hash: XOR with call site ID (TaintPass.cc style) */
    __marco_ctxh = __marco_ctxh ^ call_site_id;
    
    if (qemu_loglevel_mask(CPU_LOG_SYM_BLK_CNT)) {
        fprintf(stderr, "[ctxh] CALL: func_addr=0x%lx, call_site_id=%u, ctxh: %u -> %u (depth=%u)\n",
                call_pc, call_site_id, old_ctxh, __marco_ctxh, __marco_call_depth);
    }
}

/* Restore context hash on function return (TaintPass.cc style: restore original ctx) */
static inline void restore_context_on_return(uint64_t ret_pc_unused) {
    (void)ret_pc_unused;  /* Unused: we use the call stack instead */
    if (__marco_call_depth > 0) {
        /* Pop from call stack and restore context */
        /* The call stack contains the function address (not call site PC) */
        uint64_t func_addr = __marco_call_stack[--__marco_call_depth];
        uint32_t call_site_id = hash_pc_to_call_site_id(func_addr);
        uint32_t old_ctxh = __marco_ctxh;
        
        /* Restore context hash: XOR again to remove the call site ID */
        __marco_ctxh = __marco_ctxh ^ call_site_id;
        
        if (qemu_loglevel_mask(CPU_LOG_SYM_BLK_CNT)) {
            fprintf(stderr, "[ctxh] RET: func_addr=0x%lx, call_site_id=%u, ctxh: %u -> %u (depth=%u)\n",
                    func_addr, call_site_id, old_ctxh, __marco_ctxh, __marco_call_depth);
        }
    }
}

/* Update context hash based on PC changes (heuristic for call/ret detection) */
static inline void update_context_from_pc(uint64_t pc) {
    if (__marco_last_pc == 0) {
        /* First call: initialize */
        __marco_last_pc = pc;
        return;
    }
    
    /* Simple heuristic: detect large PC jumps as potential call/ret */
    uint64_t pc_diff = (pc > __marco_last_pc) ? (pc - __marco_last_pc) : (__marco_last_pc - pc);
    
    /* If PC changed significantly (likely a call/jump), update context */
    /* Threshold: 0x1000 (4KB) - typical function size is larger than this */
    /* This is a heuristic - in real implementation, we'd need proper call/ret hooks */
    if (pc_diff > 0x1000) {
        /* Large jump: could be a call or return */
        /* Check if we're returning to a known call site (simple heuristic) */
        int is_return = 0;
        if (__marco_call_depth > 0) {
            /* Check if current PC is near a known call site (within 0x100 bytes) */
            for (int i = __marco_call_depth - 1; i >= 0; i--) {
                uint64_t call_site = __marco_call_stack[i];
                uint64_t ret_diff = (pc > call_site) ? (pc - call_site) : (call_site - pc);
                if (ret_diff < 0x100) {
                    /* Likely a return to a known call site */
                    is_return = 1;
                    break;
                }
            }
        }
        
        if (is_return) {
            restore_context_on_return(pc);
        } else {
            /* Treat as a function call */
            update_context_on_call(pc);
        }
    }
    
    __marco_last_pc = pc;
}

// Initialize Marco pipe for communication
// Marco expects SymFit to write to /tmp/wp2 (not /tmp/pcpipe)
// Format: qid,label,direction,addr,ctx,order,cons_type,tid,max_label_
// Note: This function is called lazily when first needed, and /tmp/wp2 is closed
// and reopened for each new trace in initialize_taint_from_file() to ensure EOF
static void init_marco_pipe(void) {
    // Always reopen /tmp/wp2 (don't check if already open) to ensure fresh connection
    // This matches Marco's behavior where each seed runs in a separate process
    
    const char *marco_mode = getenv("MARCO_MODE");
    /* Debug: log MARCO_MODE check */
    static int debug_marco_mode_count = 0;
    if (debug_marco_mode_count++ < 3) {
        fprintf(stderr, "[SymFit] DEBUG: init_marco_pipe: MARCO_MODE='%s'\n", marco_mode ? marco_mode : "(null)");
        fflush(stderr);
    }
    if (!marco_mode || strcmp(marco_mode, "1") != 0) {
        if (debug_marco_mode_count <= 3) {
            fprintf(stderr, "[SymFit] DEBUG: init_marco_pipe: MARCO_MODE check failed, returning early\n");
            fflush(stderr);
        }
        return;
    }
    
    // Close existing fd if any (should not happen, but be safe)
    if (__marco_pipe_fd >= 0) {
        close(__marco_pipe_fd);
        __marco_pipe_fd = -1;
    }
    
    // Marco's solve function reads from /tmp/wp2, not /tmp/pcpipe
    // /tmp/pcpipe is written by Marco's update_graph function after processing
    __marco_pipe_fd = open("/tmp/wp2", O_WRONLY | O_APPEND);
    if (__marco_pipe_fd < 0) {
        // Try to create the pipe if it doesn't exist
        int mkfifo_ret = mkfifo("/tmp/wp2", 0666);
        if (mkfifo_ret < 0 && errno != EEXIST) {
            fprintf(stderr, "[SymFit] ERROR: mkfifo('/tmp/wp2') failed: errno=%d (%s)\n", errno, strerror(errno));
            fflush(stderr);
        }
        __marco_pipe_fd = open("/tmp/wp2", O_WRONLY | O_APPEND);
        /* Debug: log open result */
        static int debug_open_count = 0;
        debug_open_count++;
        if (debug_open_count <= 5) {
            fprintf(stderr, "[SymFit] DEBUG: init_marco_pipe: open('/tmp/wp2') after mkfifo: fd=%d, errno=%d (%s)\n", 
                    __marco_pipe_fd, __marco_pipe_fd < 0 ? errno : 0, __marco_pipe_fd < 0 ? strerror(errno) : "success");
            fflush(stderr);
        }
    } else {
        /* Debug: log open success */
        static int debug_open_success_count = 0;
        debug_open_success_count++;
        if (debug_open_success_count <= 5) {
            fprintf(stderr, "[SymFit] DEBUG: init_marco_pipe: open('/tmp/wp2') success: fd=%d\n", __marco_pipe_fd);
            fflush(stderr);
        }
    }
    
    // Always log if pipe is still not open (critical error)
    if (__marco_pipe_fd < 0) {
        fprintf(stderr, "[SymFit] ERROR: init_marco_pipe: failed to open /tmp/wp2 after all attempts: errno=%d (%s)\n", errno, strerror(errno));
        fflush(stderr);
    }
    
    // Open acknowledgment pipe for reading (only once, persistent)
    if (__marco_ack_fd < 0) {
        __marco_ack_fd = open("/tmp/myfifo", O_RDONLY | O_NONBLOCK);
        if (__marco_ack_fd < 0) {
            mkfifo("/tmp/myfifo", 0666);
            __marco_ack_fd = open("/tmp/myfifo", O_RDONLY | O_NONBLOCK);
        }
    }
}

// Ensure wp2/myfifo fds are closed when the process exits, so FastGen.solve sees EOF
static void close_marco_pipes(void) {
    if (__marco_pipe_fd >= 0) {
        int fd = __marco_pipe_fd;
        __marco_pipe_fd = -1;
        close(fd);
    }
    if (__marco_ack_fd >= 0) {
        int fd = __marco_ack_fd;
        __marco_ack_fd = -1;
        close(fd);
    }
}

#if defined(__GNUC__)
__attribute__((destructor))
static void symsan_marco_destructor(void) {
    close_marco_pipes();
}
#endif

// Wait for acknowledgment from scheduler
static void wait_for_ack(void) {
    if (__marco_ack_fd < 0) return;
    
    char ack_buf[256];
    int ret = read(__marco_ack_fd, ack_buf, sizeof(ack_buf) - 1);
    if (ret > 0) {
        ack_buf[ret] = '\0';
        fprintf(stderr, "DEBUG: Received ack: %s", ack_buf);
    }
}

// Marco-compatible taint initialization
static int __taint_initialized = 0;
static char *__taint_file_buf = NULL;
static size_t __taint_file_size = 0;
static int __taint_fd = -1;
static char __taint_file_path[512] = {0};  /* Store input file path for queueid detection */
static int __trace_count = 0;  /* Track trace count to detect new trace */
static uint32_t __marco_traceid = 0;  /* Store traceid parsed from filename at initialization */
static uint32_t __marco_queueid = 0;  /* Store queueid determined from file path at initialization */
static bool __marco_traceid_queueid_set = false;  /* Flag to track if traceid/queueid have been set */

// Declare external functions from dfsan.cpp
extern void taint_set_file(const char *filename, int fd);
extern int taint_get_file(int fd);

static void initialize_taint_from_file(void) {
    // Marco-compatible: Reset __taint_initialized for each new trace
    // This ensures /tmp/wp2 is closed and reopened for each trace, matching Marco's behavior
    // where each seed runs in a separate process that closes /tmp/wp2 on exit
    if (__taint_initialized) {
        // Close /tmp/wp2 to signal EOF to FastGen's solve() function
        // This matches Marco's behavior where each process closes /tmp/wp2 on exit
        if (__marco_pipe_fd >= 0) {
            close(__marco_pipe_fd);
            __marco_pipe_fd = -1;
        }
        __taint_initialized = 0;  // Reset to allow re-initialization for new trace
        __trace_count++;
        // IMPORTANT: Clear __taint_file_path when resetting for new trace
        // This ensures we read the new file path from TAINT_OPTIONS
        // Also reset __marco_traceid_queueid_set to allow re-parsing traceid/queueid for new file
        __taint_file_path[0] = '\0';
        __marco_traceid_queueid_set = false;  // Reset to allow re-parsing for new file
        __marco_traceid = 0;  // Reset traceid
        __marco_queueid = 0;  // Reset queueid
    }
    
    const char *taint_file = getenv("TAINT_OPTIONS");
    if (!taint_file) {
        // If TAINT_OPTIONS is not set, clear __taint_file_path to avoid using stale value
        if (__taint_file_path[0] != '\0') {
            fprintf(stderr, "[SymFit] WARNING: TAINT_OPTIONS not set, clearing __taint_file_path (was '%s')\n", __taint_file_path);
            __taint_file_path[0] = '\0';
        }
        return;
    }
    
    // Parse taint_file=filename from TAINT_OPTIONS
    const char *file_start = strstr(taint_file, "taint_file=");
    if (!file_start) {
        return;
    }
    
    file_start += 11; // skip "taint_file="
    /* filename may be followed by ':' or space with additional options
     * Note: Marco's TAINT_OPTIONS format is: taint_file="/path/to/id:000035":shm_id=...
     * So we need to check for ':' after closing quote, or space if no quotes */
    const char *file_end = NULL;
    /* If filename starts with quote, find the closing quote first */
    if (file_start[0] == '"') {
        const char *quote_end = strchr(file_start + 1, '"');
        if (quote_end) {
            /* Check if there's a ':' after the closing quote */
            const char *colon_after_quote = strchr(quote_end + 1, ':');
            if (colon_after_quote) {
                file_end = quote_end + 1; /* End at closing quote */
            } else {
                /* No ':' after quote, check for space */
                const char *space_after_quote = strchr(quote_end + 1, ' ');
                if (space_after_quote) {
                    file_end = quote_end + 1; /* End at closing quote */
                } else {
                    file_end = quote_end + 1; /* End at closing quote */
                }
            }
        } else {
            /* No closing quote, find space or ':' */
            const char *space_pos = strchr(file_start, ' ');
            const char *colon_pos = strchr(file_start, ':');
            if (space_pos && colon_pos) {
                file_end = (space_pos < colon_pos) ? space_pos : colon_pos;
            } else if (space_pos) {
                file_end = space_pos;
            } else if (colon_pos) {
                file_end = colon_pos;
            } else {
                file_end = file_start + strlen(file_start);
            }
        }
    } else {
        /* No quote, find space or ':' */
        const char *space_pos = strchr(file_start, ' ');
        const char *colon_pos = strchr(file_start, ':');
        if (space_pos && colon_pos) {
            file_end = (space_pos < colon_pos) ? space_pos : colon_pos;
        } else if (space_pos) {
            file_end = space_pos;
        } else if (colon_pos) {
            file_end = colon_pos;
        } else {
            file_end = file_start + strlen(file_start);
        }
    }
    
    char filename[512];
    size_t len = (size_t)(file_end - file_start);
    if (len >= sizeof(filename)) len = sizeof(filename) - 1;
    strncpy(filename, file_start, len);
    filename[len] = '\0';
    /* strip optional surrounding quotes */
    if (filename[0] == '"') {
        size_t n = strlen(filename);
        if (n >= 2 && filename[n-1] == '"') {
            /* shift left and drop trailing quote */
            memmove(filename, filename+1, n-2);
            filename[n-2] = '\0';
        }
    }
    
    // Open file and set up taint tracking
    __taint_fd = open(filename, O_RDONLY);
    if (__taint_fd < 0) {
        fprintf(stderr, "DEBUG: Failed to open file '%s': %s\n", filename, strerror(errno));
        return;
    }
    
    // Set up taint file tracking
    fprintf(stderr, "DEBUG: Calling taint_set_file with filename=%s, fd=%d\n", filename, __taint_fd);
    taint_set_file(filename, __taint_fd);
    fprintf(stderr, "DEBUG: taint_set_file completed\n");
    
    struct stat st;
    if (fstat(__taint_fd, &st) < 0) {
        close(__taint_fd);
        __taint_fd = -1;
        return;
    }
    
    __taint_file_size = st.st_size;
    __taint_file_buf = malloc(__taint_file_size);
    if (!__taint_file_buf) {
        close(__taint_fd);
        __taint_fd = -1;
        return;
    }
    
    ssize_t bytes_read = read(__taint_fd, __taint_file_buf, __taint_file_size);
    if (bytes_read != (ssize_t)__taint_file_size) {
        free(__taint_file_buf);
        __taint_file_buf = NULL;
        close(__taint_fd);
        __taint_fd = -1;
        return;
    }
    
    // Reset file position for future reads
    lseek(__taint_fd, 0, SEEK_SET);
    
    // Create taint labels for each byte (Marco-compatible)
    for (size_t i = 0; i < __taint_file_size; i++) {
        dfsan_label label = dfsan_create_label(i);
        // Store the label in shared memory for later use
        // This will be handled by the fgtest driver
    }
    
    fprintf(stderr, "DEBUG: Taint file initialized: %s (size=%zu, fd=%d)\n", filename, __taint_file_size, __taint_fd);
    
    /* Store input file path for queueid detection (Marco-compatible) */
    // Always update __taint_file_path from current filename to ensure it's correct
    strncpy(__taint_file_path, filename, sizeof(__taint_file_path) - 1);
    __taint_file_path[sizeof(__taint_file_path) - 1] = '\0';
    
    // Debug: log the update
    FILE *debug_fp = fopen("/tmp/symfit_traceid_debug.log", "a");
    if (debug_fp) {
        fprintf(debug_fp, "[SymFit] DEBUG: initialize_taint_from_file: filename='%s', __taint_file_path='%s', __taint_initialized=%d\n", 
                filename, __taint_file_path, __taint_initialized);
        fflush(debug_fp);
        fclose(debug_fp);
    }
    fprintf(stderr, "[SymFit] DEBUG: initialize_taint_from_file: filename='%s', __taint_file_path='%s', __taint_initialized=%d\n", 
            filename, __taint_file_path, __taint_initialized);
    fflush(stderr);
    
    // Parse traceid and queueid from filename ONLY if they haven't been set yet
    // This ensures we don't overwrite the values if initialize_taint_from_file() is called multiple times
    // (e.g., for different branches in the same process, or after reset)
    // We use a flag to track if they've been set, since traceid=0 is a valid value
    if (!__marco_traceid_queueid_set) {
        // CRITICAL FIX: Priority 1 - Use inputid from TAINT_OPTIONS if available
        // This ensures we use the correct tid passed from the test script, not from filename
        const char *taint_opts = getenv("TAINT_OPTIONS");
        if (taint_opts) {
            const char *inputid_start = strstr(taint_opts, "inputid=");
            if (inputid_start) {
                inputid_start += 8; /* skip "inputid=" */
                unsigned long v = strtoul(inputid_start, NULL, 10);
                __marco_traceid = (uint32_t)(v % 1000000);
                fprintf(stderr, "[SymFit] DEBUG: Using inputid=%u from TAINT_OPTIONS\n", __marco_traceid);
                fflush(stderr);
            }
        }
        // Priority 2 - Fallback to filename if inputid not found in TAINT_OPTIONS
        if (__marco_traceid == 0 || !taint_opts || !strstr(taint_opts, "inputid=")) {
            const char *id_start = strstr(filename, "id:");
            if (id_start) {
                id_start += 3; /* skip "id:" */
                unsigned long v = strtoul(id_start, NULL, 10);
                __marco_traceid = (uint32_t)(v % 1000000);
                fprintf(stderr, "[SymFit] DEBUG: Using traceid=%u from filename (fallback)\n", __marco_traceid);
                fflush(stderr);
            }
        }
        
        // Determine queueid from file path and store it
        // Check for both relative and absolute paths, and handle various path formats
        if (strstr(filename, "afl-slave/queue/") != NULL || 
            strstr(filename, "/afl-slave/queue/") != NULL ||
            strstr(filename, "afl-slave/queue") != NULL) {
            __marco_queueid = 0;  /* Initial seeds from afl-slave/queue */
        } 
        else if (strstr(filename, "fifo/queue/") != NULL || 
                 strstr(filename, "/fifo/queue/") != NULL ||
                 strstr(filename, "fifo/queue") != NULL) {
            __marco_queueid = 1;  /* Generated test cases from fifo/queue */
        } 
        else if (strstr(filename, "grader/queue/") != NULL || 
                 strstr(filename, "/grader/queue/") != NULL) {
            __marco_queueid = 1;  /* Grader queue (category 1) */
        } 
        else if (strstr(filename, "grader-path/queue/") != NULL || 
                 strstr(filename, "/grader-path/queue/") != NULL) {
            __marco_queueid = 2;  /* Grader-path queue (category 2) */
        } 
        else {
            /* Fallback: try environment variable */
            const char *env_qid = getenv("MARCO_QUEUEID");
            if (env_qid) {
                unsigned long v = strtoul(env_qid, NULL, 10);
                __marco_queueid = (uint32_t)v;
            } else {
                /* Default: assume initial seed (queueid=0) */
                __marco_queueid = 0;
            }
        }
        
        __marco_traceid_queueid_set = true;  // Mark as set
        fprintf(stderr, "[SymFit] DEBUG: initialize_taint_from_file: SET traceid=%u, queueid=%u from filename='%s'\n", 
                __marco_traceid, __marco_queueid, filename);
        fflush(stderr);
    } else {
        // Values already set: don't overwrite, just log current values
        fprintf(stderr, "[SymFit] DEBUG: initialize_taint_from_file: KEEP traceid=%u, queueid=%u (already set), filename='%s'\n", 
                __marco_traceid, __marco_queueid, filename);
        fflush(stderr);
    }
    
    // Reset branch orders for new trace (Marco resets __branches map per trace)
    reset_branch_orders();
    
    // Reset path-prefix / bitmap state (Marco process restarts per trace)
    __marco_pp_state = 0;
    __marco_seen_pp_size = 0;
    __marco_seen_pp_idx = 0;
    memset(__marco_seen_pp, 0, sizeof(__marco_seen_pp));
    memset(__marco_bitmap, 0, sizeof(__marco_bitmap));
    memset(__marco_virgin_map, 0, sizeof(__marco_virgin_map));
    memset(__marco_trace_map, 0, sizeof(__marco_trace_map));
    memset(__marco_context_map, 0, sizeof(__marco_context_map));
    __marco_prev_loc = 0;
    __marco_visited_size = 0;

    // Clear cached branch dependencies so next trace recomputes them
    if (__marco_branch_deps != NULL) {
        memset(__marco_branch_deps, 0, sizeof(branch_dep_t) * __marco_branch_deps_size);
    }

    // Marco-compatible: reset max_label for new trace (Marco resets __max_label per trace)
    // Note: This is called only once per trace when taint is initialized
    __marco_max_label = 0;
    
    // Marco-compatible: reset context hash for new trace
    // Marco resets __taint_trace_callstack per trace (starts at 0)
    __marco_ctxh = 0;
    __marco_call_depth = 0;
    __marco_last_pc = 0;
    
    // Marco-compatible: Close and reopen /tmp/wp2 for each new trace
    // This ensures FastGen's solve() sees EOF after each trace, matching Marco's behavior
    // where each seed runs in a separate process that closes /tmp/wp2 on exit
    if (__marco_pipe_fd >= 0) {
        close(__marco_pipe_fd);
        __marco_pipe_fd = -1;
    }
    __trace_count++;

    FILE *reset_log_fp = fopen("/tmp/symfit_traceid_debug.log", "a");
    if (reset_log_fp) {
        fprintf(reset_log_fp,
                "[SymFit] RESET TRACE: filename='%s' ctxh=%u pp_state=%lu seen_pp_size=%u branch_orders=%u\n",
                filename, __marco_ctxh,
                (unsigned long)__marco_pp_state,
                __marco_seen_pp_size,
                __marco_branch_orders_count);
        fflush(reset_log_fp);
        fclose(reset_log_fp);
    }
    
    __taint_initialized = 1;
}

static uint64_t symsan_setcond_internal(CPUArchState *env, uint64_t arg1, uint64_t arg1_label,
                                     uint64_t arg2, uint64_t arg2_label,
                                     int32_t cond, uint64_t result, uint8_t result_bits, uint64_t pc)
{
    // Initialize taint first (may close __marco_pipe_fd if reinitializing)
    initialize_taint_from_file();
    // Then initialize Marco pipe (will reopen /tmp/wp2 if needed)
    init_marco_pipe();
    
    // Note: Context hash (__marco_ctxh) is updated via HELPER(symsan_notify_call/return)
    // in TCG translation phase, so we just use the current value here
    // No need to call update_context_from_pc() - rely on proper call/return hooks
    
    // Determine predicate (Marco-compatible encoding)
    // IMPORTANT: Marco's logic checks union result, not input labels
    // Marco's __taint_trace_cmp: if (op1==0 && op2==0) return; else compute union and check result
    // We should follow the same logic: compute union first, then check result
    uint32_t predicate = 0;
    switch (cond) {
    case TCG_COND_EQ: predicate = bveq; break;
    case TCG_COND_NE: predicate = bvneq; break;
    case TCG_COND_LT: predicate = bvslt; break;
    case TCG_COND_GE: predicate = bvsge; break;
    case TCG_COND_LE: predicate = bvsle; break;
    case TCG_COND_GT: predicate = bvsgt; break;
    case TCG_COND_LTU: predicate = bvult; break;
    case TCG_COND_GEU: predicate = bvuge; break;
    case TCG_COND_LEU: predicate = bvule; break;
    case TCG_COND_GTU: predicate = bvugt; break;
    default: g_assert_not_reached();
    }

    // Create ICmp label like Marco: temp = union(op1,op2,(predicate<<8)|ICmp,size,arg1,arg2)
    // Marco's logic: compute union first, then check if result is 0
    // This matches Marco's __taint_trace_cmp behavior
    uint32_t cmp_op = (predicate << 8) | ICmp;
    
    // Debug: log arg labels before union (only first 100 times to avoid spam)
    // IMPORTANT: Always log first 100 calls to diagnose taint propagation issues
    static int debug_arg_labels_count = 0;
    if (debug_arg_labels_count++ < 100) {
        fprintf(stderr, "[SymFit] DEBUG setcond: arg1_label=%llu, arg2_label=%llu, arg1=0x%llx, arg2=0x%llx, pc=0x%llx, second_ccache_flag=%d\n",
                (unsigned long long)arg1_label, (unsigned long long)arg2_label,
                (unsigned long long)arg1, (unsigned long long)arg2, (unsigned long long)pc, second_ccache_flag);
        fflush(stderr);
    }
    
    // CRITICAL: If second_ccache_flag is 0, helper should not be called at all!
    // But if we're here, it means second_ccache_flag was 1 when tcg_gen_setcond_i32 was called
    // However, if arg1_label and arg2_label are both 0, it means labels were not propagated
    // This could happen if:
    // 1. Memory loads didn't set labels (symsan_load_guest not called or returned 0)
    // 2. Arithmetic operations didn't propagate labels (symsan_add/sub/etc not called or returned 0)
    // 3. Labels were lost during TCG temporary variable operations
    
    uint32_t temp_label = dfsan_union((uint32_t)arg1_label, (uint32_t)arg2_label,
                                      cmp_op, result_bits, arg1, arg2);
    
    // Debug: log temp_label after union
    if (debug_arg_labels_count <= 100) {
        fprintf(stderr, "[SymFit] DEBUG setcond: temp_label=%u (after union)\n", temp_label);
        fflush(stderr);
    }
    
    // Marco-compatible filtering: check union result (like Marco's __taint_trace_cond)
    // Marco's __taint_trace_cond: if (label == 0) return;
    // But we want to report ALL branches (including concrete) to build complete graph
    // So we don't filter here - let FastGen handle concrete branches
    
    // Marco-compatible: update max_label (track max label in current trace)
    // Note: temp_label is the result of dfsan_union, which returns a label value
    // We need to track the maximum label seen so far in the current trace
    // This matches Marco's logic: if (label > __max_label) __max_label = label;
    // in __taint_union function
    if (temp_label > __marco_max_label) {
        __marco_max_label = temp_label;
    }
    // Also update max_label with the input labels (arg1_label, arg2_label) if they are larger
    // This ensures we track all labels seen, not just the union result
    if (arg1_label > __marco_max_label) {
        __marco_max_label = (uint32_t)arg1_label;
    }
    if (arg2_label > __marco_max_label) {
        __marco_max_label = (uint32_t)arg2_label;
    }

    // Marco-compatible pipe message emission (write to /tmp/wp2 for FastGen's solve function)
    // IMPORTANT: Report ALL branches (including concrete ones with temp_label==0)
    // This ensures complete graph structure, matching our update_graph() modification
    // FastGen will handle concrete branches appropriately (add to graph but not solve)
    
    /* Debug: log __marco_pipe_fd status */
    static int debug_pipe_fd_count = 0;
    if (debug_pipe_fd_count++ < 3) {
        fprintf(stderr, "[SymFit] DEBUG: __marco_pipe_fd=%d, __taint_file_path='%s'\n", __marco_pipe_fd, __taint_file_path);
        fflush(stderr);
    }
    
    // Always write to /tmp/wp2, even if temp_label==0 (concrete branch)
    // This matches our modification to update_graph() which now accepts label==0 branches
    if (__marco_pipe_fd >= 0) {
        /* Debug: log before traceid parsing */
        static int debug_before_traceid = 0;
        if (debug_before_traceid++ < 3) {
            fprintf(stderr, "[SymFit] DEBUG: before traceid parsing: __taint_file_path='%s'\n", __taint_file_path);
            fflush(stderr);
        }
        
        uint32_t label = temp_label;
        
        // IMPORTANT: Use PC passed from TCG translation phase
        // The PC is captured at translation time (s->pc), which is the actual guest instruction PC
        // This is more reliable than cpu_get_tb_cpu_state which may return libc PC if env->eip has been updated
        uint64_t addr_val = pc;  // Use PC passed from TCG translation phase
        
        // Debug: log the PC passed from translation phase
        static int debug_pc_count = 0;
        if (debug_pc_count++ < 100) {
            target_ulong guest_pc_from_env, cs_base;
            uint32_t flags;
            cpu_get_tb_cpu_state(env, &guest_pc_from_env, &cs_base, &flags);
            fprintf(stderr, "[SymFit] DEBUG PC: translation_pc=0x%llx, env->eip=0x%llx, cs_base=0x%llx, runtime_pc=0x%llx\n",
                    (unsigned long long)pc, (unsigned long long)env->eip, (unsigned long long)cs_base, 
                    (unsigned long long)guest_pc_from_env);
            fflush(stderr);
        }
        
        /* Context hash is now updated via HELPER(symsan_notify_call/return) 
         * in TCG translation phase, so we just use the current value */
        uint64_t ctxh_val = __marco_ctxh;
        uint32_t tkdir = result ? 1 : 0;
        
        /* Marco-compatible: get order for (ctxh, addr) pair
         * Similar to Marco's __branches map: tracks call count per (context, PC) */
        uint16_t order = get_branch_order((uint32_t)ctxh_val, addr_val);
        
        /* Use traceid and queueid that were parsed and stored at initialization time
         * This ensures we always use the correct values from the current input file,
         * not from a potentially stale __taint_file_path or TAINT_OPTIONS */
        uint32_t traceid = __marco_traceid;
        uint32_t queueid = __marco_queueid;
        
        /* Debug: log the values being used */
        static int debug_traceid_queueid_count = 0;
        if (debug_traceid_queueid_count++ < 100) {
            fprintf(stderr, "[SymFit] DEBUG: using cached traceid=%u, queueid=%u (from __marco_traceid, __marco_queueid)\n", 
                    traceid, queueid);
            fflush(stderr);
        }
        
        // Log error if traceid is 0 and we haven't initialized (only first few times to avoid spam)
        if (traceid == 0 && __taint_file_path[0] == '\0') {
            static int debug_traceid_zero_count = 0;
            if (debug_traceid_zero_count++ < 3) {
                fprintf(stderr, "[SymFit] ERROR: traceid=0 and __taint_file_path is empty, __marco_traceid=%u, __marco_queueid=%u\n", 
                        __marco_traceid, __marco_queueid);
                fflush(stderr);
            }
        }
        
        /* Marco's solve function expects format: qid,label,direction,addr,ctx,order,cons_type,tid,max_label_
         * - qid: queue id (corresponds to Marco's __tid)
         * - label: dfsan_label
         * - direction: taken direction (0/1, corresponds to Marco's r)
         * - addr: PC address
         * - ctx: context hash (ctxh_val, corresponds to Marco's __taint_trace_callstack)
         * - order: order number (from get_branch_order, corresponds to Marco's __branches map)
         * - cons_type: constraint type (0=conditional, fixed value)
         * - tid: testcase id (traceid, corresponds to Marco's __inputid)
         * - max_label_: maximum label count in current trace (corresponds to Marco's __max_label)
         * 
         * Note: is_good, pp_hash, depth, extra are computed by Marco's update_graph function,
         * not by SymFit. SymFit only provides raw branch information to Marco.
         * 
         * Marco's logic: __max_label is updated in __taint_union: if (label > __max_label) __max_label = label;
         * So we use the maximum of __marco_max_label and the current label to ensure we track the max correctly.
         */
        uint32_t max_label = __marco_max_label;
        // Ensure we use at least the current label value (Marco's __max_label is always >= current label)
        if (label > max_label) {
            max_label = label;
        }
        
        // Debug: log max_label value being written (matching Marco's format exactly)
        static int debug_call_count = 0;
        debug_call_count++;
        if (debug_call_count <= 5) {
            fprintf(stderr, "[SymFit] DEBUG: Writing to /tmp/wp2: queueid=%u, label=%u, tkdir=%lu, addr=0x%lx, ctx=%lu, order=%u, cons_type=0, traceid=%u, max_label=%lu\n", 
                    queueid, label, (unsigned long)tkdir, (unsigned long)addr_val, ctxh_val, order, traceid, (unsigned long)max_label);
            fflush(stderr);
        }
        
        // Marco's exact format: "%u, %u, %lu, %lu, %lu, %u, %u, %u, %lu,\n"
        // Format: qid, label, direction, addr, ctx, order, cons_type, tid, max_label_
        // IMPORTANT: Parameter order must match FastGen's parsing order!
        /* Debug: log traceid before writing (only first few times to avoid spam) */
        static int debug_write_traceid_count = 0;
        if (debug_write_traceid_count++ < 3) {
            fprintf(stderr, "[SymFit] DEBUG: writing to /tmp/wp2: inputid=%u (from '%s'), queueid=%u, label=%u\n", 
                    traceid, __taint_file_path, queueid, label);
            fflush(stderr);
        }
        // Debug: log traceid before writing (especially if it's 0)
        if (traceid == 0) {
            static int debug_traceid_zero_write_count = 0;
            if (debug_traceid_zero_write_count++ < 10) {
                fprintf(stderr, "[SymFit] WARNING: writing with traceid=0! __taint_file_path='%s'\n", 
                        __taint_file_path);
                fflush(stderr);
            }
        }
        
        char rec[512];
        int n = snprintf(rec, sizeof(rec), "%u, %u, %lu, %lu, %lu, %u, %u, %u, %lu,\n",
                         queueid, label, (unsigned long)tkdir, (unsigned long)addr_val, ctxh_val, 
                         order, 0, traceid, (unsigned long)max_label);
        
        // Debug: log the actual string being written
        static int debug_write_count = 0;
        debug_write_count++;
        if (debug_write_count <= 5 || traceid == 0) {
            fprintf(stderr, "[SymFit] DEBUG: Actual data written to /tmp/wp2: %s", rec);
            fflush(stderr);
        }
        
        if (n > 0 && n < (int)sizeof(rec)) {
            if (__marco_pipe_fd < 0) {
                static int debug_pipe_fd_error_count = 0;
                if (debug_pipe_fd_error_count++ < 5) {
                    fprintf(stderr, "[SymFit] ERROR: __marco_pipe_fd=%d, cannot write to /tmp/wp2\n", __marco_pipe_fd);
                    fflush(stderr);
                }
            } else {
                ssize_t wret = write(__marco_pipe_fd, rec, (size_t)n);
                if (wret < 0) {
                    fprintf(stderr, "[SymFit] ERROR: write to /tmp/wp2 failed: errno=%d (%s)\n", errno, strerror(errno));
                    fflush(stderr);
                } else {
                    static int debug_write_success_count = 0;
                    if (debug_write_success_count++ < 5) {
                        fprintf(stderr, "[SymFit] Successfully wrote %zd bytes to /tmp/wp2\n", wret);
                        fflush(stderr);
                    }
                }
                (void)fsync(__marco_pipe_fd);
            }
        } else {
            fprintf(stderr, "[SymFit] ERROR: snprintf failed or buffer too small: n=%d, sizeof(rec)=%zu\n", n, sizeof(rec));
            fflush(stderr);
        }
    }

    // In Marco mode, SymFit only collects constraints and writes to /tmp/wp2
    // All solving is done by Marco's FastGen backend, so we disable SymFit's internal solving
    const char *marco_mode = getenv("MARCO_MODE");
    if (marco_mode && strcmp(marco_mode, "1") == 0) {
        // In Marco mode: only forward condition to SymSan runtime for label tracking
        // Do NOT invoke SymFit's internal solving logic
        // The constraint data has already been written to /tmp/wp2 above
        (void)__taint_trace_cmp(arg1_label, arg2_label, result_bits, result, predicate, arg1, arg2, env->eip);
        // Return concrete result without invoking SymFit's internal solving
        return result;
    }
    
    // Original SymFit solving logic (only used when MARCO_MODE != "1")
    return __taint_trace_cmp(arg1_label, arg2_label, result_bits, result, predicate, arg1, arg2, env->eip);
}

uint64_t HELPER(symsan_setcond_i32)(CPUArchState *env, uint32_t arg1, uint64_t arg1_label,
                              uint32_t arg2, uint64_t arg2_label,
                              int32_t cond, uint32_t result, uint64_t pc)
{
    return symsan_setcond_internal(env, arg1, arg1_label, arg2, arg2_label, cond, result, 32, pc);
}

uint64_t HELPER(symsan_setcond_i64)(CPUArchState *env, uint64_t arg1, uint64_t arg1_label,
                              uint64_t arg2, uint64_t arg2_label,
                              int32_t cond, uint64_t result, uint64_t pc)
{
    return symsan_setcond_internal(env, arg1, arg1_label, arg2, arg2_label, cond, result, 64, pc);
}

/* Guest memory opreation */
static uint64_t symsan_load_guest_internal(CPUArchState *env, target_ulong addr, uint64_t addr_label,
                                     uint64_t load_length, uint8_t result_length)
{
    void *host_addr = g2h(addr);
    
    if (addr_label) {
        // fprintf(stderr, "sym load addr 0x%lx eip 0x%lx\n", addr, env->eip);
        dfsan_label addr_label_new = \
            dfsan_union(addr_label, CONST_LABEL, Equal, 64, addr, 0);
        __taint_trace_cmp(addr_label_new, CONST_LABEL, 64, true, Equal, 0, 0, env->eip);
    }

    uint64_t res_label = dfsan_read_label((uint8_t*)host_addr, load_length);
    
    // Debug: log memory load label (first 50 times)
    static int debug_load_count = 0;
    if (debug_load_count++ < 50) {
        fprintf(stderr, "[SymFit] DEBUG load_guest: addr=0x%llx, host_addr=%p, load_length=%llu, res_label=%llu\n",
                (unsigned long long)addr, host_addr, (unsigned long long)load_length, (unsigned long long)res_label);
        fflush(stderr);
    }
    
    if (qemu_loglevel_mask(CPU_LOG_SYM_LDST_GUEST) && !noSymbolicData) {
        fprintf(stderr, "[memtrace:symbolic]op: load_guest_i%d addr: 0x%lx host_addr: %p size: %ld memory_expr: %ld\n",
                     result_length*8, addr, host_addr, load_length, res_label);
    }
    return res_label;
}

uint64_t HELPER(symsan_load_guest_i32)(CPUArchState *env, target_ulong addr, uint64_t addr_label,
                                 uint64_t length)
{
    return symsan_load_guest_internal(env, addr, addr_label, length, 4);
}

uint64_t HELPER(symsan_load_guest_i64)(CPUArchState *env, target_ulong addr, uint64_t addr_label,
                                 uint64_t length)
{
    return symsan_load_guest_internal(env, addr, addr_label, length, 8);
}


static uint64_t symsan_load_host_internal(void *addr, uint64_t offset,
                                    uint64_t load_length, uint64_t result_length)
{
    assert((uintptr_t)addr+offset >= 0x700000040000);
    uint64_t res_label = dfsan_read_label((uint8_t*)addr + offset, load_length);
    if (qemu_loglevel_mask(CPU_LOG_SYM_LDST_HOST) && !noSymbolicData) {
        fprintf(stderr, "[memtrace:symbolic]op: load_host_i%ld addr: %p size: %ld memory_expr: %ld\n",
                     result_length*8, addr+offset, load_length, res_label);
    }
    return res_label;
}

uint64_t HELPER(symsan_load_host_i32)(void *addr, uint64_t offset, uint64_t length)
{
    return symsan_load_host_internal(addr, offset, length, 4);
}

uint64_t HELPER(symsan_load_host_i64)(void *addr, uint64_t offset, uint64_t length)
{
    return symsan_load_host_internal(addr, offset, length, 8);
}

static void symsan_store_guest_internal(CPUArchState *env, uint64_t value_label,
                                     target_ulong addr, uint64_t addr_label, uint64_t length)
{
    if (qemu_loglevel_mask(CPU_LOG_SYM_LDST_GUEST) && !noSymbolicData) {
        fprintf(stderr, "[memtrace:symbolic]op: store_guest_i%ld addr: 0x%lx size: %ld value_expr: %ld\n",
                     length*8, addr, length, value_label);
    }
    if (addr_label) {
        // fprintf(stderr, "sym store addr 0x%lx eip 0x%lx\n", addr, env->eip);
        dfsan_label addr_label_new = \
            dfsan_union(addr_label, CONST_LABEL, Equal, 64, addr, 0);
        __taint_trace_cmp(addr_label_new, CONST_LABEL, 64, true, Equal, 0, 0, env->eip);
    }

    //void *host_addr = tlb_vaddr_to_host(env, addr, MMU_DATA_STORE, mmu_idx);
    void *host_addr = g2h(addr);
    assert((uintptr_t)host_addr >= 0x700000040000);
    dfsan_store_label(value_label, (uint8_t*)host_addr, length);
    // g_assert_not_reached();

}

void HELPER(symsan_store_guest_i32)(CPUArchState *env, uint64_t value_label,
                                 target_ulong addr, uint64_t addr_label, uint64_t length)
{
    symsan_store_guest_internal(env, value_label, addr, addr_label, length);
}

void HELPER(symsan_store_guest_i64)(CPUArchState *env, uint64_t value_label,
                                 target_ulong addr, uint64_t addr_label, uint64_t length)
{
    symsan_store_guest_internal(env, value_label, addr, addr_label, length);
}

void HELPER(symsan_store_host_i32)(uint64_t value_label,
                                void *addr,
                                uint64_t offset, uint64_t length)
{
    assert((uintptr_t)addr+offset >= 0x700000040000);
    if (qemu_loglevel_mask(CPU_LOG_SYM_LDST_HOST) && !noSymbolicData) {
        fprintf(stderr, "[memtrace:symbolic] op: store_host_i32 addr: %p value_label: %ld length %ld\n",
                        addr+offset, value_label, length);
    }
    dfsan_store_label(value_label, (uint8_t*)addr + offset, length);
}

void HELPER(symsan_store_host_i64)(uint64_t value_label,
                                void *addr,
                                uint64_t offset, uint64_t length)
{
    if (qemu_loglevel_mask(CPU_LOG_SYM_LDST_HOST) && !noSymbolicData) {
        fprintf(stderr, "[memtrace:symbolic] op: store_host_i64 addr: %p value_label: %ld length %ld\n",
                        addr+offset, value_label, length);
    }
    assert((uintptr_t)addr+offset >= 0x700000040000);
    dfsan_store_label(value_label, (uint8_t*)addr + offset, length);
}


// concrete mode
/* Monitor load in concrete mode, if load symbolic data, switch to symbolic mode
 * currently, we do this in the translation backend.
 */
void HELPER(symsan_check_load_guest)(CPUArchState *env, target_ulong addr, uint64_t length) {
    void *host_addr = g2h(addr);
    assert((uintptr_t)host_addr >= 0x700000040000);
    uint32_t res_label = dfsan_read_label((uint8_t*)host_addr, length);
    if (res_label != 0) {
        if (qemu_loglevel_mask(CPU_LOG_SYM_LDST_GUEST) && !noSymbolicData) {
            // fprintf(stderr, "[memtrace:switch] op: load_guest addr: 0x%lx host_addr %p mode: concrete\n",
            //                         addr, host_addr);
        }
        second_ccache_flag = 1;
        raise_exception_err_ra(env, EXCP_SWITCH, 0, GETPC());
    }
}
void HELPER(symsan_check_store_guest)(target_ulong addr, uint64_t length){
    assert(second_ccache_flag != 1);
    uint32_t value_label = 0;
    //void *host_addr = tlb_vaddr_to_host(env, addr, MMU_DATA_STORE, mmu_idx);
    void *host_addr = g2h(addr);
    assert((uintptr_t)host_addr >= 0x700000040000);
    // if (!noSymbolicData)
    // fprintf(stderr, "[memtrace] op: check_store_guest addr: 0x%lx mode: concrete\n", addr);
    dfsan_store_label(value_label, (uint8_t*)host_addr, length);
}

/* Check the register status at the end of one basic block in symbolic mode
 * if there is no symbolic registers, switch to concrete mode
 */
void HELPER(symsan_check_state_switch)(CPUArchState *env) {
    int symbolic_flag = 0;
    for (int i=0; i<CPU_NB_REGS;i++) {
        if (env->shadow_regs[i]){
            symbolic_flag = 1;
            break;
        }
    }
    if (symbolic_flag) {
        second_ccache_flag = 1;
        //if (!noSymbolicData) fprintf(stderr, "block 0x%lx state symbolic\n", env->eip);
        return;
    }
    if (env->shadow_cc_dst || env->shadow_cc_src || env->shadow_cc_src2) {
        symbolic_flag = 1;
    }
    if (!symbolic_flag && sse_operation) {
        int size = sizeof(env->xmm_regs);
        uintptr_t xmm_reg_addr = (uintptr_t)env->xmm_regs;
        uint64_t xmm_reg = 0;
        for (uintptr_t addr = xmm_reg_addr; addr < xmm_reg_addr + size; addr+=8) {
            xmm_reg = dfsan_read_label((uint8_t *)addr, 8);
            if (xmm_reg != 0) {
                symbolic_flag = 1;
                break;
            }
        }
    }
    second_ccache_flag = symbolic_flag;
    if (second_ccache_flag == 0) {
        CPUState *cs = env_cpu(env);
        cpu_loop_exit_noexc(cs);
    }
}
void HELPER(symsan_check_state)(CPUArchState *env) {
    int symbolic_flag = 0;
    for (int i=0; i<CPU_NB_REGS;i++) {
        if (env->shadow_regs[i]) {
            symbolic_flag = 1;
            break;
        }
    }
    if (symbolic_flag) {
        second_ccache_flag = 1;
        return;
    }
    if (env->shadow_cc_dst || env->shadow_cc_src || env->shadow_cc_src2) {
        symbolic_flag = 1;
    }
    if (!symbolic_flag && sse_operation) {
        int size = sizeof(env->xmm_regs);
        uintptr_t xmm_reg_addr = (uintptr_t)env->xmm_regs;
        uint64_t xmm_reg = 0;
        for (uintptr_t addr = xmm_reg_addr; addr < xmm_reg_addr + size; addr+=8) {
            xmm_reg = dfsan_read_label((uint8_t *)addr, 8);
            if (xmm_reg != 0) {
                symbolic_flag = 1;
                break;
            }
        }
    }
    second_ccache_flag = symbolic_flag;
}

void HELPER(symsan_check_state_no_sse)(CPUArchState *env) {
    int symbolic_flag = 0;
    for (int i=0; i<CPU_NB_REGS;i++) {
        if (env->shadow_regs[i]){
            symbolic_flag = 1;
            break;
        }
    }
    if (symbolic_flag) {
        second_ccache_flag = 1;
        //if (!noSymbolicData) fprintf(stderr, "block 0x%lx state symbolic\n", env->eip);
        return;
    }
    if (env->shadow_cc_dst || env->shadow_cc_src || env->shadow_cc_src2) {
        symbolic_flag = 1;
    }
    second_ccache_flag = symbolic_flag;
    // if (!noSymbolicData) fprintf(stderr, "block 0x%lx state %s\n", env->eip, second_ccache_flag?"symbolic":"concrete");
    if (second_ccache_flag == 0) {
        CPUState *cs = env_cpu(env);
        cpu_loop_exit_noexc(cs);
    }
}

/* Helper function to notify function call (called from TCG translation) */
void HELPER(symsan_notify_call)(uint64_t call_pc)
{
    /* Update context hash on function call (TaintPass.cc style) */
    update_context_on_call(call_pc);
}

/* Helper function to notify function return (called from TCG translation) */
void HELPER(symsan_notify_return)(uint64_t ret_pc)
{
    /* Restore context hash on function return (TaintPass.cc style) */
    restore_context_on_return(ret_pc);
}

