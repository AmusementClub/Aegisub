# batch34: export / search replace / selection dialogs 的 session 收口

日期: 2026-03-26
分支: `exp-ui-lifecycle-safety`

## 本轮目标

继续沿低冲突 dialog 入口推进，把三类常用对话框中的 `Context` 直连字段收口到 `GetCore()` / `GetUI()`：

- `src/dialog_export.cpp`
- `src/dialog_search_replace.cpp`
- `src/dialog_selection.cpp`

这批的共同特点：

- 都是上层 dialog 壳体，行为相对独立
- 涉及的数据面清晰，适合标准化成“父窗口 / dialog manager 属于 UI，字幕/搜索/选择状态属于 core”
- 不触碰 renderer、audio/video 深链路，因此与 `exp` / `exp-perf` 的冲突风险较低

## 改动摘要

### 1. `dialog_export.cpp`

- 父窗口改走 `GetUI().parent`
- 导出过滤器与编码设置持久字段改走 `GetCore().ass->Properties`

覆盖点：

- 构造期 export filter 恢复
- 导出编码选择恢复
- 对话框析构时过滤器列表回写
- `OnProcess()` 中导出编码回写

### 2. `dialog_search_replace.cpp`

- 父窗口改走 `GetUI().parent`
- 搜索/替换执行器改走 `GetCore().search`

覆盖点：

- 构造期 dialog 宿主
- `FindReplace()` 中 `Configure()` 与执行 `FindNext/ReplaceNext/ReplaceAll`

### 3. `dialog_selection.cpp`

- 父窗口改走 `GetUI().parent`
- `ShowSelectLinesDialog()` 改走 `GetUI().dialog`
- 选择处理逻辑中的 `ass`、`selectionController` 改走 `GetCore()`

覆盖点：

- 匹配集构建时的字幕事件来源
- 原选中集读取
- 活动行读取与更新
- 最终 `SetSelectionAndActive()`

## 架构收益

### dialog 入口层的归属模式进一步统一

- 父窗口与 dialog manager 越来越稳定地统一走 `GetUI()`
- 字幕数据、搜索执行器、选中集状态统一走 `GetCore()`

这能继续降低后续扫描其他 dialog 时的风格漂移和心智负担。

### 对 future CLI / headless 边界有间接收益

- `dialog_selection.cpp` 里的“匹配并计算新选中集”逻辑，经过这轮后更明确依赖的是 core 层选择和字幕数据
- `dialog_search_replace.cpp` 也更清楚地体现出 UI 壳体只是收集参数，真正的执行器在 core session

虽然这还不是无 wx 下沉，但会让未来继续 service 化或抽只读/只算逻辑容易很多。

## 性能影响

- 本轮没有改变导出、搜索匹配、选中集集合运算的算法
- 没有引入新的异步层、缓存或额外 dispatch
- 只是把访问路径改成显式 session 归属

因此性能影响可视为可忽略。

## 风险评估

### 低风险点

- 未改导出文件生成逻辑和 exporter 行为
- 未改搜索匹配配置项和 MRU 更新逻辑
- 未改选择行的集合运算语义

### 需要注意的点

- `dialog_export.cpp` 仍然把 UI 生命周期和 `ass->Properties` 持久字段写回绑在一起，未来如果继续现代化，可能值得再拆“导出设置模型”与 UI 壳体
- `dialog_search_replace.cpp` 和 `dialog_selection.cpp` 还没有把底层操作抽成更细的 service，只是先明确 core/ui 归属

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

继续按低冲突价值优先时，下一批更值得看的是：

1. `src/dialog_fonts_collector.cpp`
2. `src/dialog_automation.cpp`
3. `src/dialog_log.cpp`

随后再评估：

- `src/timeedit_ctrl.cpp`
- `src/grid_column.cpp`

仍建议暂缓：

- `src/audio_display.cpp`
- `src/dialog_detached_video.cpp`
- `src/video_display.cpp`

因为它们与视频/音频深链路和 `exp-perf` 的重叠面仍然更大。
