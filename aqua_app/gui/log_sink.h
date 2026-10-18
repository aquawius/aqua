#ifndef AQUA_APP_GUI_LOG_SINK_H
#define AQUA_APP_GUI_LOG_SINK_H

// core 日志 → GUI 运行日志面板的桥。
//
// aqua::init_logger() 会把 spdlog 默认 logger 换成本平台的输出 sink（stdout /
// logcat，可选 tee 到文件）。GUI 在 init_logger() 之后调用本函数，为一个额外的
// sink 挂到同一个默认 logger 上——于是 core 的每一条日志同时出现在控制台和
// 运行日志面板里，级别仍由 aqua::set_log_level() 统一决定（sink 不做二次过滤）。
//
// 顺序要求：先 init_logger()，再 install_gui_log_sink()。init_logger 会整只替换
// 默认 logger，先挂 sink 会被丢掉。

namespace aqua::gui {

void install_gui_log_sink();

} // namespace aqua::gui

#endif // AQUA_APP_GUI_LOG_SINK_H
