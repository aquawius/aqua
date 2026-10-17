#ifndef AQUA_APP_GUI_UI_DEMO_H
#define AQUA_APP_GUI_UI_DEMO_H

// 演示数据（AQUA_GUI_DEMO=1）。Phase 0 专用：全部是拟真常量/函数，不发起
// 任何真实连接。会话层（ClientRuntime）接入后本文件整体删除，对应字段改由
// session 层驱动——所以这里刻意不依赖任何 core 头。

namespace aqua::gui {

// 往全局日志缓冲写几条演示日志（只在演示模式首次启动时调一次）。
void seed_demo_logs();

// 6 个指标的演示值：{ water_level, lead_ms, target_ms, jitter_ms, loss%, underrun_ratio }。
// 与 draw_metrics_strip 的列序一一对应。
void fill_demo_tiles(double (&v)[6]);

} // namespace aqua::gui

#endif // AQUA_APP_GUI_UI_DEMO_H
