# NOTICE — 这个仓库发布的 APK 里有什么、没有什么

## 不含游戏内容

发布的 APK **不包含任何《暗黑破坏神 II：重制版》的游戏内容**，也不包含由游戏内容派生的资源（美术、文字、音频、着色器、存档、数据表）。

- 游戏本体与更新包必须由使用者自己持有，并通过 App 自带的导入流程提供（`Download/S2X/D2R Mobile/`）。
- APK 的 `assets/` 里只有一个 Vulkan 驱动（见下），`res/` 里只有一张仓库自带的启动图标；打出来的 `libdm.so` 里没有任何内嵌图片（0 个 PNG 魔数），从游戏 RomFS 生成的头文件只有尺寸、名字、索引这类元数据。
- 上游 `CONTRIBUTING.zh-CN.md` 禁止把游戏内容及其派生物放进仓库；本仓库遵守同一条。

## 第三方组件与许可

| 组件 | 许可 | 用途 |
|---|---|---|
| [jhaizhou-ops/d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile) | MIT | 兼容层本体（本仓库是它的 fork） |
| Mesa / Turnip（`vulkan.purple.so`，T30 构建，26.3.0-devel / Vulkan 1.4.359） | MIT | 内置的 Adreno Vulkan 驱动 |
| [eden-emulator/libadrenotools](https://github.com/eden-emulator/libadrenotools) | BSD-2-Clause | 运行时加载自定义 Vulkan 驱动 |
| [bylaws/liblinkernsbypass](https://github.com/bylaws/liblinkernsbypass) | BSD-2-Clause | 链接器命名空间绕过（adrenotools 依赖） |

各组件版权归其原作者。Mesa 的 MIT 许可要求保留版权与许可声明；本仓库的 `LICENSE` 保留上游 MIT 声明，第三方组件的许可文本随源码一起分发。

## 与原版 APK 的关系

- 包名不同（`com.xiaoboluo.d2r.own`），因此可以与原版 APK 同时安装、各自独立存档。
- 用 debug 签名构建，**不能**覆盖安装官方版。
- 只由本仓库作者自用与分享，不是上游官方发布物；上游的发布以 `jhaizhou-ops/d2r-mobile` 的 Release 为准。
