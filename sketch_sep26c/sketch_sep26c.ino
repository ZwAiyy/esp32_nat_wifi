//  上游: 校园网开放 SSID (2.4G)
//  下游: ESP32 自建热点 AP_SSID，连上它的设备共享 ESP32 的出口
//
//  工作流程:
//   1. STA 连上校园网，拿到 IP（若 MAC 已在自服务系统加白，直接就有外网）
//   2. 用和上游一样的信道开启 softAP（单射频必须同信道）
//   3. 把 DHCP 下发的 DNS 改成校园网 DNS，并开一个 DNS 转发兜底
//   4. esp_netif_napt_enable() 打开 NAT，下游设备即可上网
//
//  依赖: Arduino-ESP32 core 3.x (本机 3.2.1-cn / IDF v5.4)
//        该核的 sdkconfig 已含 CONFIG_LWIP_IP_FORWARD=y 与 IPV4_NAPT=y
// ============================================================
#include <WiFi.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>
#include "esp_netif.h"
#include "secrets.h"

// ---------------- 常量 ----------------
static const uint32_t STA_WAIT_MS       = 20000;   // 等上游拿 IP 的最长时间
static const uint32_t PROBE_TIMEOUT_MS  = 5000;    // 探测外网超时
static const uint32_t DNS_TIMEOUT_MS    = 800;     // DNS 转发等待上游应答
static const uint8_t  NOPORTAL_FAIL_LIMIT = 2;     // 连续几次不通才尝试重登录

// 用来判断"真的有外网"的探测地址：正常返回 204，被认证页劫持一般返回 200/302
static const char* PROBE_URLS[] = {
  "http://connect.rom.miui.com/generate_204",
  "http://connectivitycheck.platform.hicloud.com/generate_204",
};
static const uint8_t PROBE_URL_COUNT = sizeof(PROBE_URLS) / sizeof(PROBE_URLS[0]);

// ---------------- 全局 ----------------
static esp_netif_t* s_staNetif = nullptr;
static esp_netif_t* s_apNetif  = nullptr;
static IPAddress    s_upDns(223, 5, 5, 5);   // 上游 DNS，后面从 STA 自动学习
static WiFiUDP      s_dnsIn;                 // 监听 AP 侧 53 端口
static WiFiUDP      s_dnsOut;                // 向上游发查询
static uint8_t      s_buf[512];
static bool         s_dnsProxyReady = false;
static uint32_t     s_lastCheck = 0;
static uint8_t      s_failCount = 0;

// ================= 基础工具 =================
static const char* wlStatusStr(wl_status_t s) {
  switch (s) {
    case WL_IDLE_STATUS:     return "IDLE";
    case WL_NO_SSID_AVAIL:   return "找不到该SSID(信号太弱/只有5G?)";
    case WL_SCAN_COMPLETED:  return "SCAN_DONE";
    case WL_CONNECTED:       return "已连接";
    case WL_CONNECT_FAILED:  return "密码错误/被拒绝";
    case WL_CONNECTION_LOST: return "连接丢失";
    case WL_DISCONNECTED:    return "未连接";
    default:                 return "未知";
  }
}

// 等上游拿到 IP；成功返回 true
static bool waitSta(uint32_t timeoutMs) {
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) {
    delay(200);
    Serial.print('.');
  }
  Serial.println();
  return WiFi.status() == WL_CONNECTED;
}

// 探测是否真的能上外网（204 = 通；被认证页拦下会得到 200/302）
static bool internetOK() {
  HTTPClient http;
  http.setConnectTimeout(4000);
  http.setTimeout(PROBE_TIMEOUT_MS);
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  for (uint8_t i = 0; i < PROBE_URL_COUNT; i++) {
    if (!http.begin(PROBE_URLS[i])) { http.end(); continue; }
    int code = http.GET();
    http.end();
    if (code == 204) return true;
  }
  return false;
}

