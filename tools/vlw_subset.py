# -*- coding: utf-8 -*-
"""
vlw 字体解析 + 子集化工具

vlw (TFT_eSPI smooth font / Processing VLW) 结构:
  文件头 12B: [glyphCount:u32][fontSize:u32][version:u32]
  字形头 16B: [code:u32][w:u16][h:u16][xAdvance:i16][dX:i16][dY:i16][pad:u16]
  位图: w*h 字节, 8bit alpha
"""
import struct, sys, os

def parse_h(path):
    """从 .h 里提取 uint8 数组字节"""
    t = open(path, encoding='utf-8', errors='ignore').read()
    body = t.split('{', 1)[1].rsplit('}', 1)[0]
    vals = [int(x, 16) for x in body.replace('\n', '').split(',') if x.strip()]
    return bytes(vals)

def parse_vlw(data):
    """返回 {code: (w,h,xAdv,dX,dY,bitmap_bytes)}"""
    gc, fs, ver = struct.unpack('>III', data[0:12])
    gly = {}
    off = 12
    for _ in range(gc):
        if off + 16 > len(data):
            break
        code, w, h = struct.unpack('>IHH', data[off:off + 8])
        xAdv, dX, dY = struct.unpack('>hhh', data[off + 8:off + 14])
        off += 16
        n = w * h
        bmp = data[off:off + n]
        off += n
        gly[code] = (w, h, xAdv, dX, dY, bmp)
    return gly, fs, ver

def measure(gly, codes):
    """估算子集体积"""
    total = 12
    for c in codes:
        if c in gly:
            w, h, *_ = gly[c]
            total += 16 + w * h
    return total

if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    base = r'E:\ESP\五邑大学水电站\DuduClock_2.2源码包(1)\DuduClock_2.2\font'

    # UI 实际用到的字符集
    ui_text = (
        '系统启动中正在连接网络同步天气数据即将尝试失败无效城市名称请输入密码'
        '日一二三四五六年月日时分秒'
        '空气质量指数颗粒物二氧化氮硫一氧化碳臭氧'
        '计时器单击开启停止长按3秒归零还原设置恢复出厂主题切换黑白'
        '今日体感温度能见度空气指数级'
        '雪雷沙尘雾冰雹多云雨阴晴优良中差'
        '天气预报等待重启秒后'
        'Dudu Clock'
        '0123456789'
        'ABCDEFGHIJKLMNOPQRSTUVWXYZ'
        'abcdefghijklmnopqrstuvwxyz'
        '℃%:-./+ '
    )
    codes = sorted(set(ord(c) for c in ui_text))
    print('UI 字符集: %d 个 (CJK %d)' % (
        len(codes), len([c for c in codes if c > 0x2000])))

    total_before = total_after = 0
    print()
    print('%-20s %10s %10s %6s' % ('字体', '原始', '子集化', '压缩'))
    print('-' * 52)
    for f in sorted(os.listdir(base)):
        if not f.endswith('.h'):
            continue
        d = parse_h(os.path.join(base, f))
        gly, fs, ver = parse_vlw(d)
        before = len(d)
        after = measure(gly, codes)
        hit = len([c for c in codes if c in gly])
        total_before += before
        total_after += after
        print('%-20s %8.1fK %8.1fK %5.0f%%  命中 %d/%d' % (
            f[:-2], before / 1024, after / 1024,
            after / before * 100, hit, len(codes)))

    print('-' * 52)
    print('%-20s %8.1fK %8.1fK %5.0f%%' % (
        '合计', total_before / 1024, total_after / 1024,
        total_after / total_before * 100))
