#!/bin/bash

# 通用函数声明提取脚本 - 支持ANSI、GNU、K&R等各种代码风格
# 功能：从C源文件中提取函数声明，追加到头文件末尾
# 用法: 
#   ./extract_func_decl.sh -f <source.c> -o <header.h>
#   ./extract_func_decl.sh -f <source.c>              # 自动生成同名.h
#   ./extract_func_decl.sh -d <source_dir> -o <header_dir>  # 处理整个目录

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# 显示帮助
show_help() {
    cat << EOF
用法: $0 [选项]

选项:
    -f <源文件>     指定要处理的.c文件（支持相对路径和绝对路径）
    -o <头文件>     指定输出的.h文件路径（与-f配合使用）
    -d <源目录>     指定要处理的源目录（递归查找所有.c文件）
    -o <头目录>     指定输出的头文件目录（与-d配合使用）
    -h, --help      显示帮助信息

示例:
    # 单文件处理
    $0 -f ../src/ds_array.c -o ../include/ds_array.h
    $0 -f ds_array.c -o ds_array.h
    $0 -f src/ds_array.c                    # 自动生成 src/ds_array.h
    
    # 目录批量处理（指定输出目录）
    $0 -d ../src/data_structure/ -o ../include/data_structure/
    $0 -d src/ -o include/                  # 保持目录结构
    $0 -d . -o ../include/                  # 处理当前目录，输出到../include/

注意:
    - 头文件必须已存在，脚本只在末尾追加，不修改原有内容
    - 如果已有自动生成标记，会替换旧的声明
    - 目录处理时会保持相对路径结构
EOF
}

