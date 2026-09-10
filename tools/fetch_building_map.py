#!/usr/bin/env python3
"""
乐校通楼栋映射生成器（PC 版）
在 PC 上运行，遍历乐校通 API 全量楼栋树，生成 C 映射表头文件。

用法:
  python fetch_building_map.py
  python fetch_building_map.py --token "tokenInfo=xxx"

流程: 登录(手动输入验证码) → 遍历楼栋树 → 生成 building_map.h
"""

import argparse
import hashlib
import json
import time
import sys
import os
import base64
import tempfile
from urllib.request import Request, urlopen
from urllib.error import URLError, HTTPError

# ---- 配置 ----
BASE_URL = "https://v3-prod-beta-app.lxt6.cn"
SIGN_KEY = "1FF75E512"
SCHOOL_ID = "80790"
SID = "2021011300001"
VER = "4.3.9"
CTYPE = "1"

# ---- 工具函数 ----

def md5_hex(s: str) -> str:
    return hashlib.md5(s.encode()).hexdigest()

def calc_sign(params: str) -> str:
    return md5_hex(SIGN_KEY + params)

def gen_ghost() -> str:
    ms = int(time.time() * 1000)
    ts = f"{ms:012x}"
    total = xr = 0
    for i in range(0, 12, 2):
        b = int(ts[i:i+2], 16)
        total += b
        xr ^= b
    return f"{total % 256:02x}{ts}{xr:02x}"

def make_headers(token: str = "") -> dict:
    h = {
        "X-Sid": SID,
        "X-Product-Ver": VER,
        "X-clientType": CTYPE,
        "X-Ghost": gen_ghost(),
        "User-Agent": "okhttp/3.12.1",
        "Accept-Encoding": "identity",
    }
    if token:
        h["Cookie"] = token
    return h

def api_get(path: str, token: str = "") -> dict:
    sign_input = path.split("?")[1] if "?" in path else ""
    sign = calc_sign(sign_input)
    url = BASE_URL + path
    req = Request(url, headers={**make_headers(token), "X-Sign": sign})
    try:
        with urlopen(req, timeout=30) as resp:
            body = resp.read().decode("utf-8")
            set_cookie = resp.headers.get("Set-Cookie", "")
            new_token = token
            if "tokenInfo=" in set_cookie:
                for part in set_cookie.split(";"):
                    if "tokenInfo=" in part:
                        new_token = part.strip()
                        break
            return {"json": json.loads(body), "token": new_token}
    except (URLError, HTTPError) as e:
        print(f"  GET {path} 失败: {e}", file=sys.stderr)
        return {"json": None, "token": token}

def api_post(path: str, body: str, token: str = "") -> dict:
    """POST 请求，X-Sign 对 body 签名"""
    sign = calc_sign(body)
    url = BASE_URL + path
    data = body.encode("utf-8")
    req = Request(url, data=data, headers={
        **make_headers(token),
        "X-Sign": sign,
        "Content-Type": "application/json",
    })
    try:
        with urlopen(req, timeout=30) as resp:
            resp_body = resp.read().decode("utf-8")
            set_cookie = resp.headers.get("Set-Cookie", "")
            new_token = token
            if "tokenInfo=" in set_cookie:
                for part in set_cookie.split(";"):
                    if "tokenInfo=" in part:
                        new_token = part.strip()
                        break
            return {"json": json.loads(resp_body), "token": new_token}
    except (URLError, HTTPError) as e:
        print(f"  POST {path} 失败: {e}", file=sys.stderr)
        return {"json": None, "token": token}

# ---- 登录（自动验证码） ----

