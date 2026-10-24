#pragma once

// ============================================================================
//  WxSelftest：微信协议移植的黄金向量自检
//  向量由 deployment/backend-search-2026-10-24/gen_golden_vectors.py 从
//  moon-uestc Python 参考实现生成（2026-10-24 版，与 YYB-Go-Enhanced 对拍）。
// ============================================================================

#include <Arduino.h>

namespace wx {

/// @brief 运行全部对拍；返回通过数，总数写在 totalOut，失败明细写入 report。
int runSelftest(int& totalOut, String& report);

}  // namespace wx
