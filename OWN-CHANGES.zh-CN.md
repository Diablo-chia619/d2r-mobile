# 本 fork 的改动说明（自编 Android 包）

这个仓库是 [jhaizhou-ops/d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile)（MIT）的 fork。除上游代码外，`android/turnip-and-stick` 分支上有四组改动，用于做一个**能与官方 APK 并存安装、内置 Turnip 驱动、并把触屏摇杆手感调顺**的 Android 自编包。

- 发布物：<https://github.com/Diablo-chia619/d2r-mobile/releases/tag/own-2.3.0-t30-1>
- 不含游戏内容，游戏文件自备，见 [NOTICE.md](NOTICE.md)
- 基线版本：上游 **v2.3.0**（不是 2.3.1；2.3.1 只改了出包工具与 iOS 壳，运行时零变化）

改动的基本原则：**新增文件 + 少量接线 + 一个可选构建参数**，不往引擎内部改，官方升版后重放成本低（见文末「升级与重放」）。

---

## 1. 换驱动：内置 Turnip（adrenotools）

### 做了什么

引擎里 Vulkan 入口有 **117 个、直接调用 547 处**，逐个改调用点不现实。做法是在**链接期**把这些入口整体接管：

| 文件 | 作用 |
|---|---|
| `dm/android/vk_shim.c` | 117 个 Vulkan 入口的转发体（自动生成，不手写） |
| `dm/android/tools/gen_vk_shim.py` | 从 `libdm.so` 未定义符号生成上面的转发体 |
| `dm/android/vk_runtime.c` / `.h` | 加载真实 `libvulkan.so`、选择系统/Turnip、打印 `[vk]` 日志 |
| `dm/gfx/vulkan/CMakeLists.txt` | Android 分支**不再链接 `vulkan`**，只留 `log` |
| `dm/android/CMakeLists.txt` | 把上面两个 `.c` 编进同一个目标，加 `-fvisibility=hidden` |
| `dm/android/app/build.gradle` | 可选参数 `-PdmGpuDir=<dir>`：把驱动资产打进 APK |
| `dm/android/app/src/main/java/com/xiaoboluo/d2r/DmActivity.java` | 首启把 assets 里的驱动解到 `files/gpu/turnip/` |

为什么必须「不链接 + 隐藏可见性」：如果仍链接系统 `libvulkan`，链接器会把同名符号优先绑到系统库，我们的转发体永远不被调用（表现是日志里一条 `[vk]` 都没有）。去掉链接之后，**漏掉的入口会在编译期直接报未定义符号**——这就是完整性证明。

### 驱动资产

