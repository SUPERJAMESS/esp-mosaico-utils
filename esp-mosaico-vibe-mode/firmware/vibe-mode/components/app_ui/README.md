# app_ui

管理 Vibe 自有轻量绘制器、设备显示、输入、截图及渲染调度；不依赖现成 UI 框架。
`ui_services` 将业务快照转换为界面模型，`vibe_ui` 是设备和 PC 共用的不透明控制器。
`light/vui.c` 与 `vui_decode.c` 使用同一资源包在两个平台运行。

PC 由 `pc/CMakeLists.txt` 将绘制器和页面控制器编译为本机动态库；
`pc/simulator.py` 通过 ctypes 调用它，把 RGB565 帧显示到浏览器 Canvas，
鼠标输入回传同一套 C 输入处理。`pc/platform_pc.c` 提供硬件服务的模拟快照。
它不模拟 ESP32 CPU，也不运行设备固件二进制；布局和交互可以共用，
联网、Flash 写入、DMA 和面板时序仍需实机验证。

首页点阵由 `src/vibe_home_motion.c` 保存 49 个圆点的位置、半径、RGB 和亮度，
通过关键帧平滑插值生成入场组装、呼吸、斜向扫描及点击涟漪。绘制器的 `dots`
节点直接以亚像素边缘覆盖率绘制圆点，不再生成或解压首页位图动画帧。
40 ms 定时器按实际经过的毫秒数采样，仅在首页更新 120×120 区域；
离开首页（包括休眠）停止采样，返回时重播入场。点击从当前点状态开始涟漪，
不会截获页面滑动或阻塞导航。点状态与绘制器副本复用，无逐帧内存分配。

设置与显示页使用带 `scroll` / `content_height` 的容器；视口裁剪、绘制和点击
共用一个滚动偏移，标题栏不移动。与 Wi-Fi/Idea 列表共用拖动及定时减速逻辑；
点移动中的内容停止滚动，固定标题栏按钮仍立即响应。

应用图标直接显示打开确认框，不等待 NAND 扫描或后台更新检查。忙碌期间显示
等待状态且不接纳写入；扫描完成后刷新未保存开发版本提醒，用户需明确确认。
真正打开前重新读取状态，保留文件校验、低电量保护和写入互斥。

公共业务依赖为 app_settings、app_device；私有适配依赖 app_network、app_ideas、
app_nand_update、app_system_update、app_bridge 和 BSP / display presenter / ESP-Iris 公共 API。
所有控制器与渲染操作由单个渲染任务串行执行；外部请求排队，旋转在帧间持锁执行。
资源复制、PNG 和 Canvas 的所有权规则见 `include/vui.h`；解码失败不接收调用方资源所有权。

设备入口为 `factory_ui_start()`；PC 使用 `vui_create()` 后调用 `vibe_ui_create()`。
生命周期结束先 `vibe_ui_deinit()` 再 `vui_destroy()`。渲染上下文不得与销毁并发。
