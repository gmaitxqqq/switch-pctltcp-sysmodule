# switch-pctltcp-sysmodule

## 🎯 项目系列演进故事

本项目历经 **6 次迭代**，逐步从本机工具演进为完善的双通道远程管理方案：

| 版本 | 仓库 | 日期 | 核心改进 |
|------|------|------|----------|
| V1 | switch-parental-timer | 05-25 | 本机 NRO 工具，直接操作家长控制 |
| V2 | switch-pctltcp-nro | 05-26 | 增加 TCP 服务器，PC 客户端远程管理 |
| V3 | switch-pctltcp-web | 05-27 | TCP 改为 HTTP，手机浏览器直接管理 |
| V4 | switch-pctltcp-sysmodule | 05-27 | 转为后台 sysmodule，开机自启 |
| V5 | switch-pctltcp-remote | 05-31 | 增加远程隧道，支持外出管理 |
| **V6（最终版）** | **[switch-pctltcp-remoteandlocal](https://github.com/gmaitxqqq/switch-pctltcp-remoteandlocal)** | **06-09** | **双通道：远程 + 本地局域网** |

> 📖 完整演进故事和所有版本对比，请查看最终版仓库：
> https://github.com/gmaitxqqq/switch-pctltcp-remoteandlocal

## 📊 系列工具对比

本项目共有 6 个版本，逐步演进。请根据使用场景选择合适的版本：

| 版本 | 仓库 | 类型 | 适用场景 | 核心特点 |
|------|------|------|----------|----------|
| V1 | [switch-parental-timer](https://github.com/gmaitxqqq/switch-parental-timer) | 本机 NRO | 在 Switch 上直接操作，无需网络 | PIN 验证、纯前台应用 |
| V2 | [switch-pctltcp-nro](https://github.com/gmaitxqqq/switch-pctltcp-nro) | 前台 NRO + TCP | 固定 IP 局域网，PC 客户端远程管理 | TCP 文本协议、PC Tkinter 客户端 |
| V3 | [switch-pctltcp-web](https://github.com/gmaitxqqq/switch-pctltcp-web) | 前台 NRO + Web UI | 外出时手机浏览器管理（无固定 IP） | HTTP 服务器、手机友好 UI |
| V4 | [switch-pctltcp-sysmodule](https://github.com/gmaitxqqq/switch-pctltcp-sysmodule) | 后台 sysmodule | 固定 IP 家庭环境，开机自动运行 | 后台服务、LAN only |
| V5 | [switch-pctltcp-remote](https://github.com/gmaitxqqq/switch-pctltcp-remote) | 后台 sysmodule | 需要远程控制（外出管理） | 远程隧道、长轮询 |
| **V6（推荐）** | **[switch-pctltcp-remoteandlocal](https://github.com/gmaitxqqq/switch-pctltcp-remoteandlocal)** | **后台 sysmodule** | **最完善方案，双通道控制** | **远程 + 本地、高可靠** |

> ⭐ **推荐直接使用 V6 最终版**，功能最完整。


Nintendo Switch 家长控制 sysmodule，**局域网版**（纯 LAN，无需外网服务器）。

> 如果你需要 **远程控制**（外出时管理），请使用 [switch-pctltcp-remote](https://github.com/gmaitxqqq/switch-pctltcp-remote)。

## 功能

| 特性 | 说明 |
|------|------|
| 开机自启 | boot2 sysmodule，无需手动启动 |
| 浏览器控制 | `http://<Switch-IP>:8081` |
| 按天设限 | 每天独立设置游玩时限（5 分钟增量） |
| 周配额 | 每天独立限额，互不干扰 |
| 实时查看 | 剩余时间、已玩时间 |
| 随时追加 | 正数加时间，负数减时间 |
| 休眠恢复 | 网络断线自动重连 |
| 配置热重载 | 改配置无需重启 |

## 快速开始

### 安装

1. 从 [Release](../../releases) 页面下载最新版 `pctltcp-sysmodule.zip`
2. 解压到 SD 卡根目录，确认目录结构：
   ```
   sdmc:/atmosphere/contents/010000000000BD23/
   ├── exefs.nsp
   ├── toolbox.json
   └── flags/
       └── boot2.flag
   ```
3. 重启 Switch（或通过 Hekate 重启到 CFW）

### 使用

1. 在 Switch 上确认 sysmodule 已加载（查看日志 `sdmc:/switch/pctltcp-sysmodule/sysmodule.log`）
2. 在电脑/手机浏览器访问 `http://<Switch-IP>:8081`
3. 输入要追加的分钟数，点 Confirm

> 💡 查看 Switch IP：`设置 → 互联网 → 连接状态 → IP 地址`

## API

| 接口 | 方法 | 说明 |
|------|------|------|
| `/api/status` | GET | 获取当前状态（JSON） |
| `/api/allow` | POST `minutes=N` | 追加（正数）或减少（负数）游玩时间 |

### 状态 JSON 格式

```json
{
  "daily_limit_min": 120,
  "remaining_min": 45,
  "played_min": 75,
  "today": 3,
  "today_name": "Wed",
  "version": "v1.8.0"
}
```

## 技术说明

### 架构

- **boot2 sysmodule**：开机自动启动，独立后台进程
- **单线程 + select()**：HTTP 服务器基于 `select()` 事件驱动，无线程生命周期 bug
- **永久线程**：HTTP 线程持续运行，WiFi 重连时只换 socket，不销毁线程
- **懒初始化 pctl**：仅在 HTTP 请求时初始化 pctl 服务，避免启动时崩溃

### 修复历史

| 版本 | 重要修复 |
|------|---------|
| v1.8.1 | **彻底修复**休眠唤醒后 8081 端口无法访问。借鉴远程隧道"每次都是新 socket"思路，在 `accept()` 连续失败时主动关闭并重建 listening socket。 |
| v1.8.0 | 合并 remote 架构改进：去掉 `pthread_join()`（修复 2168-0002），增加 client socket 超时，线程永久运行 |
| v1.5.0 | 修复 `http_server_stop()` 竞态，支持负数减少时间，减少 WiFi 等待延迟 |
| v1.4.1 | 休眠/唤醒自动恢复网络连接 |

### 构建

需要 [devkitPro](https://devkitpro.org/) + libnx：

```bash
cd /path/to/switch-pctltcp-sysmodule
make
```

输出：`pctltcp-sysmodule.nsp`，安装到 `sdmc:/atmosphere/contents/010000000000BD23/exefs.nsp`

## 常见问题

**Q: 浏览器访问不了？**
A: 确认 Switch 和手机/电脑在同一 WiFi；确认端口 8081 未被防火墙拦截。

**Q: 安装后 Switch 启动卡住？**
A: 删除 `sdmc:/atmosphere/contents/010000000000BD23/` 目录，重启，排查日志 `sdmc:/switch/pctltcp-sysmodule/sysmodule.log`。

**Q: 和 remote 版有什么区别？**
A: 此版本（sysmodule）只支持局域网控制；remote 版额外支持通过互联网远程下发控制命令。两个版本 **不能同时使用**。

## 授权

MIT License