| 资产 | 来源 | 许可 |
|---|---|---|
| `assets/turnip/vulkan.purple.so` | Mesa Turnip T30（26.3.0-devel / Vulkan 1.4.359） | MIT |
| `lib/arm64-v8a/libadrenotools.so` + 4 个 hook | [eden-emulator/libadrenotools](https://github.com/eden-emulator/libadrenotools) | BSD-2-Clause |
| `lib/arm64-v8a/libhardware.so` | 本仓库自写的桩（`dm/android/gpu_stubs/hw_stub.c`） | 同本仓库 |

### 开关（不需要重新打包）

写 `files/dm/env`（App 私有目录，或 `/sdcard/Android/data/com.xiaoboluo.d2r.own/files/dm/env`）：

```
DM_GPU_DRIVER=turnip      # 内置 Turnip；改成 system 或删掉这行 = 系统驱动
DM_GPU_DRIVER_REQUIRE=1   # 加载失败直接报错，不静默回落（排查时非常有用）
```

### 三个会「静默失败」的坑（移植时踩过）

1. **`adrenotools_open_libvulkan` 的 `customDriverDir` 必须带结尾斜杠**。它内部是 `std::string(dir) + name` 直接拼接（`driver.cpp` 里的 `stat`），少了斜杠会去找 `.../turnipvulkan.purple.so`，`stat` 失败就 `return nullptr`——**一行日志都不打**。
2. **Turnip 依赖平台私有库 `libhardware.so`**，App 的命名空间看不到它，报 `dlopen failed: library "libhardware.so" not found ... in namespace clns-9`。驱动只从它引用一个符号 `hw_get_module`，自己编一个返回 `-ENOENT` 的桩就能过，Mesa 会走 `AHardwareBuffer` 路径。
3. **`DM_GPU_DRIVER` 读的是「运行目录」下的 `env`**，而运行目录只在内部 `files/dm/out/main.patched.bin` 存在时才是 `files/dm`。导入包没走完时它会静默回落到系统驱动——别相信「我写了 env」，只认日志里的 `requested=` / `selected=`。

### 怎么验证真的在用 Turnip

```bash
adb shell run-as com.xiaoboluo.d2r.own grep -a '\[vk\]' files/dm/boot.log | tail -6
# 期望四行：internal dir / hook dir / 驱动选择：Turnip（adrenotools）/ instance: 解析 117/117，缺 0（requested=turnip selected=turnip）

adb shell run-as com.xiaoboluo.d2r.own grep -a 'NVN caps device' files/dm/boot.log | tail -1
# Turnip：  api=1.4.359  + 设备名带 (git-xxxxxxxx)/unknown
# 系统驱动： api=1.3     + Driver Build a97e8c1cc4…（高通字符串）
```

> 只看自己写的 `selected=turnip` 不够：adrenotools 即使加载失败也可能返回非 NULL 句柄（静默回落到系统 loader）。`NVN caps device … api=` 这一行才是硬证据。

---

## 2. 存档导入：把编年史一起带过来

### 问题

官方导出把两个**散文件**放在存档根目录：

```
ModernSharedStashSoftCoreV2.swvault   编年史仓库（约 6 KB）
sw_elysium.txt                        Elysium 角色名单
```

而 `SaveTransfer.importFrom` 只认**文件夹**（角色目录 / `ModernSharedStash*` / `Settings`），散文件直接跳过 —— 所以「存档导过来了，编年史是空的」。

### 修法（`dm/android/app/src/main/java/com/xiaoboluo/d2r/SaveTransfer.java`）

- 导入时也收 `*.swvault`；
- 也收 `sw_elysium.txt`，并且按**并集合并**（名单不是覆盖文件，两边角色名都保留）。

### 迁移步骤（官方包 → 本包，同一台手机）

```bash
# 1) 官方包导出：往游戏文件夹放 export 记号，打开官方 App，会在 save-export/<时间戳>/ 生成一份
adb shell "echo x > '/sdcard/Download/S2X/D2R Mobile/export'"

# 2) 把导出那一层复制成 save-import，打开本 App 自动导入
adb shell "rm -rf '/sdcard/Download/S2X/D2R Mobile/save-import'; \
           cp -r '/sdcard/Download/S2X/D2R Mobile/save-export/<时间戳>' '/sdcard/Download/S2X/D2R Mobile/save-import'"

# 3) 验证：应有角色目录 + 共享仓库 + *.swvault
adb shell run-as com.xiaoboluo.d2r.own ls -la files/save
```

导入完成后 `save-import` 会被改名为 `save-import.done-<时间戳>`；同名角色跳过，已有共享仓库走并回收件箱。

---

## 3. 触屏摇杆：满偏行程减半 + 开方曲线

### 问题

原来的映射是线性的，满偏要手指走 60 点（约 9–10 mm）：

```c
float radius = 60 * in_k;                       // 满偏行程
float strength = fminf(1, (distance - dead) / (radius - dead));
s->lx = ex * strength * DM_STICK_MAX;           // 游戏按幅度决定速度
```

推 15 点只有约 17% 速度——人物播着跑步动作却像慢镜头；反向时手指要从 +60 跨到 −60，等于 120 点，所以经常得抬手重按。

### 改成（`dm/ios/Sources/dm_touch_controls.c`，iOS 与安卓共用）

- 满偏行程 **60 → 30 点**；
- 叠加**开方曲线** `strength = sqrt((distance-dead)/(radius-dead))`：推一两毫米就开始动，30 点内到满速；
- 摇杆头改为按**手指真实位置**绘制（新增 `joy_px/joy_py`，经 `dm/android/touch_android.c` 传给绘制层），否则行程缩短后头会跑到手指前面；
- 可调：`DM_TOUCH_STICK_TRAVEL=<12–60>`（默认 30），写进 `files/dm/env` 重启即可，不必重编。

同步更新了两个会因映射变化而失效的单测：`dm/ios/tests/t_joy_travel.c`、`dm/android/tests/test_touch_snapshot.c`。

代价：极轻推时的精细度略降（贴墙、过门、站传送点边缘的可控范围变小）；嫌太灵敏可以把 `DM_TOUCH_STICK_TRAVEL` 调到 40。

---

## 4. 与官方并存安装

| 项 | 参数 | 说明 |
|---|---|---|
| 包名 | `-PdmAppId=com.xiaoboluo.d2r.own` | 与官方 `com.xiaoboluo.d2r` 不同，可同时装 |
| 显示名 | 默认与官方一致「D2R Mobile」 | 需要区分时传 `-PdmLabelSuffix=自编`；不传就是原版名（放包里默认不传） |
| 签名 | debug | 不能覆盖官方版；卸载会清 `files/save` 与 `files/dm`，先导出存档 |

> 两个图标的显示名都是「D2R Mobile」时只能靠位置区分；命令行核对包名：`adb shell pm list packages | grep xiaoboluo`（官方 `com.xiaoboluo.d2r`、本包 `com.xiaoboluo.d2r.own`）。

本机出包脚本：`build-apk-sbs.sh`（并排包 + 内置驱动 + 标签后缀）。注意必须用这个脚本，误用不带 `-PdmAppId` 的会出官方包名，安装时报 `INSTALL_FAILED_UPDATE_INCOMPATIBLE: signatures do not match`。

---

## 5. 升级与重放（官方出新版时）

```bash
# 1) 取上游新版源码
curl -sSL -o s.tar.gz https://codeload.github.com/jhaizhou-ops/d2r-mobile/tar.gz/refs/tags/v2.3.1
tar xzf s.tar.gz --strip-components=1 -C <dir>

# 2) 把本 fork 的改动打成一个补丁再套上去
git -C <fork> diff v2.3.0..android/turnip-and-stick -- dm/android dm/gfx/vulkan > driver-port.patch
cd <dir> && git apply --check driver-port.patch     # 必须干净通过

# 3) 重编
REPO=<dir> bash build-apk-sbs.sh
```

已验证：上面这个补丁在 **v2.3.1** 上 `git apply --check` 退出码 0（11 个文件全部干净套上），因为 2.3.1 没有动 `dm/host`、`dm/android`、`dm/gfx`、`dm/i18n`。

以后真正会随上游漂的只有「Vulkan 入口清单」这一类可机械重生的东西：引擎新增调用时，`gen_vk_shim.py` 重新生成 `vk_shim.c` 即可；漏掉的入口会在链接期直接报名字。

---

## English summary

This fork adds four things on top of upstream v2.3.0, all as "new files + a little wiring + one optional build flag":

1. **Runtime Vulkan driver selection** — 117 Vulkan entry points are taken over at link time (`vk_shim.c` generated from the engine's undefined symbols, `vk_runtime.c` picks the driver, Android no longer links `libvulkan` so a missed entry point fails the link). An optional bundled Mesa Turnip T30 is loaded through adrenotools; `DM_GPU_DRIVER=turnip` in `files/dm/env` enables it, `DM_GPU_DRIVER_REQUIRE=1` makes failures loud. Three silent-failure traps are documented above (trailing slash for the driver directory, the `libhardware.so` stub, and the env file living in the save work directory).
2. **Save import carries the chronicle** — the export writes `*.swvault` (chronicle vault) and `sw_elysium.txt` as loose files at the save root, while the importer used to look at folders only. Both are imported now, and the Elysium list is merged (union).
3. **Touch stick feel** — full deflection goes from 60 to 30 points of finger travel with a square-root curve, and the thumb is drawn from the real finger position. Tunable at runtime with `DM_TOUCH_STICK_TRAVEL` (12–60, default 30).
4. **Side-by-side install** — different application id and a launcher-name suffix, so it installs next to the official app.

No game content is included or distributed; see NOTICE.md.
