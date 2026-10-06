// ============================================================
//  配置区 —— 只需要改这个文件
// ============================================================
#pragma once

// ---------- 上游：校园网（ESP32 用它上网的那张网）----------
#define UPSTREAM_SSID   "NUN"      // 校园网 SSID（2.4G，必须是 1~13 信道那种）
#define UPSTREAM_PASS   ""              // 开放式网络留空；有密码就填

// ---------- 本机热点：你的设备连这个 ----------
#define AP_SSID         "ESP32-2.4G"    // 尽量用纯英文数字，别用中文
#define AP_PASS         "esp32relay"    // 至少 8 位；留空 "" 表示开放热点

// ---------- 网页认证（如果 MAC 白名单已经能直接上网，就别开）----------
// 0 = 关闭；1 = 打开（会走下面这套账号密码登录）
#define ENABLE_PORTAL_LOGIN   0
#define PORTAL_BASE     "http://bd.nmu.edu.cn"
#define PORTAL_USER     ""              
#define PORTAL_PASS     ""              
#define PORTAL_NASID    "2"
#define PORTAL_ISP      "local"

// ---------- 其它（一般不用动）----------
#define STA_HOSTNAME    "esp32-relay"
#define AP_MAX_CONN     4               // 热点最多允许几台设备（最大 10）
#define CHECK_INTERVAL_MS  30000UL      // 多久检查一次外网
#define USE_DNS_PROXY   1               // 自带 DNS 转发，1=开（建议保持开）
