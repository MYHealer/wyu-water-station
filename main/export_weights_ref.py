"""导出C权重"""
import os, torch, torch.nn as nn

class CaptchaCNN(nn.Module):
    def __init__(self):
        super().__init__()
        self.features = nn.Sequential(
            nn.Conv2d(1, 16, 3, padding=1), nn.BatchNorm2d(16), nn.ReLU(), nn.MaxPool2d(2),
            nn.Conv2d(16, 32, 3, padding=1), nn.BatchNorm2d(32), nn.ReLU(), nn.MaxPool2d(2),
        )
        self.classifier = nn.Sequential(
            nn.Flatten(), nn.Linear(32 * 3 * 2, 64), nn.ReLU(), nn.Dropout(0.3),
            nn.Linear(64, 32), nn.ReLU(), nn.Linear(32, 10),
        )
    def forward(self, x):
        return self.classifier(self.features(x))

model = CaptchaCNN()
model.load_state_dict(torch.load("captcha_cnn_best.pth"))
state = model.state_dict()

def quantize(tensor):
    scale = tensor.abs().max() / 127
    q = torch.clamp(torch.round(tensor / scale), -128, 127).to(torch.int8)
    return q.numpy(), scale.item()

lines = [
    "// Auto-generated captcha CNN weights for ESP32 (v4 - with blue line removal)",
    "// Input: 12x8 binary image (B>R+50 && B>G+50 removed first)",
    "",
    "#ifndef CAPTCHA_CNN_WEIGHTS_H",
    "#define CAPTCHA_CNN_WEIGHTS_H",
    "",
    "#include <stdint.h>",
    "#include <math.h>",
    "",
]

for name, param in state.items():
    q, s = quantize(param)
    safe_name = name.replace(".", "_")
    flat = q.flatten()
    lines.append(f"// {name}: shape={list(q.shape)}, scale={s:.6f}")
    lines.append(f"const float {safe_name}_scale = {s:.6f}f;")
    lines.append(f"const int8_t {safe_name}[] = {{")
    for i in range(0, len(flat), 16):
        chunk = flat[i:i+16]
        lines.append("  " + ", ".join(f"{int(v):4d}" for v in chunk) + ",")
    lines.append("};")
    lines.append(f"const int {safe_name}_len = {len(flat)};")
    lines.append("")

# 推理函数
infer = r"""// CNN inference for ESP32-C3
// IMPORTANT: Remove blue line (B>R+50 && B>G+50) before converting to grayscale!
static int captcha_predict(const uint8_t *img) {
    float conv1_out[16][12][8];
    for (int f = 0; f < 16; f++)
        for (int y = 0; y < 12; y++)
            for (int x = 0; x < 8; x++) {
                float sum = features_0_bias[f] * features_0_bias_scale;
                for (int ky = -1; ky <= 1; ky++)
                    for (int kx = -1; kx <= 1; kx++) {
                        int iy = y+ky, ix = x+kx;
                        if (iy>=0 && iy<12 && ix>=0 && ix<8) {
                            int wi = f*9 + (ky+1)*3 + (kx+1);
                            sum += (float)features_0_weight[wi] * features_0_weight_scale * img[iy*8+ix];
                        }
                    }
                float bn_w = features_1_weight[f] * features_1_weight_scale;
                float bn_b = features_1_bias[f] * features_1_bias_scale;
                float bn_rm = features_1_running_mean[f] * features_1_running_mean_scale;
                float bn_rv = features_1_running_var[f] * features_1_running_var_scale;
                sum = (sum - bn_rm) * bn_w / sqrtf(bn_rv + 1e-5f) + bn_b;
                conv1_out[f][y][x] = sum > 0 ? sum : 0;
            }
    float pool1[16][6][4];
    for (int f = 0; f < 16; f++)
        for (int y = 0; y < 6; y++)
            for (int x = 0; x < 4; x++) {
                float m = conv1_out[f][y*2][x*2];
                if (conv1_out[f][y*2][x*2+1] > m) m = conv1_out[f][y*2][x*2+1];
                if (conv1_out[f][y*2+1][x*2] > m) m = conv1_out[f][y*2+1][x*2];
                if (conv1_out[f][y*2+1][x*2+1] > m) m = conv1_out[f][y*2+1][x*2+1];
                pool1[f][y][x] = m;
            }
    float conv2_out[32][6][4];
    for (int f = 0; f < 32; f++)
        for (int y = 0; y < 6; y++)
            for (int x = 0; x < 4; x++) {
                float sum = features_4_bias[f] * features_4_bias_scale;
                for (int c = 0; c < 16; c++)
                    for (int ky = -1; ky <= 1; ky++)
                        for (int kx = -1; kx <= 1; kx++) {
                            int iy = y+ky, ix = x+kx;
                            if (iy>=0 && iy<6 && ix>=0 && ix<4) {
                                int wi = f*144 + c*9 + (ky+1)*3 + (kx+1);
                                sum += (float)features_4_weight[wi] * features_4_weight_scale * pool1[c][iy][ix];
                            }
                        }
                float bn_w = features_5_weight[f] * features_5_weight_scale;
                float bn_b = features_5_bias[f] * features_5_bias_scale;
                float bn_rm = features_5_running_mean[f] * features_5_running_mean_scale;
                float bn_rv = features_5_running_var[f] * features_5_running_var_scale;
                sum = (sum - bn_rm) * bn_w / sqrtf(bn_rv + 1e-5f) + bn_b;
                conv2_out[f][y][x] = sum > 0 ? sum : 0;
            }
    float pool2[32][3][2];
    for (int f = 0; f < 32; f++)
        for (int y = 0; y < 3; y++)
            for (int x = 0; x < 2; x++) {
                float m = conv2_out[f][y*2][x*2];
                if (conv2_out[f][y*2][x*2+1] > m) m = conv2_out[f][y*2][x*2+1];
                if (conv2_out[f][y*2+1][x*2] > m) m = conv2_out[f][y*2+1][x*2];
                if (conv2_out[f][y*2+1][x*2+1] > m) m = conv2_out[f][y*2+1][x*2+1];
                pool2[f][y][x] = m;
            }
    float fc1[64];
    for (int i = 0; i < 64; i++) {
        float sum = classifier_1_bias[i] * classifier_1_bias_scale;
        for (int j = 0; j < 192; j++)
            sum += (float)classifier_1_weight[i*192+j] * classifier_1_weight_scale * pool2[j/6][(j/2)%3][j%2];
        fc1[i] = sum > 0 ? sum : 0;
    }
    float fc2[32];
    for (int i = 0; i < 32; i++) {
        float sum = classifier_4_bias[i] * classifier_4_bias_scale;
        for (int j = 0; j < 64; j++)
            sum += (float)classifier_4_weight[i*64+j] * classifier_4_weight_scale * fc1[j];
        fc2[i] = sum > 0 ? sum : 0;
    }
    float logits[10];
    int best = 0;
    for (int i = 0; i < 10; i++) {
        float sum = classifier_6_bias[i] * classifier_6_bias_scale;
        for (int j = 0; j < 32; j++)
            sum += (float)classifier_6_weight[i*32+j] * classifier_6_weight_scale * fc2[j];
        logits[i] = sum;
        if (sum > logits[best]) best = i;
    }
    return best;
}
#endif
"""
lines.append(infer)

with open("captcha_cnn_weights.h", "w") as f:
    f.write("\n".join(lines))
size = os.path.getsize("captcha_cnn_weights.h")
print(f"导出完成: captcha_cnn_weights.h ({size/1024:.1f} KB)")
