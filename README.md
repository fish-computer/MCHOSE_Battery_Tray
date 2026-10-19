# MCHOSE 键盘电量托盘 (MCHOSE Battery Tray)

在 Windows 任务栏上直接显示迈从（MCHOSE）无线键盘的剩余电量 —— 一个绿色的小胶囊，里面是百分比数字，鼠标移上去看详情，右键出菜单。

- 单文件 exe，约 310 KB，静态链接 CRT，**不需要 .NET / Python / 运行库**
- **不需要管理员权限**，不装驱动、不注入 explorer、不挂后台服务
- 直接读 USB 接收器的 HID 报文，不依赖 MCHOSE HUB 是否在运行
- 自带安装 / 卸载，安装后自动开机启动，卸载后自删干净

> 样式对标官方 MCHOSE HUB 的托盘胶囊：绿底白字显示剩余百分比，低于 50% 转琥珀色，低于 20% 转红色。

## 截图

| 托盘图标 | 右键菜单 |
| --- | --- |
| ![托盘图标](docs/tray.png) | ![右键菜单](docs/menu.png) |

图标会随电量变色（绿 → 琥珀 → 红），读不到时显示灰色 `?`，不会瞎猜。

---

## 快速开始

从 [Releases](../../releases) 下载 `MchoseTray.exe`，双击运行即可。

第一次运行会问要不要安装到本机：

- **是** → 复制到 `%LOCALAPPDATA%\Programs\MCHOSEBatteryTray\`，写入开机启动，
  并在「设置 → 应用 → 已安装的应用」里登记，之后从开始菜单启动。
- **否** → 便携模式，就在当前目录跑，关掉即走。

也可以手动：

```powershell
.\MchoseTray.exe --install     # 安装到本机并开机自启
.\MchoseTray.exe --uninstall   # 卸载（删除自启、快捷方式、注册表项、程序目录）
.\MchoseTray.exe --tray        # 只跑托盘（不弹安装提示）
.\MchoseTray.exe --once        # 读一次电量，打印后退出
.\MchoseTray.exe --probe       # 枚举所有 HID 设备并尝试识别
```

> **Windows 11 提示**：新程序的托盘图标默认会被折叠进 `^` 溢出区。
> 打开 `^`，把电量胶囊拖到任务栏上即可常驻（只需一次）。

## 支持的设备

目前实现了迈从的私有 HID 协议（`mchose-v1`）：

| 设备 | 状态 |
| --- | --- |
| 迈从 G87（2.4G 接收器 `41E4:2001`） | ✅ 实机验证 |
| 其他迈从 2.4G 键盘 / 鼠标（同协议族） | 大概率可用，欢迎回报 |

识别规则只看 **VID/PID + HID 接口特征**（`usagePage=0xFF02`、20 字节输入输出报表），
不按型号写死，所以同协议的其它型号应当直接能用。遇到不支持的设备，
`--probe` 的输出可以直接贴到 issue 里。

## 右键菜单

```
MCHOSE 2.4G Wireless        ← 设备名
电量 100%                    ← 电量（充电时显示「充电中」）
12 秒前更新                  ← 上次成功读取的时间
──────────────
立即刷新            F5
打开 MCHOSE HUB
──────────────
开机自启             [勾]
安装到本机 / 卸载...
图标不见了？
打开日志文件夹
──────────────
退出
```

「图标不见了？」会说明 Windows 11 的 `^` 溢出区怎么把图标拖出来。

## 取电策略（为什么它很轻）

键盘空闲时会睡过去，那时接收器**完全不推报文**，硬轮询只会把键盘一直吵醒、白耗电。
所以：

- 读到一次成功值后 **5 分钟**才查下一次
- 失败则 **15 秒**后重试
- 额外监听「用户从长时间空闲忽然有输入」的瞬间，那时键盘刚被唤醒，是最省事的下手时机
- 键盘睡着时**保留上次读数**，不会跳成 0% 或者 `?`

整套东西常驻内存个位数 MB，CPU 基本为 0（无轮询线程空转）。

## 从源码构建

需要 Visual Studio（2019/2022 或更新的 MSVC 工具链）：

```powershell
.\build.ps1          # 产物在 dist\MchoseTray.exe
```

`build.ps1` 会自动找 `vcvars64.bat`，编译参数 `cl /std:c++17 /utf-8 /O2 /MT`，
所以产物是单文件、不依赖 VC 运行库。

## 仓库结构

```
src/
  main.cpp            托盘、菜单、定时刷新、单实例、模式分发
  hid.h / hid.cpp     SetupAPI + hid.dll 的极简封装（无第三方库）
  protocol.h          驱动接口：一个 Driver = 识别设备 + 读电量
  protocol_mchose.cpp 迈从协议实现（本仓库唯一的机型相关代码）
  icon.h / icon.cpp   GDI+ 画电量胶囊（超采样抗锯齿，按 DPI 出图）
  install.h / .cpp    安装 / 卸载 / 开机自启
  common.h            路径、日志、DPI、格式化
  app.manifest        Per-Monitor V2 DPI、Win10/11 支持声明
docs/PROTOCOL.md      协议逆向记录（帧格式、命令、实测数据）
```

## 加一个新机型

`protocol.h` 里的 `Driver` 只有两个方法：

```cpp
class Driver {
public:
    virtual const wchar_t* id() const = 0;             // "mchose-v1"
    virtual const wchar_t* displayName() const = 0;    // "MCHOSE"
    virtual bool matches(const HidInterface&) = 0;     // 这个设备归我管吗
    virtual bool read(const HidInterface&, Reading&) = 0;
};
```

照着 `protocol_mchose.cpp` 写一个，在 `Drivers()` 里注册，其余代码不用动。

## 卸载

三种方式，效果一样：

1. 「设置 → 应用 → 已安装的应用」→「MCHOSE 键盘电量托盘」→ 卸载
2. 托盘右键 →「卸载...」
3. 命令行 `MchoseTray.exe --uninstall`

会删掉开机启动项、开始菜单快捷方式、卸载登记项和程序目录本身。

## 常见问题

**双击后弹出「Windows 已保护你的电脑」** —— exe 没有代码签名，点「更多信息 → 仍要运行」即可。
想要彻底没有这个提示，可以自己用 signtool 加一个自签名证书，或者从源码编译。

**点「安装到本机」提示无法创建安装目录（错误码 5）** ——
说明 exe 所在的文件夹本身不允许写（某些受管目录、杀毒软件的保护目录、或只读介质）。
把 exe 挪到桌面 / 下载目录再运行一次即可。

**图标在 `^` 里，拖不出来** ——
右键菜单点「图标不见了？」有说明；也可以在「设置 → 个性化 → 任务栏 → 其他系统托盘图标」
里把它打开。

**读不到电量** —— 先确认接收器插着、键盘是 2.4G 模式（不是蓝牙）；然后
`MchoseTray.exe --once` 看输出，`--probe` 会列出所有 HID 接口供排错。

## 已知限制

- 只在 **Windows 10/11 x64** 上测过；ARM64 未验证（理论上重新编译即可）。
- 蓝牙连接的键盘走不到这条 HID 通道，只支持 2.4G 接收器。
- 电量读数是设备上报的粗粒度值（通常 10% 一跳）。

## License

MIT
