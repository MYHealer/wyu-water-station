# -*- coding: utf-8 -*-
"""生成 DuduClock UI 所需的精确字符集，供 lv_font_conv 使用"""
import sys
sys.stdout.reconfigure(encoding='utf-8', errors='replace')

# ---- 1. 固定 UI 文案（来自 task.cpp / net.cpp / tftUtil.cpp 的 drawString）----
UI = [
    'Dudu Clock',              # 标题
    '系统启动中...',
    '正在连接网络...',
    '同步天气数据',
    '网络连接失败',
    '城市名称无效',
    '即将尝试连接',
    '已恢复出厂设置',
    '6日天气预报',
    '空气质量指数',
    '计时器',
    '还原设置',
    '主题切换',
    '单击开启/停止计时',
    '长按3秒计数器归零',
    '长按3秒钟',
    '恢复出厂设置',
    '切换白色主题',
    '切换黑色主题',
]

# ---- 2. 动态内容 ----
WEEK    = '周日周一周二周三周四周五周六'
DATE    = '年月日'                    # + 数字
AIR_LVL = '优良中差'                  # 空气质量等级
AIR_ITEM = ['颗粒物', 'PM10', 'PM2.5', '二氧化氮', 'NO2', '二氧化硫', 'SO2',
            '一氧化碳', 'CO', '臭氧', 'O3']
WEATHER = '晴阴多云雨雪雷沙尘雾冰雹'   # getWea() 全部返回值
SCROLL  = ['今日', '体感温度', '能见度', '空气指数']  # 轮播文案
TIMER   = ['H']                        # 计时器单位
COUNTDOWN = '秒后系统重启'

# ---- 3. 符号与数字 ----
SYMBOL = '0123456789' \
         'ABCDEFGHIJKLMNOPQRSTUVWXYZ' \
         'abcdefghijklmnopqrstuvwxyz' \
         '℃%:-./+° 　'   # 含全角空格

chars = set()
for s in UI + [WEEK, DATE, AIR_LVL] + AIR_ITEM + [WEATHER] + SCROLL + TIMER + [COUNTDOWN]:
    chars.update(s)
chars.update(SYMBOL)

cjk = sorted([c for c in chars if ord(c) > 0x2000], key=ord)
asc = sorted([c for c in chars if ord(c) <= 0x2000], key=ord)

print('总字符 %d (CJK %d, ASCII %d)' % (len(chars), len(cjk), len(asc)))
print()
print('CJK:', ''.join(cjk))
print()
print('ASC:', ''.join(asc))

# 输出纯字符集（lv_font_conv --symbols 用）
open(r'E:\ESP\dudu-clock-idf\tools\charset.txt', 'w', encoding='utf-8').write(
    ''.join(sorted(chars, key=ord)))
print()
print('已写入 tools/charset.txt')
