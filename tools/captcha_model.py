#!/usr/bin/env python3
"""
从 ESP32 量化权重重建 PyTorch CNN + 完整验证码识别流水线。
与 ESP32 版本完全一致：去蓝线 → 灰度 → 二值化 → 列投影切字符 → 逐字 CNN 推理。
"""
import re
import os
import numpy as np
import torch
import torch.nn as nn
from PIL import Image
import io

# ---- 量化权重解析 ----

def parse_weights_header(path: str) -> dict:
    with open(path, "r") as f:
        content = f.read()
    # 统一换行符
    content = content.replace("\r\n", "\n").replace("\r", "\n")
    weights = {}
    pattern = r'// ([\w.]+): shape=\[.*?\], scale=([\d.]+)\nconst float [\w_]+_scale = [\d.]+f;\nconst int\d+_t [\w_]+\[\] = \{([^}]+)\}'
    for m in re.finditer(pattern, content, re.DOTALL):
        name = m.group(1)
        scale = float(m.group(2))
        values = [int(x.strip()) for x in m.group(3).split(",") if x.strip().lstrip("-").isdigit()]
        weights[name] = (scale, np.array(values, dtype=np.int8))
    return weights

def dequantize(int8_vals: np.ndarray, scale: float) -> torch.Tensor:
    return torch.tensor(int8_vals.astype(np.float32) * scale)

# ---- CNN 模型 ----

class CaptchaCNN(nn.Module):
    """单数字识别: 输入 1x12x8, 输出 10 类 (0-9)"""
    def __init__(self):
        super().__init__()
        self.features = nn.Sequential(
            nn.Conv2d(1, 16, 3, padding=1),   # 0
            nn.BatchNorm2d(16),                 # 1
            nn.ReLU(),                          # 2
            nn.MaxPool2d(2),                    # 3
            nn.Conv2d(16, 32, 3, padding=1),   # 4
            nn.BatchNorm2d(32),                 # 5
            nn.ReLU(),                          # 6
            nn.MaxPool2d(2),                    # 7
        )
        self.classifier = nn.Sequential(
            nn.Flatten(),                       # 0
            nn.Linear(32 * 3 * 2, 64),          # 1
            nn.ReLU(),                          # 2
            nn.Linear(64, 32),                  # 3
            nn.ReLU(),                          # 4
            nn.Linear(32, 10),                  # 5
        )

    def forward(self, x):
        return self.classifier(self.features(x))

def load_quantized_weights(model: CaptchaCNN, weights_path: str):
    w = parse_weights_header(weights_path)
    def dq(name):
        scale, vals = w[name]
        return torch.tensor(vals.astype(np.float32) * scale)
    model.features[0].weight.data = dq("features.0.weight").reshape(16, 1, 3, 3)
    model.features[0].bias.data = dq("features.0.bias")
    model.features[1].weight.data = dq("features.1.weight")
    model.features[1].bias.data = dq("features.1.bias")
    model.features[1].running_mean = dq("features.1.running_mean")
    model.features[1].running_var = dq("features.1.running_var")
    model.features[4].weight.data = dq("features.4.weight").reshape(32, 16, 3, 3)
    model.features[4].bias.data = dq("features.4.bias")
    model.features[5].weight.data = dq("features.5.weight")
    model.features[5].bias.data = dq("features.5.bias")
    model.features[5].running_mean = dq("features.5.running_mean")
    model.features[5].running_var = dq("features.5.running_var")
    model.classifier[1].weight.data = dq("classifier.1.weight").reshape(64, 192)
    model.classifier[1].bias.data = dq("classifier.1.bias")
    model.classifier[3].weight.data = dq("classifier.4.weight").reshape(32, 64)
    model.classifier[3].bias.data = dq("classifier.4.bias")
    model.classifier[5].weight.data = dq("classifier.6.weight").reshape(10, 32)
    model.classifier[5].bias.data = dq("classifier.6.bias")
    return model

# ---- 验证码识别（与 ESP32 完全一致） ----

def predict_captcha_full(model: CaptchaCNN, img_bytes: bytes) -> str:
    """完整验证码识别流水线，返回 5 位数字字符串"""
    img = Image.open(io.BytesIO(img_bytes)).convert("RGB")
    pixels = np.array(img)
    h, w = pixels.shape[:2]

    # 1. 去蓝色干扰线
    r, g, b = pixels[:,:,0].astype(int), pixels[:,:,1].astype(int), pixels[:,:,2].astype(int)
    blue_mask = (b > r + 50) & (b > g + 50)
    pixels[blue_mask] = [255, 255, 255]

    # 2. 灰度化 (与 ESP32 一致的加权公式)
    gray = (pixels[:,:,0] * 0.299 + pixels[:,:,1] * 0.587 + pixels[:,:,2] * 0.114).astype(np.uint8)

    # 3. 二值化
    binary = (gray < 128).astype(np.uint8)  # 黑色=1, 白色=0

    # 4. 列投影找字符区域
    col_sum = binary.sum(axis=0)  # shape: (w,)
    regions = []
    in_region = False
    start = 0
    for x in range(w):
        if col_sum[x] >= 2:
            if not in_region:
                start = x
                in_region = True
        else:
            if in_region:
                if regions and start - regions[-1][1] < 3:
                    regions[-1] = (regions[-1][0], x - 1)  # 合并
                else:
                    regions.append((start, x - 1))
                in_region = False
    if in_region:
        if regions and start - regions[-1][1] < 3:
            regions[-1] = (regions[-1][0], w - 1)
        else:
            regions.append((start, w - 1))

    if len(regions) != 5:
        return ""  # 识别失败

    # 5. 逐字识别
    result = []
    model.eval()
    with torch.no_grad():
        for rx0, rx1 in regions:
            rw = rx1 - rx0 + 1
            # 找字符垂直范围
            char_cols = binary[:, rx0:rx1+1]
            rows_with_ink = np.where(char_cols.sum(axis=1) > 0)[0]
            if len(rows_with_ink) == 0:
                ry0, ry1 = 0, h - 1
            else:
                ry0, ry1 = rows_with_ink[0], rows_with_ink[-1]
            # 加 1px margin
            ry0 = max(0, ry0 - 1)
            ry1 = min(h - 1, ry1 + 1)
            rh = ry1 - ry0 + 1

            # 缩放到 12x8 (最近邻), 取反 (背景=1, 笔画=0)
            char_img = np.zeros((12, 8), dtype=np.float32)
            for dy in range(12):
                sy = ry0 + dy * rh // 12
                for dx in range(8):
                    sx = rx0 + dx * rw // 8
                    char_img[dy, dx] = 1.0 - binary[sy, sx]  # 取反

            # CNN 推理
            x = torch.tensor(char_img).unsqueeze(0).unsqueeze(0)  # 1x1x12x8
            output = model(x)
            digit = output.argmax(dim=1).item()
            result.append(str(digit))

    return "".join(result)

# 测试
if __name__ == "__main__":
    weights_path = os.path.join(os.path.dirname(__file__), "..", "main", "captcha_cnn_weights.h")
    model = CaptchaCNN()
    model = load_quantized_weights(model, weights_path)
    print(f"模型加载成功, 参数量: {sum(p.numel() for p in model.parameters())}")
