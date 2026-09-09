# -*- coding: utf-8 -*-
"""
反推 DuduClock .h 字体字形头结构

已知 (来自文件头 + 文件名交叉验证):
  [0:4]   glyphCount : u32
  [4:8]   ?          : u32  = 11 (常量)
  [8:12]  fontHeight : u32  = 文件名里的字号 (24/45/64...)

策略: 枚举 code/w/h/度量的字段顺序与宽度,
      用「跑完 gc 个字形后 offset 精确等于 len(data)」做自洽检验,
      并用 fontHeight 约束 h 的合理性。
"""
import struct, sys, itertools, os
sys.stdout.reconfigure(encoding='utf-8', errors='replace')

def parse_h(path):
    t = open(path, encoding='utf-8', errors='ignore').read()
    body = t.split('{', 1)[1].rsplit('}', 1)[0]
    return bytes(int(x, 16) for x in body.replace('\n', '').split(',') if x.strip())

def run(d, gc, fh, layout):
    """layout: [(name, fmt)] ，含 'code','w','h' 及可选度量字段"""
    g = {}
    off = 12
    for _ in range(gc):
        st = off
        vals = {}
        for name, fmt in layout:
            if fmt == 'u32':
                vals[name] = struct.unpack('>I', d[off:off+4])[0]; off += 4
            elif fmt == 'u16':
                vals[name] = struct.unpack('>H', d[off:off+2])[0]; off += 2
            elif fmt == 'i16':
                vals[name] = struct.unpack('>h', d[off:off+2])[0]; off += 2
        w, h, code = vals['w'], vals['h'], vals['code']
        # 合理性: h 不超过字号高度的 2 倍，w 不超过 3 倍
        if h == 0 or h > fh * 2 + 8 or w > fh * 3 + 8 or code > 0x10FFFF:
            return False, None
        n = w * h
        if off + n > len(d):
            return False, None
        vals['bmp'] = d[off:off+n]
        off += n
        g[code] = vals
    return off == len(d), g

def probe(path):
    d = parse_h(path)
    gc, _, fh = struct.unpack('>III', d[0:12])
    name = os.path.basename(path)[:-2]

    fields = ['w', 'h']
    metrics = ['xAdv', 'dX', 'dY']
    results = []

    # code 固定在开头；w/h 用 u16 或 u32；度量可选
    for wh_fmt in ('u16', 'u32'):
        for use_metrics in (True, False):
            for perm in itertools.permutations(fields):
                for mperm in (itertools.permutations(metrics) if use_metrics else [()]):
                    for mfmt in ('i16', 'u32'):
                        layout = [('code', 'u32')]
                        layout += [(f, wh_fmt) for f in perm]
                        layout += [(m, mfmt) for m in mperm]
                        ok, g = run(d, gc, fh, layout)
                        if ok:
                            codes = sorted(g.keys())
                            mono = all(codes[i] < codes[i+1] for i in range(len(codes)-1))
                            results.append((layout, codes, mono))

    if results:
        # 优先取 code 单调递增的
        results.sort(key=lambda r: not r[2])
        for layout, codes, mono in results[:3]:
            print('  %s' % ([f for f, _ in layout],))
            print('    h范围=%d..%d  首10码位=%s  单调=%s' % (
                min(g[c]['h'] for c in codes), max(g[c]['h'] for c in codes),
                [hex(c) for c in codes[:10]], mono))
        return True
    return False

if __name__ == '__main__':
    base = r'E:\ESP\五邑大学水电站\DuduClock_2.2源码包(1)\DuduClock_2.2\font'
    for f in ['clock_num_big_64.h', 'clock_title_45.h', 'air_24.h']:
        print('=== %s ===' % f[:-2])
        if not probe(os.path.join(base, f)):
            print('  未找到自洽结构')
