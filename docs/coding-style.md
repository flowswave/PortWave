# 编码与错误约定

- C++20，UTF-8，4 空格，以 `.clang-format` 为准。类型 PascalCase，函数 camelCase，成员尾部下划线。
- core 使用标准库、值对象与 RAII。校验失败返回有 error 字段的结果；不可表示的协议长度抛 invalid_argument，仅限调用前可验证的程序输入。
- Qt 后台服务用错误信号，不跨线程抛异常。不用阻塞 UI 的 waitForReadyRead、sleep、join 或 QThread::wait。
- QObject 使用 parent 所有权；编辑器以 unique_ptr 管理，QPointer 处理父 QWidget 先销毁的情况。
- 原始字节用 uint8_t/span；长度、序号先检查再窄化。UI 文本用 QString，边界处明确 UTF-8。
- 配置以稳定 machine ID 关联，显示名可改；用户错误必须有状态提示，不能无声成功。
- 所有共享队列有边界；性能声明必须附构建类型、环境、数据集和测量输出。
- 注释描述线程、所有权和约束，避免复述语句。

```powershell
./scripts/format.ps1
./scripts/format.ps1 -Check
./scripts/analyze.ps1 -Preset msvc-release
```

clang-tidy 的编译数据库必须与当前工具链一致；第三方代码的上游兼容警告记录，不修改其源码掩盖警告。