def login_auto(phone: str, password: str, weights_path: str) -> str:
    """自动验证码识别登录"""
    from captcha_model import CaptchaCNN, load_quantized_weights, predict_captcha_full

    print(f"\n加载验证码模型...")
    model = CaptchaCNN()
    model = load_quantized_weights(model, weights_path)
    print(f"模型参数量: {sum(p.numel() for p in model.parameters())}")

    print(f"登录乐校通 (手机号: {phone})...")
    for attempt in range(10):
        # 获取验证码
        result = api_get(f"/user/authentication/getCode?account={phone}")
        data = result["json"]
        if not data or data.get("Code") != 0:
            print(f"  获取验证码失败: {data}")
            continue

        b64_data = data.get("Data", "")
        if "," in b64_data:
            b64_data = b64_data.split(",", 1)[1]

        img_bytes = base64.b64decode(b64_data)

        # CNN 识别
        captcha = predict_captcha_full(model, img_bytes)
        if len(captcha) != 5:
            print(f"  验证码识别失败 (got '{captcha}'), attempt {attempt+1}")
            continue

        print(f"  验证码识别: {captcha} (attempt {attempt+1})")

        # 构造登录请求（与 ESP32 格式完全一致：无空格）
        inner = '{"studentMobile":"%s","loginPassword":"%s","code":"%s"}' % (phone, password, captcha)
        inner_b64 = base64.b64encode(inner.encode()).decode()
        outer = '{"data":"%s"}' % inner_b64

        login_result = api_post("/user/login/userLoginV2WithEncrypt", outer)
        login_data = login_result["json"]
        if login_data and login_data.get("Code") == 0:
            token = login_result["token"]
            print(f"  登录成功! Token: {token[:60]}...")
            return token
        else:
            print(f"  登录失败: {login_data}")

    print("登录失败，已重试 10 次")
    return ""

# ---- 楼栋树遍历 ----

def traverse_tree(token: str, area_id: str, depth: int = 0,
                  building_map: dict = None, parent_name: str = "") -> dict:
    """递归遍历区域树，收集所有楼栋节点。
    楼栋 = 子节点是楼层（名称含"层"）的节点。
    优先保留冷水区域的楼栋（北区冷水/南区冷水），跳过热水。
    """
    if building_map is None:
        building_map = {}
    if depth > 5:
        return building_map

    indent = "  " * depth
    print(f"{indent}查询区域 {area_id} (depth={depth}, parent='{parent_name}')...")

    result = api_get(f"/baseDict/site/getLowerAreas?areaId={area_id}", token)
    data = result["json"]
    if not data or data.get("Code") != 0:
        code = data.get("Code") if data else "None"
        print(f"{indent}  查询失败 (Code={code})")
        return building_map

    items = data.get("Data", [])
    if not items:
        print(f"{indent}  无子区域")
        return building_map

    for item in items:
        node_id = item.get("id", "")
        node_name = item.get("name", "")
        children = item.get("childList", [])

        if not children:
            # 叶子节点 = 最底层（房间/设备），跳过
            continue

        # 检查子节点是否是楼层（名称含"层"）
        first_child_name = children[0].get("name", "") if children else ""
        is_building = "层" in first_child_name

        if is_building:
            # 这是一个楼栋：子节点是楼层
            # 判断是冷水还是热水：检查当前节点或父节点名称
            is_cold = "冷水" in parent_name or "冷水" in node_name
            is_hot = "热水" in parent_name or "热水" in node_name

            if is_hot and not is_cold:
                # 热水区域：如果已有冷水同名条目则跳过
                if node_name in building_map:
                    print(f"{indent}  跳过热水楼栋: {node_name} (已有冷水映射)")
                    continue
                print(f"{indent}  热水楼栋(无冷水): {node_name} (id={node_id}, {len(children)}层)")
            else:
                print(f"{indent}  楼栋: {node_name} (id={node_id}, {len(children)}层, {'冷水' if is_cold else '未知'})")

            building_map[node_name] = {"id": node_id, "parent_id": area_id}
        else:
            # 中间区域，递归
            print(f"{indent}  区域: {node_name} (id={node_id}, {len(children)}个子节点)")
            traverse_tree(token, node_id, depth + 1, building_map, node_name)

    return building_map

# ---- 输出 C 头文件 ----