# 获取绝对路径（处理包含..的路径）
get_abs_path() {
    local path="$1"
    if [[ "$path" = /* ]]; then
        echo "$path"
        return
    fi
    
    local current_dir="$(pwd)"
    
    if command -v realpath >/dev/null 2>&1; then
        realpath "$path" 2>/dev/null || echo "$current_dir/$path"
    elif command -v readlink >/dev/null 2>&1; then
        readlink -f "$path" 2>/dev/null || echo "$current_dir/$path"
    else
        local dir=$(dirname "$path")
        local file=$(basename "$path")
        if [ "$dir" = "." ]; then
            echo "$current_dir/$file"
        elif [ -d "$dir" ]; then
            local abs_dir="$(cd "$dir" 2>/dev/null && pwd)"
            if [ -n "$abs_dir" ]; then
                echo "$abs_dir/$file"
            else
                echo "$current_dir/$path"
            fi
        else
            echo "$current_dir/$path"
        fi
    fi
}

# 获取相对路径
get_rel_path() {
    local target="$1"
    local base="$2"
    
    # 如果target是绝对路径，转换为相对于base的路径
    if [[ "$target" = /* ]]; then
        # 使用realpath获取相对路径
        if command -v realpath >/dev/null 2>&1; then
            realpath --relative-to="$base" "$target" 2>/dev/null || echo "$target"
        else
            echo "$target"
        fi
    else
        echo "$target"
    fi
}

# 提取函数声明的核心函数
extract_functions() {
    local source_file="$1"
    
    awk '
    BEGIN {
        in_func = 0
        in_comment = 0
        func_decl = ""
        func_count = 0
    }
    
    # 处理多行注释
    /\/\*/ {
        if (!in_comment) {
            in_comment = 1
        }
        if ($0 ~ /\*\//) {
            in_comment = 0
            next
        }
        next
    }
    /\*\// {
        if (in_comment) {
            in_comment = 0
            next
        }
    }
    in_comment { next }
    
    # 跳过单行注释
    /^[[:space:]]*\/\// { next }
    
    # 跳过预处理指令
    /^[[:space:]]*#/ { next }
    
    # 跳过空行
    /^[[:space:]]*$/ { next }
    
    {
        line = $0
        gsub(/^[[:space:]]+/, "", line)
        gsub(/[[:space:]]+$/, "", line)
        
        # 单行函数定义
        if (!in_func && line ~ /^[a-zA-Z_][a-zA-Z0-9_]*[[:space:]]+[a-zA-Z_][a-zA-Z0-9_]*[[:space:]]*\(/) {
            if (line ~ /^(if|while|for|switch|return|sizeof|else|do|case|default|typedef|struct|union|enum)/) {
                next
            }
            
            if (line ~ /\{/) {
                func_line = line
                gsub(/\{.*$/, "", func_line)
                gsub(/[[:space:]]+/, " ", func_line)
                gsub(/^[[:space:]]+/, "", func_line)
                gsub(/[[:space:]]+$/, "", func_line)
                if (func_line ~ /\([^)]*\)$/) {
                    print func_line ";"
                    func_count++
                }
                next
            }
        }
        
        # 检测函数定义开始
        if (!in_func && line ~ /^[a-zA-Z_][a-zA-Z0-9_]*[[:space:]]+[a-zA-Z_][a-zA-Z0-9_]*[[:space:]]*\(/) {
            if (line ~ /^(if|while|for|switch|return|sizeof|else|do|case|default|typedef|struct|union|enum)/) {
                next
            }
            
            if (line ~ /;$/) {
                next
            }
            
            in_func = 1
            func_decl = line
            
            if (line ~ /\{/) {
                gsub(/\{.*$/, "", func_decl)
                gsub(/[[:space:]]+/, " ", func_decl)
                gsub(/^[[:space:]]+/, "", func_decl)
                gsub(/[[:space:]]+$/, "", func_decl)
                if (func_decl ~ /\([^)]*\)$/) {
                    print func_decl ";"
                    func_count++
                }
                in_func = 0
                func_decl = ""
                next
            }
            
            next
        }
        
        # 在函数定义中
        if (in_func) {
            if (line == "{") {
                if (func_decl != "") {
                    gsub(/[[:space:]]+/, " ", func_decl)
                    gsub(/^[[:space:]]+/, "", func_decl)
                    gsub(/[[:space:]]+$/, "", func_decl)
                    gsub(/;$/, "", func_decl)
                    if (func_decl ~ /\)$/) {
                        print func_decl ";"
                        func_count++
                    }
                }
                in_func = 0
                func_decl = ""
                next
            }
            
            if (line ~ /\{/) {
                func_decl = func_decl " " line
                gsub(/\{.*$/, "", func_decl)
                gsub(/[[:space:]]+/, " ", func_decl)
                gsub(/^[[:space:]]+/, "", func_decl)
                gsub(/[[:space:]]+$/, "", func_decl)
                gsub(/;$/, "", func_decl)
                if (func_decl ~ /\)$/) {
                    print func_decl ";"
                    func_count++
                }
                in_func = 0
                func_decl = ""
                next
            }
            
            # K&R风格参数声明
            if (line ~ /^[a-zA-Z_][a-zA-Z0-9_]*[[:space:]]+[a-zA-Z_][a-zA-Z0-9_]*;/) {
                next
            }
            
            if (line ~ /;$/) {
                next
            }
            
            func_decl = func_decl " " line
        }
    }
    
    END {
        if (func_count == 0) {
            print "/* 警告: 没有找到任何函数定义 */"
        }
    }
    ' "$source_file"
}

# 追加函数声明到头文件
append_to_header() {
    local source_file="$1"
    local header_file="$2"
    
    # 转换为绝对路径
    local abs_source=$(get_abs_path "$source_file")
    local abs_header=$(get_abs_path "$header_file")
    
    echo -e "\n${BLUE}========================================${NC}"
    echo -e "${BLUE}处理文件:${NC}"
    echo -e "  源文件: $abs_source"
    echo -e "  头文件: $abs_header"
    echo -e "${BLUE}========================================${NC}"
    
    # 检查源文件是否存在
    if [ ! -f "$abs_source" ]; then
        echo -e "${RED}错误: 源文件 '$abs_source' 不存在${NC}"
        return 1
    fi
    
    # 检查头文件是否存在
    if [ ! -f "$abs_header" ]; then
        echo -e "${RED}错误: 头文件 '$abs_header' 不存在${NC}"
        echo -e "${YELLOW}提示: 请先手动创建头文件${NC}"
        return 1
    fi
    
    # 提取函数声明
    local func_temp=$(mktemp)
    extract_functions "$abs_source" > "$func_temp"
    
    # 检查是否提取到函数
    if [ ! -s "$func_temp" ] || grep -q "警告" "$func_temp"; then
        echo -e "${YELLOW}警告: 没有找到函数定义${NC}"
        rm -f "$func_temp"
        return 0
    fi
    
    local total=$(cat "$func_temp" | grep -c ";")
    echo -e "${GREEN}提取到 $total 个函数声明${NC}"
    
    # 检查是否已有自动生成标记
    if grep -q "/\* ===== 自动生成的函数声明 =====" "$abs_header"; then
        echo -e "${YELLOW}检测到已有自动生成的声明，将替换${NC}"
        # 创建备份
        local backup_file="${abs_header}.bak.$(date +%Y%m%d_%H%M%S)"
        cp "$abs_header" "$backup_file"
        echo -e "${GREEN}已创建备份: $backup_file${NC}"
        # 删除旧的自动生成部分
        sed -i '/\/\* ===== 自动生成的函数声明 ===== \*\//,/\/\* ===== 结束自动生成的函数声明 ===== \*\//d' "$abs_header"
    fi
    
    # 确保文件末尾有换行
    echo "" >> "$abs_header"
    
    # 追加函数声明
    local src_basename=$(basename "$abs_source")
    cat >> "$abs_header" << EOF
/* ===== 自动生成的函数声明 ===== */
/* 以下函数声明从 $src_basename 自动提取 */
/* 请手动筛选，删除不需要导出的函数 */

$(cat "$func_temp")

/* ===== 结束自动生成的函数声明 ===== */
EOF
    
    echo -e "${GREEN}✓ 完成! 已追加到: $abs_header${NC}"
    echo -e "${GREEN}共追加 $total 个函数声明${NC}"
    
    # 显示前几个
    echo -e "\n${YELLOW}提取的函数声明（前10个）:${NC}"
    cat "$func_temp" | head -10 | sed 's/^/  /'
    if [ $total -gt 10 ]; then
        echo "  ... 还有 $(($total - 10)) 个"
    fi
    
    rm -f "$func_temp"
    return 0
}

# 处理目录
process_directory() {
    local source_dir="$1"
    local header_dir="$2"
    
    # 转换为绝对路径
    local abs_source_dir=$(get_abs_path "$source_dir")
    local abs_header_dir=$(get_abs_path "$header_dir")
    
    echo -e "${BLUE}========================================${NC}"
    echo -e "${BLUE}扫描目录:${NC}"
    echo -e "  源目录: $abs_source_dir"
    echo -e "  头目录: $abs_header_dir"
    echo -e "${BLUE}========================================${NC}"
    
    if [ ! -d "$abs_source_dir" ]; then
        echo -e "${RED}错误: 源目录 '$abs_source_dir' 不存在${NC}"
        return 1
    fi
    
    if [ ! -d "$abs_header_dir" ]; then
        echo -e "${YELLOW}警告: 头目录 '$abs_header_dir' 不存在，将创建${NC}"
        mkdir -p "$abs_header_dir"
    fi
    
    # 查找所有.c文件（递归）
    local c_files=$(find "$abs_source_dir" -name "*.c" -type f)
    
    if [ -z "$c_files" ]; then
        echo -e "${RED}错误: 在源目录中找不到任何.c文件${NC}"
        return 1
    fi
    
    local total_files=$(echo "$c_files" | wc -l)
    echo -e "${GREEN}找到 $total_files 个C文件${NC}"
    
    local success=0
    local fail=0
    
    for file in $c_files; do
        # 计算相对路径
        local rel_path="${file#$abs_source_dir/}"
        local header_file="$abs_header_dir/${rel_path%.c}.h"
        
        # 确保头文件目录存在
        local header_dir_path=$(dirname "$header_file")
        mkdir -p "$header_dir_path"
        
        # 如果头文件不存在，创建空的头文件
        if [ ! -f "$header_file" ]; then
            echo -e "${YELLOW}创建头文件: $header_file${NC}"
            local basename=$(basename "$file" .c)
            local guard_name=$(echo "${basename}_H" | tr '[:lower:]' '[:upper:]')
            cat > "$header_file" << EOF
#ifndef ${guard_name}
#define ${guard_name}

#include <stddef.h>
#include <stdbool.h>

/* 请在这里添加你的类型定义和函数声明 */

#ifdef __cplusplus
extern "C" {
#endif



#ifdef __cplusplus
}
#endif

#endif /* ${guard_name} */
EOF
        fi
        
        if append_to_header "$file" "$header_file"; then
            ((success++))
        else
            ((fail++))
        fi
    done
    
    echo -e "\n${GREEN}========================================${NC}"
    echo -e "${GREEN}处理完成: 成功 $success 个，失败 $fail 个${NC}"
}

# 主程序
main() {
    local source_file=""
    local header_file=""
    local source_dir=""
    local header_dir=""
    
    # 解析命令行参数
    while [[ $# -gt 0 ]]; do
        case $1 in
            -f)
                source_file="$2"
                shift 2
                ;;
            -d)
                source_dir="$2"
                shift 2
                ;;
            -o)
                # 如果已经设置了source_dir，则header_dir是目录
                # 否则header_file是文件
                if [ -n "$source_dir" ]; then
                    header_dir="$2"
                else
                    header_file="$2"
                fi
                shift 2
                ;;
            -h|--help)
                show_help
                exit 0
                ;;
            *)
                echo -e "${RED}错误: 未知参数 '$1'${NC}"
                show_help
                exit 1
                ;;
        esac
    done
    
    # 检查参数
    if [ -n "$source_dir" ] && [ -n "$source_file" ]; then
        echo -e "${RED}错误: 不能同时指定目录和文件${NC}"
        exit 1
    fi
    
    if [ -n "$source_dir" ]; then
        # 目录模式
        if [ -z "$header_dir" ]; then
            # 如果没有指定输出目录，使用源目录
            header_dir="$source_dir"
            echo -e "${YELLOW}未指定头文件目录，使用源目录: $header_dir${NC}"
        fi
        process_directory "$source_dir" "$header_dir"
    elif [ -n "$source_file" ]; then
        # 单文件模式
        if [ -z "$header_file" ]; then
            header_file="${source_file%.c}.h"
            echo -e "${YELLOW}未指定头文件，使用: $header_file${NC}"
        fi
        append_to_header "$source_file" "$header_file"
    else
        echo -e "${RED}错误: 请指定要处理的文件或目录${NC}"
        show_help
        exit 1
    fi
}

# 运行主程序
main "$@"