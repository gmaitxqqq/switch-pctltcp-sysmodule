# Switch 家长控制 Web UI — 后台常驻版

开机自动启动的后台系统服务（boot2 sysmodule），通过浏览器访问 Switch IP 即可设置家长控制时间。无需手动打开任何应用，开机即用。

**版本**：v1.4 | **端口**：8081 | **固件**：兼容 Atmosphere 22.1.0+

---

## 适用场景

> 固定 IP 的家庭环境，Switch 常驻同一 WiFi，开机自动运行，随时用浏览器管理。

---

## 安装

1. 从 [Releases](../../releases) 下载最新版 zip
2. 解压后复制到 SD 卡，最终目录结构：

```
SD卡:/
└── atmosphere/
    └── contents/
        └── 010000000000BD23/
            ├── exefs.nsp        ← 重命名自 pctltcp-sysmodule.nsp
            ├── toolbox.json
            └── flags/
                └── boot2.flag   ← 空文件，开机自启
```

3. 重启 Switch，服务自动启动

> ⚠️ **注意**：下载的 `pctltcp-sysmodule.nsp` 需重命名为 `exefs.nsp` 放入上述目录。

---

## 使用方法

1. 确认 Switch 已连接 WiFi（设置 → 互联网 → 查看 IP 地址）
2. 电脑/手机浏览器打开 `http://<Switch-IP>:8081`
3. 即可设置家长控制时间

### Web UI 功能

- 查看实时计时状态（运行/暂停、剩余时间）
- 按天设置每日时间限制（周日至周六）
- 统一设置所有天相同限额
- 启动 / 暂停计时器

### REST API

| Method | Path | 说明 |
|--------|------|------|
| GET | `/` | Web UI 页面 |
| GET | `/api/status` | 计时器状态（JSON） |
| POST | `/api/allow` | 增加游玩时间：`{"minutes": 30}` |
| GET | `/api/version` | 版本号 |

---

## 调试

无法访问时，查看 SD 卡日志：

```
SD:/switch/pctltcp-sysmodule/sysmodule.log
```

日志包含：启动时间、IP 地址、HTTP server 状态、错误信息、时区加载结果。

---

## 特性

- **开机自启**：boot2 方式，Atmosphere 启动时自动加载
- **后台常驻**：不需要打开任何应用，完全后台运行
- **自动恢复**：HTTP server 挂掉自动重启，网络异常自动重连
- **休眠恢复**：息屏唤醒后自动检测并重建网络（5-10 秒内恢复）
- **IP 变化检测**：切换 WiFi 后自动更新日志
- **Hekate 工具箱**：`toolbox.json` 支持在 Hekate 中显示插件名称

---

## 卸载

删除 SD 卡上对应目录即可：

```
SD:/atmosphere/contents/010000000000BD23/
```

重启 Switch。

---

## 项目结构

```
switch-pctltcp-sysmodule/
├── source/
│   ├── main.c              # sysmodule 入口 + 网络健康检查循环
│   ├── http_server.c/h     # HTTP 服务端 + 嵌入式 Web UI
│   └── pctl_handler.c/h    # pctl IPC 封装 + 时区加载
├── pctltcp-sysmodule.json  # NPDM 权限配置
├── toolbox.json            # Hekate 工具箱声明
└── Makefile
```

---

## 从源码编译

```bash
export DEVKITPRO=/opt/devkitpro
make clean && make        # 输出 pctltcp-sysmodule.nsp
```

推送至 GitHub 后 Actions 自动构建，在 [Actions](../../actions) 页面下载 Artifact。

---

## 技术要点

- **boot2 sysmodule**：与 MissionControl 蓝牙插件相同机制
- **CRT0 覆写**：`__nx_applet_type = 0` + 自定义 `__appInit`/`__appExit`
- **NPDM 权限**：`service_access: ["*"]`，`service_host: []`（空数组，非通配符）
- **pctl 按需使用**：每次 API 调用 init/exit，避免单客户端限制冲突
- **时区处理**：sysmodule 中 `localtime()` 无时区数据，通过 `setsys` + `timeLoadTimeZoneRule` 显式加载
- **网络恢复**：不使用 PSC 电源监控（会死锁），改用主循环健康检查自动重建

---

## 同系列工具

| 项目 | 类型 | 适用场景 |
|------|------|---------|
| [switch-parental-timer](https://github.com/gmaitxqqq/switch-parental-timer) | 本机 NRO | 在 Switch 上直接操作，无需网络 |
| [switch-pctltcp-nro](https://github.com/gmaitxqqq/switch-pctltcp-nro) | 前台 NRO + TCP | 固定 IP 局域网，PC 客户端远程管理 |
| [switch-pctltcp-web](https://github.com/gmaitxqqq/switch-pctltcp-web) | 前台 NRO + Web UI | 外出时手机浏览器管理（无固定 IP） |
| **switch-pctltcp-sysmodule**（本仓库） | 后台 sysmodule | 固定 IP 家庭环境，开机自动运行 |

---

## 版本历史

| 版本 | 变更 |
|------|------|
| **v1.4** | 移除 PSC 电源监控（解决休眠唤醒死机），改用主循环健康检查自动恢复 |
| **v1.3** | 修复星期读取错误，显式加载时区规则；端口改为 8081 |
| **v1.2** | 修复 pctl 按需 init/exit，避免与系统家长控制界面冲突 |
| **v1.1** | 修复 socket 初始化（改用 BsdServiceType_System）；修复 NPDM service_host 配置 |
| **v1.0** | 首个可用版：boot2 sysmodule，HTTP Web UI，日志记录 |
