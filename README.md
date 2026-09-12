# 五邑大学水电站

基于 ESP32-C3 + ST7789 LCD 的宿舍桌面时钟，一键显示**天气、电费、水费**。

项目名称调侃自五邑大学宿舍的「水电」查询系统——让你不用打开手机就能看到宿舍剩余水电。

## 功能

- 🕐 时钟（NTP 自动对时，UTC+8）
- ☁️ 天气（wttr.in 免费接口，无需 key）
- ⚡ 电费查询（五邑大学校园网 API）
- 💧 水费查询（乐校通 API，含 CNN 验证码自动识别登录）
- 📱 160×320 ST7789 屏，LVGL 图形界面

> [!WARNING]
> **乐校通账号顶号机制：同一账号只能在一处登录。**
> 用水费查询会占用乐校通登录态，导致洗澡小程序被踢下线。
> **请勿使用洗澡用的手机主号登录**，建议在乐校通上用副卡注册一个小号专门用于查询。

## 硬件

- 主控：ESP32-C3（RISC-V，无 PSRAM）
- 屏幕：ST7789 240×320，SPI 接口
- 连接：USB-Serial/JTAG（自动下载，无需手动按 BOOT）

## 目录结构

```
main/
  main.c                  # 主程序：WiFi/NTP/天气/电费/水费调度
  water_api.c             # 乐校通水费 API（登录/发现/查询）
  water_api.h             # 水费接口
  user_config.h           # 配置（优先加载 user_config.local.h）
  user_config.h.example   # 配置模板
  captcha_cnn_weights.h   # 验证码 CNN int8 量化权重（19KB flash）
  display.c / ui.c        # ST7789 + LVGL
  lv_font_*.c             # 中文字体
```

## 配置

复制模板并填写真实值：

```bash
cd main
cp user_config.h.example user_config.local.h
# 编辑 user_config.local.h，填写：
#   WIFI_SSID / WIFI_PASS   WiFi 账号密码
#   WATER_PHONE / WATER_PASS  乐校通登录手机号密码
#   DORM_NUMBER  宿舍号，格式"楼栋-房间"（如 46-416）
```

> `user_config.local.h` 已被 `.gitignore` 忽略，不会上传 GitHub，保护隐私。

## 构建与烧录

需要 ESP-IDF v5.5+：

```bash
# Windows
powershell -File build.ps1 -Action build    # 编译
powershell -File build.ps1 -Action flash -Port COMx   # 烧录
powershell -File build.ps1 -Action all  -Port COMx   # 编译+烧录+监控
```

## 水费查询原理（乐校通）

1. **登录**：获取验证码图片 → CNN 识别 5 位数字 → 签名请求登录
2. **设备发现**：`getLowerAreas` 逐层定位楼栋 → 楼层 → 房间 → 水表
3. **余额查询**：`wallet/find` 返回可余额（单位：分）

登录签名：
- `X-Sign` = MD5(`1FF75E512` + 请求参数)
- `X-Ghost` = hex 时间戳的校验和 + 异或（反重放）

验证码 CNN 模型训练自 [lexiaotong-re](https://github.com/MYHealer/lexiaotong-re)，int8 量化后在 ESP32-C3 上实时推理，99%+ 准确率。

## 免责声明

- 本项目仅用于学习 ESP32 开发与 HTTP API 调用
- 电费/水费查询接口为校园网内部服务，请勿滥用
- 请勿将本项目用于商业用途
