# -*- coding: utf-8 -*-
"""扫描 DuduClock 2.2 UI 代码实际用到的字符集"""
import sys, re, os
sys.stdout.reconfigure(encoding='utf-8', errors='replace')

base = r'E:\ESP\五邑大学水电站\DuduClock_2.2源码包(1)\DuduClock_2.2'
files = ['DuduClock_2.2.ino', 'task.cpp', 'net.cpp', 'tftUtil.cpp',
         'preferencesUtil.cpp', 'common.h']

chars = set()

# 1. 双引号 C 字符串里的字符
str_re = re.compile(r'"((?:[^"\\]|\\.)*)"')
for f in files:
    t = open(os.path.join(base, f), encoding='utf-8', errors='ignore').read()
    for m in str_re.finditer(t):
        for ch in m.group(1):
            chars.add(ch)

# 2. 代码注释里的中文也抓（UI 文案常在注释里）
for f in files:
    t = open(os.path.join(base, f), encoding='utf-8', errors='ignore').read()
    for ch in t:
        if ord(ch) > 0x2000:  # CJK 及以后
            chars.add(ch)

# 3. 天气状况名称（和风 API 返回，getWea 里写死）
for c in '雪雷沙尘雾冰雹多云雨阴晴':
    chars.add(c)

# 4. 星期/日期/空气质量等级
for c in '周日一二三四五六年月日时分秒':
    chars.add(c)
for c in '优良中差':
    chars.add(c)

# 5. 温湿度符号、数字、常用标点
for c in '0123456789:-.%℃/+ ':
    chars.add(c)

cjk = sorted([c for c in chars if ord(c) > 0x2000], key=ord)
asc = sorted([c for c in chars if ord(c) <= 0x2000], key=ord)

print('CJK 字符 %d 个:' % len(cjk))
print(''.join(cjk))
print()
print('ASCII 字符 %d 个:' % len(asc))
print(''.join(asc))
