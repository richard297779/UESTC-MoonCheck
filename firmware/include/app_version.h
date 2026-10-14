#pragma once

// ============================================================================
//  固件版本与协议版本
// ============================================================================

/// @brief 固件版本号（与 docs/api-contract.md 的实施进度对应）。
///        多人版（-DWANAN_MULTIUSER=1）带 -multi 后缀，便于从 hello 回执区分。
#ifdef WANAN_MULTIUSER
#define FW_VERSION "0.2.0-multi"
#else
#define FW_VERSION "0.2.0"
#endif

/// @brief BLE 协议版本（hello 指令上报）。
#define FW_PROTO_VERSION 1

/// @brief 设备广播名前缀（后接 MAC 末两字节，例如 WANAN-A1B2）。
#define FW_DEVICE_NAME_PREFIX "WANAN"

/// @brief 固件构建时间（用于确认烧录是否成功）。
#define FW_BUILD_TIME __DATE__ " " __TIME__
