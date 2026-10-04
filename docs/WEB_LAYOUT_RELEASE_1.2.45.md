# 1.2.45-usbtest5 页面整理版

2026-10-02 经用户授权 OTA 到 192.168.31.4，HTTP 200、success=true。
构建环境为 `esp32s3_n16r8_usb_test`，沿用此前已刷 TEST 5 的 USB 实现，仅更新版本号与页面。

## 页面

- Wi-Fi 顶部状态卡三行：SSID/空闲休眠、IP、热点状态/域名访问。
- 系统设置为左右两栏，Wi-Fi 与配网、AP 热点配置分成两张卡。
- 出厂重置归备份与恢复；诊断与日志集中，保留原配对页与紧凑蓝牙首页状态卡。
- 撤回用户不满意的蓝牙信息大卡整合。

## 验证

- 编译通过：RAM 114684 字节，应用 1427457 字节。
- 上传镜像 1427824 字节，SHA256 `003E85B2C4B58378BE10612FEFD0D459924F4E17583F761C5E5282519A450603`。
- 设备从 app0 切换至 app1，版本为 1.2.45-usbtest5，image_state=valid。
- 启动超过 120 秒后 rollback_armed=false、boot_fail_count=0。
- 实际 HTTP 页面与定稿源码全文核对一致。
- Wi-Fi/AP/节能/按键配置、蓝牙绑定、保护参数升级前后核对一致。
- Windows 三次麦克风开关采集通过，传输约 31.5–31.7 KB/s；音频内容丢弃、不保存。

首次受限环境的 WinMM 打开返回 error 1，额外权限下的检查通过。
运行日志另记录 uptime 179 秒时 HID 卡顿触发 USB Recovery #1，自动重连后恢复；设备未再次重启。
升级前 TEST 5 的本次启动已有 8 次 USB recovery。USB 源文件哈希未变，本次不宣称修复底层卡顿。
升级前后原始诊断、日志、配置脱敏快照和检查输出一并保留，便于继续排查。

归档：`.cache/firmware-1.2.45-usbtest5-ui/`（binary、ELF、源码、manifest、诊断与检查记录）。
回退：`.cache/firmware-1.2.44-usbtest5-flashed/firmware.bin`，使用相同 OTA 入口上传应用镜像。
