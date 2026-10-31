# RHINE MUSIC DATABASE · 本地版（Local Edition）

**纯本地音乐播放器——基于 Rhine Music Database 的本地化改造分支**

这是对《明日方舟》特别映像「莱茵生命：访问」终端界面的非官方复刻（上游项目）的**本地版改造分支**：
在上游基线 [MikeQu223/Rhine-Music-Database](https://github.com/MikeQu223/Rhine-Music-Database)（基线 `aa1ba39`）之上，
**移除全部在线音乐服务与网络旁路**，只保留本地音乐播放、本地歌词与本地索引。使用 **Qt 6 / QML / C++** 构建。

![面板总览](Assets/previews/panel.png)

> 上图为上游版本的界面截图（`Assets/` 预览随上游保留）；本地版界面在大体一致的基础上去除了账号与在线来源相关元素。

## 与上游的差异

- **无任何在线服务**：网易云 / QQ / 酷狗三家在线来源、账号登录、在线搜索、在线封面与音频流、
  更新检查已整体移除；应用运行期间不发起任何网络请求（断网可完整使用）。
- **歌词完全本地化**：LRC 解析与编码探测改为 C++ 实现（UTF-8 / GBK / Big5 / Shift-JIS 字节级三层兜底，
  可手动指定编码重解）；支持同名 `.lrc` 侧车文件与手动导入歌词。
- **封面使用占位卡**：不再读取或下载任何封面图片。
- **移除 FidelityFX 渲染依赖与 Node 桥**：构建与分发不再包含 Node.js 运行时与音乐桥接子进程。
- 不保留任何编译开关——改动直接生效，在线能力没有「切回开关」。

## 功能列表

- [x] 本地音乐文件夹 / 文件导入（递归扫描，建立索引，音乐文件保留在原位置）
- [x] 本地检索、分组浏览（专辑 / 文件夹 / 歌手）
- [x] 本地播放、暂停、切歌、进度与音量；循环、随机、队列管理
- [x] 播放队列、播放历史与本地索引跨重启保留
- [x] 歌词：同名 `.lrc` 侧车自动加载 → 多编码字节级兜底 → 手动指定编码重解 → 手动导入
- [x] 桌面性能模式（后台服务管理播放，关闭到托盘音频不中断）

## 快速运行

当前分支只在 **Windows** 上构建与验证（CI 亦只跑 Windows 单平台）：

| 平台    | 程序                                                         |
| ------- | ------------------------------------------------------------ |
| Windows | `output/RhineMusicDatabase-<构建标识>-Windows-x64-Setup.exe` |

Windows 运行 `RhineMusicDatabase-<构建标识>-Windows-x64-Setup.exe` 安装程序。
Qt、MSVC 运行库与 FFmpeg 已随包提供，无需另行安装；**无需 Node.js**。

macOS / Linux 说明随上游 README（[上游仓库](https://github.com/MikeQu223/Rhine-Music-Database)）保留，
本分支不再随包提供对应产物。

## 从源码构建（Windows）

依赖：Qt 6.12.0（msvc2022_64，含 Shader Tools 与 Multimedia）、VS 2022 Build Tools、CMake 3.24+、Ninja、Python 3、Node.js（仅构建脚本使用）。

```sh
npm ci
npm run build        # 资源准备 + CMake 构建 + 部署，产物在 output/
ctest --test-dir output/qt-build --output-on-failure   # 回归测试（15 项）
```

构建默认关闭 FidelityFX（`RHINE_ENABLE_FIDELITYFX=OFF` 由构建脚本固定注入）。
`RHINE_VERSION=vX.Y.Z` 可指定正式版本串；未指定时以基线提交短 SHA 作为构建标识。

## 本机数据与隐私

本版本不连接任何音乐平台：没有账号、没有上传、没有遥测。
本地生成的数据只有音乐索引、歌词导入映射、播放历史与偏好设置，全部存放在本机应用数据目录；
卸载或删除对应目录即完全清除。

## 参考项目

- **[MikeQu223 / Rhine-Music-Database](https://github.com/MikeQu223/Rhine-Music-Database)**：本项目改造所基于的上游。
- **[LBEILC / RhineLabUI](https://github.com/LBEILC/RhineLabUI)**：特别感谢原界面、模型与场景作者，版权声明随项目保留。
- 上游的其他第三方依赖与许可说明见上游 README；本分支移除的在线服务相关依赖不再列举。

## 开源许可

本项目保留 [MIT License](LICENSE) 与原作者版权声明。第三方字体、Qt 和依赖分别遵循各自许可证；相关许可随部署包保留。

《明日方舟》、莱茵生命及其名称、标志与原作视觉设计的相关权利归各自权利人所有。本项目为非官方衍生作品，项目的 MIT 许可不代替第三方内容的授权。
