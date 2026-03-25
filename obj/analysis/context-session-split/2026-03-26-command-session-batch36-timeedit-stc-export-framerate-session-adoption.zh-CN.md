# batch36: timeedit / STC editor / export framerate helpers 的 session 收口

日期: 2026-03-26
分支: `exp-ui-lifecycle-safety`

## 本轮目标

继续推进编辑器辅助路径和只读/配置 helper，把以下文件中的 `Context` 直连访问收口到 `GetCore()`：

- `src/timeedit_ctrl.cpp`
- `src/subs_edit_ctrl_stc.cpp`
- `src/export_framerate.cpp`

这批价值在于：

- 它们不属于大 dialog，但都是字幕编辑和导出路径中的关键辅助组件
- 改动面不大、风险可控，却能把“project/selection/text selection 等 core 能力”表达得更明确

## 改动摘要

### 1. `timeedit_ctrl.cpp`

- `project->Timecodes()` 全部改走 `GetCore().project`

覆盖点：

- `GetFrame()`
- `SetFrame()`
- `SetByFrame()`
- `OnModified()`
- `UpdateText()`

这让 `TimeEdit` 更明确地表现为：UI 控件依赖 core session 提供的时间码模型。

### 2. `subs_edit_ctrl_stc.cpp`

- 活动行查询改走 `GetCore().selectionController`
- 文本选择同步改走 `GetCore().textSelectionController`

覆盖点：

- `UpdateStyle()`
- `SetTextTo()`

这有助于把 STC 控件看作“编辑器壳体”，而非直接耦合裸 `Context` 字段。

### 3. `export_framerate.cpp`

- `project->VideoProvider()` 与 `project->Timecodes()` 改走 `GetCore().project`

覆盖点：

- 配置界面里的 “From video” 填充
- `LoadSettings()` 的默认输入/输出 FPS 读取

## 架构收益

### 编辑器辅助件和 core 时间模型的边界更明确

- `TimeEdit` 和 `SubsStyledTextEditCtrl` 现在都更直观地表达出自己依赖的是：
  - `project`
  - `selectionController`
  - `textSelectionController`

这些都属于 core session，而不是任意裸 `Context` 成员。

### 对 future editor modernize 更有帮助

- 这类小控件往往分散在编辑主路径各处
- 先统一 session 归属，后续如果要继续抽 editor service、headless-compatible helper 或更清晰的 view-model 边界，会容易很多

## 性能影响

- 本轮没有改时间码换算算法、STC 样式刷新逻辑、帧率变换导出逻辑
- 没有新增额外分发或异步层
- 仅是访问路径重定向

因此性能影响可视为可忽略。

## 风险评估

### 低风险点

- 未改 `TimeEdit` 的输入模式、覆盖模式和格式化行为
- 未改 STC 的着色、拼写检查或 calltip 算法
- 未改帧率变换的计算逻辑

### 需要注意的点

- `textSelectionController` 目前仍被视作 core session 的一部分，但它和具体 UI 编辑控件仍存在天然耦合；未来如果进一步现代化 editor 架构，可能还需要重新评估其边界归属
- `export_framerate.cpp` 仍然是 UI 配置直接驱动 filter 对象，本轮没有继续拆配置模型与 UI 宿主

## 验证结果

### 构建

- `RelWithDebInfo` build: 通过
- `Release` build: 通过

### 测试

- `RelWithDebInfo` tests: `704` 运行，`702` 通过，`2` 跳过
- 跳过项与本轮无关：
  - `native_library.unix_suffixed_names_keep_dotnet_style_lib_prefix_variants`
  - `video_renderer_placebo_runtime.loads_runtime_when_available`

### 脚本/GUI 验证

- `tools/verify-ui-choice-decoupling-local.ps1 -SkipRelWithDebInfo`: 通过
- `RelWithDebInfo` GUI smoke: 通过
- `Release` GUI smoke: 通过

### 已知非阻塞问题

- 仍有 CMake dev warning:
  - `CMP0167 / FindBoost`

## 下一步建议

在当前这条线路上，接下来更适合评估的是：

1. `src/grid_column.cpp`
2. `src/dialog_style_editor.cpp`
3. 再审视 `src/audio_display.cpp` / `src/dialog_detached_video.cpp` 是否已经到了值得进入的整合窗口

其中：

- `grid_column.cpp` 主要是只读计算型访问，仍然比较适合低冲突推进
- `dialog_style_editor.cpp` 价值高，但比纯 helper 更复杂，适合单独批次
