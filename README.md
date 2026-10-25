# 噬神者：解放重生 · 手柄拔插失效修复（GOD EATER RESURRECTION gamepad hot-plug fix）

[![build](https://github.com/KunBaoQAQ/ger-gamepad-hotplug-fix/actions/workflows/build.yml/badge.svg)](https://github.com/KunBaoQAQ/ger-gamepad-hotplug-fix/actions/workflows/build.yml)

修复 **GOD EATER RESURRECTION（噬神者：解放重生，Steam 版，32 位）** 的一个顽疾：

> 游戏运行中把手柄拔掉再插回去，手柄就再也不响应，必须重启游戏。

本仓库提供一个 **DirectInput8 代理 DLL**（`dinput8.dll`），不修改游戏本体、
不修改存档、不修改任何游戏资源；与 **CODE EATER 汉化组** 的 v1.10.3 汉化补丁
自动兼容（安装脚本会把汉化补丁的加载器改名为 `GECV_LOADER.dll` 保留）。

> 本仓库不包含游戏文件、不包含汉化补丁本体。

---

## 一、根因（实测分析）

1. `GER.exe` 是 32 位程序，手柄输入走 **DirectInput8**
   （二进制里有 `QDirectInputPad` / `io::Controller` 等符号；
   `XInputSetState` / `XInputGetCapabilities` 只用于震动）。
2. `IDirectInputDevice8` 设备对象绑定在**物理设备实例**上。USB 手柄一拔，
   该对象永久失效，之后 `Poll()` / `GetDeviceState()` 始终返回
   `DIERR_INPUTLOST` / `DIERR_UNPLUGGED` / `DIERR_NOTACQUIRED`，
   重新插上也不会复活（必须重新 `CreateDevice`）。
3. 更致命的一层：游戏会周期性调用 `IDirectInput8::GetDeviceStatus()`。
   拔掉手柄后它拿到失败结果，就把该手柄从"活动手柄列表"里删掉、
   **再也不去轮询** —— 所以只能重启游戏。
   （实测日志：拔掉后 `GetDeviceState` 调用计数直接冻结，不再增长。）

## 二、修复原理

代理层接管 `IDirectInput8` / `IDirectInputDevice8`（ANSI + Unicode），并：

- 读取出错时自动恢复：先重新 `Acquire()`；不行就重新枚举 + `CreateDevice`，
  并把原来的 **数据格式 / `DIPROP_*` 属性 / `SetCooperativeLevel` /
   `SetEventNotification` / `Acquire`** 原样重放一遍；
- 手柄不在的窗口期上报**中性输入**（摇杆居中、按键全松开），
  让游戏永远看不到"手柄掉了"（也避免角色乱走/乱按）；
- `GetDeviceStatus()` 对已知手柄返回"已连接"，引擎不再停止轮询；
- 后台看门狗线程持续修复（即使游戏某一刻停止轮询）；
- 拦截 `WM_DEVICECHANGE` / `RegisterDeviceNotification`，避免引擎在拔掉瞬间
  就把手柄从列表里删除。

结果：拔掉 = 暂时没输入；插回 = 自动恢复，**无需重启**。

### 关键踩坑记录

- `DIPROP_RANGE` 在 `dinput.h` 里是 `MAKEDIPROP(4)`，即
  `(REFGUID)4` —— 一个**伪 GUID 指针**，绝对不能解引用。
  早期版本在这里直接 Access Violation（已在源码中处理）。
- 手柄设备要用 `c_dfDIJoystick2`（164 objects / 272 bytes），
  属性设置使用 `DIPH_BYID`。

## 三、安装

1. 下载 **整个仓库 / Release 压缩包**（不要只拿 `dinput8.dll`）。
2. 双击 `安装.bat`：
   - 自动在 Steam 库中查找游戏目录，找不到会让你粘贴路径；
   - 若检测到汉化补丁的 `dinput8.dll`（GECV 加载器）→ 自动改名
     `GECV_LOADER.dll` 保留，**汉化不受影响**；
   - 若目录里已有其它 `dinput8.dll` → 备份为 `dinput8.dll.backup-时间戳`。
3. 从 Steam 正常启动游戏。

卸载：双击 `卸载.bat`（会删除代理与配置，并把 `GECV_LOADER.dll` 还原成
`dinput8.dll`）。

### 使用注意

- **请先插好手柄，再启动游戏**。引擎只在启动时枚举一次手柄；
  启动时没有手柄，之后插上它不会认。
- 进入游戏后可以随意拔插，会自动恢复。
- 汉化补丁更新后，请重新运行一次 `安装.bat`。

## 四、配置文件 `dinput8_fix.ini`

| 键 | 默认 | 说明 |
|---|---|---|
| `[General] Log` | 1 | 写 `dinput8_fix.log`（诊断用，随时可删） |
| `KeepAlive` | 1 | 手柄不在时仍上报"已连接 + 中性输入"（**修复核心，保持 1**） |
| `DisconnectedMode` | 0 | 0 中性输入 / 1 保持最后状态 / 2 上报错误（会重现原 bug） |
| `Watchdog` | 1 | 后台看门狗线程 |
| `RebindIntervalMs` | 400 | 重新枚举设备的最小间隔 |
| `HeartbeatSeconds` | 30 | 日志心跳间隔，0 = 关闭 |
| `Verbose` | 0 | 记录每次 DirectInput 调用（排错用，很啰嗦） |
| `BlockDeviceChange` | 1 | 拦截设备变更通知（**修复的一部分**） |
| `Diagnostics` | 1 | 异常时写 `dinput8_crash.log` |
| `[Borderless] Enabled` | 0 | 强制无边框全屏（需游戏处于窗口化模式） |
| `ToggleKey` | 122 | 无边框开关热键（122 = F11，0 = 关闭） |
| `DpiAware` | 跟随 Enabled | 让进程 DPI 感知，避免 Windows 二次缩放发糊 |

诊断配置是 **UTF-16 BOM** 编码（Windows INI 标准），Notepad / VSCode 都能正常编辑。

## 五、可选：无边框全屏

1. `dinput8_fix.ini` 里 `[Borderless] Enabled=1`；
2. 游戏设为窗口化（游戏内设置，或 `config.ini` 的 `[GFX] Fullscreen=0`）；
3. 建议分辨率设为显示器原生分辨率；
4. 游戏内按 **F11** 随时开关强制无边框。

无边框模式下建议开垂直同步，否则可能有画面撕裂。

## 六、常见问题

**Q：杀软 / Defender 报毒？**
DLL 代理（同名 `dinput8.dll`）容易被启发式误报。本仓库附带完整源码，
可自行编译核对；也可把游戏目录加入信任。DLL 只依赖 `kernel32.dll`，
不联网、不写注册表、不改游戏文件。

**Q：会影响联机 / 被当成外挂吗？**
不会。它只修复手柄输入设备的读取，不修改游戏内存数据、不修改存档、
不影响网络同步，不提供任何游戏内优势。

**Q：支持 Steam Input / DS4Windows / 无线接收器拔插吗？**
支持。修复工作在 DirectInput 层，对这些方案同样有效。

**Q：热插拔后日志一直显示 `device not present`？**
说明系统层面手柄还没回来（驱动 / 接收器重连较慢）。等几秒会自动恢复；
若长时间不恢复，检查设备管理器或重新插拔一次。

**Q：想用原版（不修复）？**
卸载即可，或删除 `dinput8.dll` + `dinput8_fix.ini`，把 `GECV_LOADER.dll`
改名回 `dinput8.dll`。

## 七、自行编译

需要 Visual Studio（"使用 C++ 的桌面开发"）+ Windows 10/11 SDK：

```bat
src\build.bat
```

输出 `src\out\dinput8.dll`（**必须 32 位 /MACHINE:X86**，因为 GER.exe 是 32 位）。
CI 见 `.github/workflows/build.yml`（GitHub Actions 用 MSVC x86 重新编译并上传产物）。

> 注意：CI 用的是云端最新版 MSVC，编译出来的 DLL 与仓库里 `files\dinput8.dll`
> **功能等价但字节不同**（构建时间戳 / 编译器版本不同），所以哈希会有差异。
> 仓库/Release 里那份的校验值以本 README 第八节为准。

## 八、文件校验（v1.0）

```
dinput8.dll  172,032 bytes
  MD5     DB635DE1BA6B24BFC8A39F25AB96B623
  SHA256  09D67F8118FB36D0780F1D7B6F8ABD97F11D7BC2041147B670F40C632EF884C9
```

## 九、致谢 / 免责声明

- 汉化补丁来自 **CODE EATER 汉化组**（Qingyu510 / HaoJun0823 等），
  本仓库不包含、也不再分发其任何文件，仅在安装时保留其 `GECV_LOADER.dll`。
- 本修复与 Bandai Namco / Shift / QLOC 无关。
- 仅供学习交流使用，请支持正版游戏。

## License

[MIT](LICENSE)
