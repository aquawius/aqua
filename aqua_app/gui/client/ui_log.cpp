// 运行日志面板的 client 适配层。
//
// 真正的实现住在 common/ui_log_panel.cpp（client / server 共用一份）：面板只认
// LogView，不认 UiState。这里把 UiState 的四个日志视图字段搬进 LogView，画完再把
// 用户改动的两项（级别 / 自动滚动）写回——级别是要下发给 core 的，不能只活在
// 面板里。

#include "ui_panels.h"

#include "app_settings.h"
#include "ui_log_panel.h"
#include "ui_state.h"

namespace aqua::gui {

void draw_log_panel(UiState& s)
{
    LogView view;
    view.min_level = s.log_min_level;
    view.autoscroll = s.log_autoscroll;
    view.font_size = s.log_font_size;
    view.mono_font = s.font_mono;
    view.export_dir = data_dir();
    view.export_prefix = "aqua-client";
    view.empty_hint = "（暂无日志：连接服务器后这里会实时输出 core 的日志）";

    draw_log_panel(view);

    s.log_min_level = view.min_level;
    s.log_autoscroll = view.autoscroll;
}

} // namespace aqua::gui
