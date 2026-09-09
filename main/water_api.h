/*
 * water_api.h - 乐校通水费查询接口
 *
 * 完整流程：自动登录（CNN验证码识别）→ 设备发现 → 余额查询
 */
#pragma once

#include <stdbool.h>

/* 登录 + 设备发现 + 查询，返回水费余额（元），失败返回负数 */
float water_query_all(void);

/* 仅查询（token + machineId 已缓存），返回余额（元） */
float water_query_balance(void);
