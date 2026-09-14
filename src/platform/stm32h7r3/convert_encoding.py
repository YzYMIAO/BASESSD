#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
文件编码检查与转换工具（纯Python版，无第三方依赖）
功能：递归扫描当前目录，检测文件编码，转换为UTF-8
"""

import os
import codecs
from pathlib import Path

# 配置：要处理的文件扩展名（None表示处理所有文件）
TARGET_EXTENSIONS = {
    '.c', '.h', '.cpp', '.hpp', '.cc', '.cxx', '.c++',
    '.py', '.pyw', '.txt', '.md', '.rst',
    '.xml', '.json', '.yaml', '.yml', '.toml',
    '.html', '.htm', '.css', '.js', '.ts',
    '.sh', '.bash', '.zsh', '.fish',
    '.cfg', '.conf', '.ini', '.properties',
    '.s', '.S', '.asm',  # 汇编文件
    '.ld', '.lds',       # 链接脚本
    '.cmake', '.make', '.mk',
}

# 排除的目录
EXCLUDE_DIRS = {
    '.git', '.svn', '.hg', '__pycache__',
    'node_modules', '.venv', 'venv', 'env',
    '.idea', '.vscode', 'build', 'dist',
    '.pytest_cache', '.mypy_cache', '.tox',
}

# 排除的文件
EXCLUDE_FILES = {
    '.DS_Store', 'Thumbs.db', 'desktop.ini',
}


def detect_encoding(file_path):
    """检测文件编码（使用BOM和常见编码尝试）"""
    
    # 常见编码列表（按优先级排序）
    common_encodings = [
        'utf-8-sig',   # 带BOM的UTF-8
        'utf-8',       # 标准UTF-8
        'gbk',         # 简体中文
        'gb2312',      # 简体中文（旧）
        'big5',        # 繁体中文
        'shift_jis',   # 日文
        'euc_jp',      # 日文
        'euc_kr',      # 韩文
        'cp949',       # 韩文（Windows）
        'latin-1',     # 西欧（ISO-8859-1）
        'cp1252',      # Windows西欧
        'ascii',       # 纯ASCII
    ]
    
    try:
        with open(file_path, 'rb') as f:
            raw_data = f.read()
        
        # 检查BOM（字节顺序标记）
        if raw_data.startswith(codecs.BOM_UTF8):
            return 'utf-8-sig', 1.0
        if raw_data.startswith(codecs.BOM_UTF16_LE):
            return 'utf-16-le', 1.0
        if raw_data.startswith(codecs.BOM_UTF16_BE):
            return 'utf-16-be', 1.0
        if raw_data.startswith(codecs.BOM_UTF32_LE):
            return 'utf-32-le', 1.0
        if raw_data.startswith(codecs.BOM_UTF32_BE):
            return 'utf-32-be', 1.0
        
        # 尝试用各种编码解码
        for encoding in common_encodings:
            try:
                raw_data.decode(encoding)
                # 如果是ASCII，优先用ascii
                if encoding == 'ascii':
                    return 'ascii', 1.0
                return encoding, 0.95
            except UnicodeDecodeError:
                continue
        
        return None, 0.0
        
    except Exception as e:
        print(f"  ⚠️  检测失败: {e}")
        return None, 0.0


def convert_to_utf8(file_path, original_encoding):
    """将文件转换为UTF-8"""
    try:
        # 特殊处理：如果是ascii，直接写入UTF-8
        if original_encoding == 'ascii':
            with open(file_path, 'r', encoding='ascii') as f:
                content = f.read()
            with open(file_path, 'w', encoding='utf-8') as f:
                f.write(content)
            return True
        
        # 尝试用检测到的编码读取
        try:
            with open(file_path, 'r', encoding=original_encoding) as f:
                content = f.read()
        except UnicodeDecodeError:
            # 如果失败，尝试用latin-1（能解码任何字节）
            with open(file_path, 'r', encoding='latin-1') as f:
                content = f.read()
        
        # 写入UTF-8
        with open(file_path, 'w', encoding='utf-8') as f:
            f.write(content)
        return True
        
    except Exception as e:
        print(f"  ❌ 转换失败: {e}")
        return False


def should_skip_file(file_path):
    """判断是否应该跳过该文件"""
    filename = os.path.basename(file_path)
    
    # 排除特定文件
    if filename in EXCLUDE_FILES:
        return True
    
    # 排除隐藏文件（以.开头）
    if filename.startswith('.'):
        return True
    
    # 检查文件扩展名
    ext = os.path.splitext(filename)[1].lower()
    if TARGET_EXTENSIONS and ext not in TARGET_EXTENSIONS:
        return True
    
    return False


def scan_and_convert(root_dir='.'):
    """扫描并转换所有文件"""
    root = Path(root_dir)
    
    print("=" * 70)
    print("文件编码检查与转换工具 (纯Python版，无依赖)")
    print("=" * 70)
    print(f"扫描目录: {root.absolute()}")
    print(f"处理扩展名: {', '.join(sorted(TARGET_EXTENSIONS))}")
    print(f"排除目录: {', '.join(EXCLUDE_DIRS)}")
    print("=" * 70)
    
    stats = {
        'total': 0,
        'converted': 0,
        'skipped': 0,
        'errors': 0,
        'already_utf8': 0,
    }
    
    converted_files = []
    
    for file_path in root.rglob('*'):
        if not file_path.is_file():
            continue
        
        # 跳过隐藏目录中的文件
        if any(part.startswith('.') for part in file_path.parts):
            continue
        
        # 检查是否跳过目录
        parent_dirs = file_path.parent.parts
        if any(d in EXCLUDE_DIRS for d in parent_dirs):
            continue
        
        # 检查是否跳过文件
        if should_skip_file(str(file_path)):
            stats['skipped'] += 1
            continue
        
        stats['total'] += 1
        
        # 检测编码
        encoding, confidence = detect_encoding(str(file_path))
        
        rel_path = file_path.relative_to(root)
        
        if encoding is None:
            print(f"  ❌ {rel_path}: 无法检测编码")
            stats['errors'] += 1
            continue
        
        # 如果已经是UTF-8或ASCII，跳过
        if encoding.lower() in ['utf-8', 'utf-8-sig', 'ascii']:
            status = "UTF-8 (带BOM)" if encoding == 'utf-8-sig' else "UTF-8"
            print(f"  ✅ {rel_path}: {status} - 跳过")
            stats['already_utf8'] += 1
            continue
        
        # 转换为UTF-8
        print(f"  🔄 {rel_path}: {encoding} ({confidence:.1%}) -> UTF-8")
        if convert_to_utf8(str(file_path), encoding):
            stats['converted'] += 1
            converted_files.append(str(rel_path))
        else:
            stats['errors'] += 1
    
    # 打印统计结果
    print("=" * 70)
    print("转换完成！统计结果：")
    print(f"  总文件数:    {stats['total']}")
    print(f"  已是UTF-8:   {stats['already_utf8']}")
    print(f"  已转换:      {stats['converted']}")
    print(f"  已跳过:      {stats['skipped']}")
    print(f"  错误:        {stats['errors']}")
    
    if converted_files:
        print("\n已转换的文件列表：")
        for f in converted_files:
            print(f"    - {f}")
    
    print("=" * 70)
    
    # 提示git状态
    if stats['converted'] > 0:
        print("\n⚠️  检测到文件已转换，建议使用 'git diff' 检查变更。")


if __name__ == '__main__':
    import sys
    root_dir = sys.argv[1] if len(sys.argv) > 1 else '.'
    scan_and_convert(root_dir)

