# 模块导航

各目录对应一个 CMake target，保留功能归属，避免将协议、读写和持久化挤入主窗口。

| 模块 | 主要入口 | 负责什么 |
|---|---|---|
| app | main.cpp | 应用生命周期、命令行、Windows 资源 |
| ui | MainWindow / SettingsDialog / ThemeManager | 布局、绑定、设置暂存与主题快照 |
| features | TerminalPane / LogWorkspace / LogDocumentView / FindDialog | 用户工作流、窗格协调、日志文档状态 |
| editor | EditorInterface / makeEditor | GUI 线程编辑能力；QScintilla 实现不越过接口泄漏 |
| services | Session / FileService / SearchService | 异步任务门面、取消与结果交接 |
| infrastructure | SessionWorker / LogWriter / SettingsRepository | 串口/TCP 设备、磁盘线程、持久化与归档 |
| core | protocol / commands / ansi / BoundedQueue | 不依赖 Qt 的 C++20 算法与有界数据结构 |

业务依赖沿 `app → ui → features → services → infrastructure → core` 向下，features 通过 editor 的接口使用编辑器。头文件的公开接口说明线程、所有权和错误语义。第三方源码位于 `third_party/` 并保持原样。

新增功能先在所属模块复用现有接口；跨模块改变更新 [架构](../docs/architecture.md) 和 [ADR](../docs/decisions/)。构建、发布、清理和验证见 [根 README](../README.md)。
