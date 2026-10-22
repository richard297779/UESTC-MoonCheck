#pragma once

// ============================================================================
//  固件版本与协议版本
// ============================================================================

/// @brief 固件版本号（与 docs/api-contract.md 的实施进度对应）。
///        单人/多人不再是两种构建：账号表里有几条就打几个，因此不再加 -multi 后缀。
#define FW_VERSION "0.4.0"

/// @brief BLE 协议版本（hello 指令上报）。
#define FW_PROTO_VERSION 1

/// @brief 设备广播名前缀（后接 MAC 末两字节，例如 WANAN-A1B2）。
#define FW_DEVICE_NAME_PREFIX "WANAN"

/// @brief 固件构建时间（用于确认烧录是否成功）。
#define FW_BUILD_TIME __DATE__ " " __TIME__