def write_c_header(building_map: dict, output_path: str):
    sorted_buildings = sorted(building_map.items(), key=lambda x: x[0])

    with open(output_path, "w", encoding="utf-8") as f:
        f.write("/*\n")
        f.write(" * building_map.h - 乐校通楼栋映射表\n")
        f.write(" * 自动生成，请勿手动编辑\n")
        f.write(f" * 生成时间: {time.strftime('%Y-%m-%d %H:%M:%S')}\n")
        f.write(f" * 楼栋数量: {len(sorted_buildings)}\n")
        f.write(" */\n\n")
        f.write("#pragma once\n\n")
        f.write("#include <stdint.h>\n\n")
        f.write("typedef struct {\n")
        f.write("    const char *name;     /* 楼栋名 */\n")
        f.write("    const char *area_id;  /* 区域 ID */\n")
        f.write("} building_entry_t;\n\n")
        f.write(f"#define BUILDING_MAP_SIZE {len(sorted_buildings)}\n\n")
        f.write("static const building_entry_t BUILDING_MAP[] = {\n")
        for name, info in sorted_buildings:
            name_esc = name.replace("\\", "\\\\").replace('"', '\\"')
            f.write(f'    {{"{name_esc}", "{info["id"]}"}},\n')
        f.write("};\n\n")
        f.write("/* 按楼栋名查找 area_id，精确数字前缀匹配（name=\"46\" 匹配 \"46栋\"，不匹配 \"146栋\"） */\n")
        f.write("static inline const char *find_building_area_id(const char *name)\n")
        f.write("{\n")
        f.write("    int nlen = strlen(name);\n")
        f.write("    for (int i = 0; i < BUILDING_MAP_SIZE; i++) {\n")
        f.write("        const char *bname = BUILDING_MAP[i].name;\n")
        f.write("        /* 精确前缀匹配：bname 以 name 开头，且下一个字符非数字 */\n")
        f.write("        if (strncmp(bname, name, nlen) == 0 &&\n")
        f.write("            (bname[nlen] == '\\0' || !((unsigned char)bname[nlen] >= '0' && (unsigned char)bname[nlen] <= '9')))\n")
        f.write("            return BUILDING_MAP[i].area_id;\n")
        f.write("    }\n")
        f.write("    return NULL;\n")
        f.write("}\n")

    print(f"\n已生成: {output_path}")
    print(f"楼栋数量: {len(sorted_buildings)}")

# ---- 主函数 ----

def main():
    parser = argparse.ArgumentParser(description="乐校通楼栋映射生成器")
    parser.add_argument("--token", help="TokenInfo cookie (跳过登录)")
    parser.add_argument("--phone", default="19237540807", help="手机号")
    parser.add_argument("--password", default="hcj20030807", help="密码")
    parser.add_argument("--school-id", default=SCHOOL_ID, help="学校 ID")
    parser.add_argument("--output", default="building_map.h", help="输出文件")
    args = parser.parse_args()

    # 获取 token
    token = ""
    if args.token:
        token = args.token
        if not token.startswith("tokenInfo="):
            token = f"tokenInfo={token}"
        # 验证 token
        print("验证 token...")
        result = api_get(f"/baseDict/site/getLowerAreas?areaId={args.school_id}", token)
        if not result["json"] or result["json"].get("Code") != 0:
            print("Token 无效或过期，需要重新登录")
            token = ""
        else:
            print("Token 有效")

    if not token:
        weights_path = os.path.join(os.path.dirname(__file__), "..", "main", "captcha_cnn_weights.h")
        token = login_auto(args.phone, args.password, weights_path)
        if not token:
            sys.exit(1)

    # 遍历楼栋树
    print(f"\n开始遍历楼栋树 (学校ID={args.school_id})...")
    building_map = traverse_tree(token, args.school_id)

    if not building_map:
        print("未找到任何楼栋！")
        sys.exit(1)

    # 输出
    write_c_header(building_map, args.output)

    json_path = args.output.replace(".h", ".json")
    with open(json_path, "w", encoding="utf-8") as f:
        json.dump(building_map, f, ensure_ascii=False, indent=2)
    print(f"JSON 映射: {json_path}")

if __name__ == "__main__":
    main()
