#ifndef AQUA_APP_GUI_UI_PREFS_H
#define AQUA_APP_GUI_UI_PREFS_H

// 两个前端共用的显示偏好枚举/文案（目前只有刷新率模式）。
//
// 放在公共层的原因很实际：设置里的档位名与 main 循环里的帧率映射必须一一对应，
// 两个前端各抄一份的话，改档位时漏改一处就会出现"设置写着 30 fps、实际跑 60"。

namespace aqua::gui {

// 刷新率模式（设置 → 外观）：主动期的帧率 + 静止期的帧率。
//   0 自适应：有输入时 60 fps，静止 1.5 s 后 15 fps（默认，最省电）
//   1 60 fps / 2 30 fps / 3 15 fps：固定值，不再区分动静
//   4 不限：只靠 vsync（高刷屏上会跑满刷新率，最耗电）
inline constexpr int kFrameRateModeCount = 5;
inline constexpr const char* kFrameRateModeNames[kFrameRateModeCount] = {
    "自适应（60 / 15）", "固定 60 fps", "固定 30 fps", "固定 15 fps", "不限（跟随显示器）",
};

} // namespace aqua::gui

#endif // AQUA_APP_GUI_UI_PREFS_H
