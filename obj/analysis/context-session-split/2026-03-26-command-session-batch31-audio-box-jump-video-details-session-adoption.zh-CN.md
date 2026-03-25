# batch31: audio box / jump to / video details 的 session 收口

日期: 2026-03-26
分支: `exp-ui-lifecycle-safety`

## 本轮目标

继续沿“低冲突优先”的路线推进，把一组音频容器层和轻量视频对话框的 `Context` 直连访问收口到 `GetCore()` / `GetUI()`：

- `src/audio_box.cpp`
- `src/dialog_jumpto.cpp`
- `src/dialog_video_details.cpp`

这批的选择原因：

- `audio_box.cpp` 是音频 UI 链路上的上层容器，架构价值高，但还没有直接碰到 `audio_display.cpp` 那类和 `exp-perf` 明确重叠的深热点
- `dialog_jumpto.cpp`、`dialog_video_details.cpp` 是低风险视频入口，适合继续把父窗口 / 视频 provider / controller 访问显式分层
- 三者合在一起能继续压缩 `Context` 裸字段直连，同时把 future core shell / UI shell 的边界表达得更清楚

## 改动摘要

### 1. `audio_box.cpp`

- `audioController` 改走 `GetCore()`
- `karaoke` UI 组件改走 `GetUI()`

覆盖点：

- 构造函数中的 controller 注入与 audio open listener 订阅
- `AudioDisplay` 构造注入
- karaoke bar 创建、sizer show/hide
- `OnSashDrag()`
- `ShowKaraokeBar()`

这使 `AudioBox` 的职责更清晰：

- 音频播放 / 控制能力属于 core session
- karaoke bar 这种可视控件属于 UI session

### 2. `dialog_jumpto.cpp`

- 对话框父窗口改走 `GetUI().parent`
- 当前帧、总帧数、时间帧互转、跳转动作改走 `GetCore()`

覆盖点：

- 构造期初始 frame/time 填充
- `OnOK()`
- `OnEditTime()`
- `OnEditFrame()`

这让 dialog 本身只保留 UI 宿主含义，视频状态与导航逻辑显式留在 core session。

### 3. `dialog_video_details.cpp`

- 对话框父窗口改走 `GetUI().parent`
- 视频 provider 与视频名改走 `GetCore().project`

覆盖点：

- dialog 构造期 provider 读取
- 文件名、尺寸、FPS、时长等只读字段展示

这个文件虽然简单，但很适合作为“只读型视频信息面板”的样板，后续做 CLI/headless 只读能力时更容易抽出无 wx 的 core summary。

## 架构收益

### 音频 UI 容器首次更明确地区分 core 与 UI 归属

- `AudioBox` 不再把音频控制器和 karaoke 可视控件都挂成同一层 `Context` 直连字段
- 这为后续继续处理 `audio_box` 周边桥接逻辑、再进入更重的 `audio_display.cpp` 之前，先建立稳定的 session 使用习惯

### 视频对话框开始形成统一模式

- 父窗口、dialog manager 等宿主元素统一归入 `GetUI()`
- provider、controller、project 访问统一归入 `GetCore()`

这能减少后面继续扫视频类 dialog 时的风格漂移，也能降低未来大合并时“同一语义被不同文件用不同写法表达”的冲突噪声。

## 性能影响

- 本轮没有引入新的 service、额外 dispatch 或异步层
- `audio_box` 的 sash 拖动和 karaoke show/hide 仍是等价重路由
- `dialog_jumpto` 的 frame/time 更新路径仍直接走 controller，不增加额外拷贝或状态同步

因此本轮性能影响可视为可忽略，主要收益仍是边界清晰度和后续可维护性。

## 风险评估

### 低风险点

- 没有改音频波形绘制、缓存、分析和后台任务逻辑
- 没有改视频 provider 行为，也没有改跳转算法
- 两个 dialog 都只是在 session 归属层面做访问重定向，未改交互流程

### 需要注意的点

- `audio_box.cpp` 仍持有 `context` 并向更深层 UI 链路传递，后续若继续清理音频层，要避免在 `audio_display.cpp` 上和 perf 线直接撞车
- `dialog_jumpto.cpp` 里 frame/time 双向更新仍依赖现有 controller 语义，未来若改视频 session API，要保持这两个入口的行为一致

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

当前继续推进、同时尽量控制未来合并冲突时，优先级建议仍是：

1. `dialog_style_manager.cpp`
2. `subs_edit_box.cpp`
3. 继续评估 `audio_box` 周边残留热点，但先避免直接深入 `audio_display.cpp`

其中：

- `dialog_style_manager.cpp` 的价值在于 style 编辑链路同时接触字幕数据与 UI 宿主，适合继续磨出稳定模式
- `subs_edit_box.cpp` 对 core/ui 边界价值更高，但复杂度也更高，适合在 style manager 之后处理
- `audio_display.cpp` 仍建议暂缓，等与 `exp` / `exp-perf` 的整合窗口更合适时再进入
