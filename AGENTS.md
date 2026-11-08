# PortWave 协作规则

修改前阅读本文件、`docs/architecture.md` 和 `docs/parity.md`，先确定功能所属模块。

1. 复用现有接口；core 只依赖标准 C++20，不得引用 Qt 或 UI。
2. 依赖方向为 app → ui → features → services → infrastructure → core；editor 仅通过 EditorInterface 提供编辑能力。
3. MainWindow 只做布局、绑定和窗口协调，协议、文件解析、持久化不得放入其中。
4. 公共接口必须说明职责、线程归属、生命周期和错误约定；修改后搜索全部调用方。
5. QWidget 仅在 GUI 线程操作，后台耗时工作必须可取消；关闭通过异步 stopped 信号完成，禁止 GUI wait()/join()。
6. 接收与发送队列必须有容量边界和可观察的丢失/拒绝策略。高频数据禁止逐条 queued signal 刷新 UI。
7. 新增大型依赖需要明确理由，不随意引入；依赖集中管理并同步版本、来源、SHA256 与许可证。
8. 第三方源码保持原样，不通过删除测试、屏蔽项目警告或修改依赖来掩盖问题。
9. 修 Bug 优先增加有效回归测试，不为简单可逆的样式调整创建镜像实现的测试。
10. 架构改变同步更新 architecture 和 docs/decisions 中的 ADR。
11. 采用最小可维护实现，避免空类、空模块和无测量的性能优化。
12. 统一 UTF-8、clang-format 和现有命名、目录风格。
13. 主题改变仅更新视图；不得重连、重读文件、改变发送草稿/目标或开启自动发送。
14. 不在日志记录 RX/TX 正文、剪贴板内容、固件内容或凭据；诊断文件必须分卷限量。
15. 配置与旧 Python 版隔离，不修改参考源码或其用户设置。
16. 任务完成报告修改文件、设计、真实构建/测试结果与遗留项，不得将“代码存在”写成“已实机验收”。
17. Windows 工具链统一为 MSVC v143 + Qt msvc2022_64，默认共享位置为 C:\Program Files\PortWaveToolchain；不在工程中携带 SDK，不混用 MinGW DLL。
18. 使用 scripts/configure.ps1、build.ps1、test.ps1、format.ps1；UI 改动检查真实运行截图，不能将设计稿当作验收。

本项目不自动初始化 Git/SVN，也不自动提交；用户自行管理版本。
