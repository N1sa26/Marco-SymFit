#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    printf("=== LLVMFuzzerTestOneInput called ===\n");
    printf("Input size: %zu\n", size);
    
    if (size < 1) return 0;
    
    // 多个条件分支，增加符号执行的复杂性
    int result = 0;
    
    // 第一个分支：检查第一个字符
    if (data[0] == 'A') {
        printf("First char is A\n");
        result += 1;
    } else if (data[0] == 'B') {
        printf("First char is B\n");
        result += 2;
    } else if (data[0] == 'C') {
        printf("First char is C\n");
        result += 3;
    } else {
        printf("First char is other: %c\n", data[0]);
        result += 4;
    }
    
    // 第二个分支：检查长度
    if (size > 5) {
        printf("Length > 5\n");
        result += 10;
        
        // 嵌套分支：检查第二个字符
        if (size > 1) {
            if (data[1] == 'X') {
                printf("Second char is X\n");
                result += 20;
            } else if (data[1] == 'Y') {
                printf("Second char is Y\n");
                result += 30;
            } else {
                printf("Second char is other: %c\n", data[1]);
                result += 40;
            }
        }
    } else {
        printf("Length <= 5\n");
        result += 5;
    }
    
    // 第三个分支：字符串比较
    if (size >= 4) {
        if (strncmp((const char*)data, "TEST", 4) == 0) {
            printf("Input starts with TEST\n");
            result += 100;
        } else if (strncmp((const char*)data, "HELLO", 5) == 0) {
            printf("Input starts with HELLO\n");
            result += 200;
        } else if (strncmp((const char*)data, "WORLD", 5) == 0) {
            printf("Input starts with WORLD\n");
            result += 300;
        } else {
            printf("Input does not start with known patterns\n");
            result += 50;
        }
    }
    
    // 第四个分支：数值计算
    if (size >= 3) {
        int sum = 0;
        for (size_t i = 0; i < size && i < 10; i++) {
            sum += data[i];
        }
        
        if (sum > 500) {
            printf("Sum > 500: %d\n", sum);
            result += 1000;
        } else if (sum > 300) {
            printf("Sum > 300: %d\n", sum);
            result += 500;
        } else if (sum > 100) {
            printf("Sum > 100: %d\n", sum);
            result += 100;
        } else {
            printf("Sum <= 100: %d\n", sum);
            result += 10;
        }
    }
    
    // 第五个分支：循环中的条件
    int even_count = 0;
    int odd_count = 0;
    for (size_t i = 0; i < size && i < 20; i++) {
        if (data[i] % 2 == 0) {
            even_count++;
        } else {
            odd_count++;
        }
    }
    
    if (even_count > odd_count) {
        printf("More even chars: %d vs %d\n", even_count, odd_count);
        result += 2000;
    } else if (odd_count > even_count) {
        printf("More odd chars: %d vs %d\n", even_count, odd_count);
        result += 3000;
    } else {
        printf("Equal even/odd chars: %d vs %d\n", even_count, odd_count);
        result += 1500;
    }
    
    // 第六个分支：复杂的条件组合
    if (size >= 6) {
        int pattern_match = 0;
        if (data[0] == data[2]) pattern_match++;
        if (data[1] == data[3]) pattern_match++;
        if (data[2] == data[4]) pattern_match++;
        
        if (pattern_match >= 2) {
            printf("Pattern match: %d\n", pattern_match);
            result += 5000;
        } else if (pattern_match == 1) {
            printf("Partial pattern match: %d\n", pattern_match);
            result += 2500;
        } else {
            printf("No pattern match: %d\n", pattern_match);
            result += 1000;
        }
    }
    
    printf("Final result: %d\n", result);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <input_file>\n", argv[0]);
        return 1;
    }
    FILE *f = fopen(argv[1], "r");
    if (!f) {
        perror("Failed to open input file");
        return 1;
    }
    fseek(f, 0, SEEK_END);
    size_t len = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = (unsigned char*)malloc(len + 1);
    size_t n_read = fread(buf, 1, len, f);
    fclose(f);
    buf[n_read] = '\0';
    LLVMFuzzerTestOneInput(buf, n_read);
    free(buf);
    return 0;
}

