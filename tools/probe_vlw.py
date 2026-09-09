# -*- coding: utf-8 -*-
"""穷举 vlw 字形头结构，用「跑完 gc 个字形正好到文件尾」自洽性检验找出真结构"""
import struct, sys, itertools, os
sys.stdout.reconfigure(encoding='utf-8', errors='replace')

def parse_h(path):
    t = open(path, encoding='utf-8', errors='ignore').read()
    body = t.split('{', 1)[1].rsplit('}', 1)[0]
    return bytes(int(x, 16) for x in body.replace('\n', '').split(',') if x.strip())

def try_layout(d, gc, layout):
    """
    layout: [(name, fmt), ...]  fmt in ('u32','u16','i16','u8')
    返回 (ok, glyphs) — ok 表示跑完 gc 个后 offset == len(d)
    """
    g = {}
    off = 12
    for _ in range(gc):
        start = off
        vals = {}
        for name, fmt in layout:
            if fmt == 'u32':
                vals[name] = struct.unpack('>I', d[off:off+4])[0]; off += 4
            elif fmt == 'u16':
                vals[name] = struct.unpack('>H', d[off:off+2])[0]; off += 2
            elif fmt == 'i16':
                vals[name] = struct.unpack('>h', d[off:off+2])[0]; off += 2
            else:
                vals[name] = d[off]; off += 1
        head = off - start
        # w/h 必须合理
        w, h = vals.get('w', 0), vals.get('h', 0)
        if w > 200 or h > 200:
            return False, None, None
        n = w * h
        if off + n > len(d) + 4096:
            return False, None, None
        vals['bmp'] = d[off:off+n]
        off += n
        g[vals['code']] = vals
    return off == len(d), g, off

if __name__ == '__main__':
    p = r'E:\ESP\五邑大学水电站\DuduClock_2.2源码包(1)\DuduClock_2.2\font\clock_num_big_64.h'
    d = parse_h(p)
    gc, fs, ver = struct.unpack('>III', d[0:12])
    print('gc=%d fs=%d ver=%d total=%d' % (gc, fs, ver, len(d)))

    # 候选 layout: code u32 开头，后面接 w/h/xAdv/dX/dY 的各种顺序与类型
    tail_fields = ['w', 'h', 'xAdv', 'dX', 'dY']
    found = []
    for perm in itertools.permutations(tail_fields):
        if perm[0] not in ('w', 'h') or perm[1] not in ('w', 'h'):
            continue  # w,h 应紧跟 code
        for extra in (0, 2):
            layout = [('code', 'u32')]
            layout += [(f, 'u16') for f in perm[:2]]      # w,h
            layout += [(f, 'i16') for f in perm[2:]]      # 度量
            if extra:
                layout += [('pad', 'u16')]
            ok, g, off = try_layout(d, gc, layout)
            if ok:
                codes = sorted(g.keys())
                found.append((layout, codes, off))
                print('OK  layout=%s' % ([f for f, _ in layout],))
                print('    codes[:14]=%s' % [hex(c) for c in codes[:14]])
                print('    end_off=%d' % off)

    if not found:
        print('未找到自洽结构')