// ================= 网页认证（选做）=================
#if ENABLE_PORTAL_LOGIN
static bool portalLogin() {
  String referer = String(PORTAL_BASE) + "/tpl/default/login_account.html?ip=" +
                   WiFi.localIP().toString() + "&nasId=" + PORTAL_NASID;
  String body = String("username=") + PORTAL_USER + "&password=" + PORTAL_PASS +
                "&nasId=" + PORTAL_NASID + "&isp=" + PORTAL_ISP + "&timeLimit=";

  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(10000);
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);

  String url = String(PORTAL_BASE) + "/api/account/login";
  if (!http.begin(url)) { Serial.println("[portal] begin 失败"); return false; }

  http.addHeader("Accept", "*/*");
  http.addHeader("X-Requested-With", "XMLHttpRequest");
  http.addHeader("Content-Type", "application/x-www-form-urlencoded; charset=UTF-8");
  http.addHeader("Referer", referer);

  Serial.printf("[portal] POST %s  ip=%s\n", url.c_str(), WiFi.localIP().toString().c_str());
  int code = http.POST(body);
  String resp = http.getString();
  http.end();

  Serial.printf("[portal] HTTP %d\n", code);
  Serial.printf("[portal] 返回: %s\n", resp.c_str());
  // 各校 portal 返回格式不同，统一以"能不能上外网"作为最终判据
  return internetOK();
}
#endif

// ================= 开热点 + NAT =================
static void startAp() {
  int ch = WiFi.channel();
  if (ch < 1 || ch > 13) ch = 6;          // 单射频：必须跟上游同信道

  // 防止下游网段和上游撞车（校园网常用 192.168.4.x / 10.x）
  IPAddress ip(192, 168, 4, 1), gw(192, 168, 4, 1), mask(255, 255, 255, 0);
  IPAddress up = WiFi.localIP();
  if (up[0] == 192 && up[1] == 168 && up[2] == 4) {
    ip = IPAddress(10, 99, 0, 1);
    gw = ip;
    Serial.println("[ap] 检测到与上游网段冲突，热点改用 10.99.0.0/24");
  }
  WiFi.softAPConfig(ip, gw, mask);

  const char* pass = (strlen(AP_PASS) >= 8) ? AP_PASS : nullptr;
  bool ok = WiFi.softAP(AP_SSID, pass, ch, false, AP_MAX_CONN);
  Serial.printf("[ap] %s  SSID=%s  信道=%d  本机=%s\n",
                ok ? "已开启" : "开启失败", AP_SSID, ch, WiFi.softAPIP().toString().c_str());
}

// 把 DHCP 下发的 DNS 改成校园网 DNS（默认下发的是热点自己，那样解析不了域名）
static void applyApDns() {
  esp_netif_dns_info_t info = {};
  if (s_staNetif && esp_netif_get_dns_info(s_staNetif, ESP_NETIF_DNS_MAIN, &info) == ESP_OK) {
    uint32_t a = info.ip.u_addr.ip4.addr;
    if (a) {
      uint8_t* b = (uint8_t*)&a;            // ESP32 小端：b[0] 就是第一段
      s_upDns = IPAddress(b[0], b[1], b[2], b[3]);
    }
  }
  esp_ip4_addr_t dns = {};
  dns.addr = (uint32_t)s_upDns;

  esp_err_t e = esp_netif_dhcps_option(s_apNetif, ESP_NETIF_OP_SET,
                                       ESP_NETIF_DOMAIN_NAME_SERVER, &dns, sizeof(dns));
  if (e != ESP_OK) {                        // 有的 IDF 版本要求 DHCP 服务停下来才能改
    esp_netif_dhcps_stop(s_apNetif);
    e = esp_netif_dhcps_option(s_apNetif, ESP_NETIF_OP_SET,
                               ESP_NETIF_DOMAIN_NAME_SERVER, &dns, sizeof(dns));
    esp_netif_dhcps_start(s_apNetif);
  }
  Serial.printf("[dns] DHCP 下发 DNS = %s  (%s)\n",
                s_upDns.toString().c_str(), esp_err_to_name(e));
}

static void enableNapt() {
  esp_err_t e = esp_netif_napt_enable(s_apNetif);
  Serial.printf("[nat] NAPT 启用: %s\n", esp_err_to_name(e));
  if (e != ESP_OK) Serial.println("[nat] 失败! 下游设备只能访问 ESP32 本身");
}

// ================= DNS 转发兜底 =================
#if USE_DNS_PROXY
static void dnsProxyStart() {
  if (s_dnsIn.begin(53)) {                  // 监听所有接口的 53 端口
    s_dnsProxyReady = true;
    Serial.println("[dns] 转发服务已监听 53");
  } else {
    Serial.println("[dns] 53 端口监听失败，转发不可用");
  }
}

