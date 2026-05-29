# switch-pctltcp-sysmodule

Switch 家长控制 Web UI —— 后台常驻系统服务（boot2 sysmodule）

开机自动启动，通过浏览器访问 Switch 的 IP 地址即可设置家长控制时间。

## 安装方式

下载 GitHub Actions 构建的 `pctltcp-sysmodule.nsp`，然后：

```
# 放到 SD 卡（创建对应目录）
SD:/atmosphere/contents/0100000000000023/exefs.nsp
SD:/atmosphere/contents/0100000000000023/toolbox.json
SD:/atmosphere/contents/0100000000000023/flags/boot2.flag  # 空文件，开机自启
```

**目录结构最终如下：**
```
SD卡:/
└── atmosphere/
    └── contents/
        └── 0100000000000023/
            ├── exefs.nsp
            ├── toolbox.json
            └── flags/
                └── boot2.flag  # 空文件即可
```

重启 Switch，服务自动启动。

## 使用方式

1. 在 Switch 上确认已连接 WiFi，查看「设置 → 互联网 → 连接状态」获取 IP 地址
2. 电脑/手机浏览器打开 `http://<Switch-IP>:8080`
3. 设置家长控制时间，保存即可

## 调试

如果无法访问，把 SD 卡插回电脑，查看日志：

```
SD卡:/switch/pctltcp-sysmodule/sysmodule.log
```

日志包含：
- 服务启动时间
- Switch 的 IP 地址
- HTTP server 启动/重启记录
- 错误信息

## 构建（开发者）

需要 [devkitPro](https://devkitpro.org/) 环境：

```bash
make clean
make        # 输出 pctltcp-sysmodule.nsp
```

GitHub Actions 会自动构建，在 [Actions](https://github.com/gmaitxqqq/switch-pctltcp-sysmodule/actions) 页面下载 Artifact 即可。

## 技术说明

- 采用 **boot2 sysmodule** 方式（与 MissionControl 蓝牙插件相同机制）
- `boot2.flag` 告诉 Atmosphere 开机自动启动
- `toolbox.json` 声明插件名称和 Title ID
- 主循环永不退出，HTTP server 挂掉会自动重启
- 每 5 分钟检测 IP 变化（切换 WiFi 后自动更新日志）

## 参考

- [MissionControl](https://github.com/MissionControl/MissionControl) — boot2 sysmodule 参考实现
- [sys-con](https://github.com/o0Zz/sys-con) — 第三方手柄支持 sysmodule