// 极简串行转发：一次处理一个查询，低频率的物联网设备完全够用
static void dnsProxyPoll() {
  if (!s_dnsProxyReady) return;
  int len = s_dnsIn.parsePacket();
  if (len <= 0) return;
  if (len > (int)sizeof(s_buf)) { s_dnsIn.flush(); return; }

  IPAddress clientIp = s_dnsIn.remoteIP();
  uint16_t  clientPort = s_dnsIn.remotePort();
  int n = s_dnsIn.read(s_buf, sizeof(s_buf));
  if (n <= 0) return;

  s_dnsOut.beginPacket(s_upDns, 53);
  s_dnsOut.write(s_buf, n);
  s_dnsOut.endPacket();

  uint32_t t0 = millis();
  while (millis() - t0 < DNS_TIMEOUT_MS) {
    int rl = s_dnsOut.parsePacket();
    if (rl > 0) {
      int rn = s_dnsOut.read(s_buf, sizeof(s_buf));
      if (rn > 0) {
        s_dnsIn.beginPacket(clientIp, clientPort);
        s_dnsIn.write(s_buf, rn);
        s_dnsIn.endPacket();
      }
      return;
    }
    delay(1);
  }
}
#endif

// ================= 掉线维护 =================
static void maintain() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[net] 上游掉线，重连中...");
    WiFi.disconnect();
    WiFi.begin(UPSTREAM_SSID, UPSTREAM_PASS);
    if (waitSta(STA_WAIT_MS)) {
      Serial.printf("[net] 重新连上: %s  信道 %d\n",
                    WiFi.localIP().toString().c_str(), WiFi.channel());
      applyApDns();                          // DNS 可能变了，重新下发
      s_failCount = 0;
    } else {
      Serial.printf("[net] 重连失败: %s\n", wlStatusStr(WiFi.status()));
    }
    return;
  }

  if (internetOK()) {
    if (s_failCount) Serial.println("[net] 外网恢复");
    s_failCount = 0;
    return;
  }

  s_failCount++;
  Serial.printf("[net] 探不到外网 (%u/%u)\n", s_failCount, NOPORTAL_FAIL_LIMIT);
  if (s_failCount >= NOPORTAL_FAIL_LIMIT) {
    s_failCount = 0;
#if ENABLE_PORTAL_LOGIN
    Serial.println("[portal] 尝试重新登录...");
    if (portalLogin()) Serial.println("[portal] 登录成功，外网已恢复");
    else              Serial.println("[portal] 登录后仍未通，检查账号或接口是否变化");
#else
    Serial.println("[net] 提示: 若是被认证页拦截，请把 secrets.h 里 ENABLE_PORTAL_LOGIN 改成 1");
#endif
  }
}

// ================= 初始化 =================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n\n===== ESP32 校园网中继启动 =====");

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);                     // 关省电，转发更稳
  WiFi.setHostname(STA_HOSTNAME);
  WiFi.setAutoReconnect(true);

  // 这行 MAC 拿去学校自服务系统加白名单
  Serial.printf("[info] STA MAC = %s  (把这个 MAC 加到免认证/白名单)\n",
                WiFi.macAddress().c_str());

  Serial.printf("[net] 连接上游 %s ...\n", UPSTREAM_SSID);
  WiFi.begin(UPSTREAM_SSID, UPSTREAM_PASS);
  if (!waitSta(STA_WAIT_MS)) {
    Serial.printf("[net] 上游连接失败: %s\n", wlStatusStr(WiFi.status()));
    Serial.println("[net] 仍会开热点，之后自动重试");
  } else {
    Serial.printf("[net] 已连接  本机IP=%s  网关=%s  信道=%d\n",
                  WiFi.localIP().toString().c_str(),
                  WiFi.gatewayIP().toString().c_str(),
                  WiFi.channel());
  }

  s_staNetif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");

#if ENABLE_PORTAL_LOGIN
  if (WiFi.status() == WL_CONNECTED && !internetOK()) {
    Serial.println("[portal] 未认证，开始登录...");
    portalLogin();
  }
#endif
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[net] 外网状态: %s\n", internetOK() ? "可上网" : "不可上网(待认证?)");
  }

  startAp();

  s_apNetif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
  if (!s_apNetif) {
    Serial.println("[err] 取不到 AP netif，NAT 无法启用");
  } else {
    applyApDns();
    enableNapt();
  }

#if USE_DNS_PROXY
  dnsProxyStart();
#endif

  s_lastCheck = millis();
  Serial.println("===== 就绪：设备连上热点即可上网 =====");
}

void loop() {
#if USE_DNS_PROXY
  dnsProxyPoll();
#endif

  if (millis() - s_lastCheck >= CHECK_INTERVAL_MS) {
    s_lastCheck = millis();
    maintain();
  }
  delay(2);
}
