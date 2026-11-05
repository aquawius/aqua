#ifndef AQUA_APP_GUI_SETTINGS_IO_H
#define AQUA_APP_GUI_SETTINGS_IO_H

// 设置文件的通用读写（client / server 共用）。
//
// 格式是最朴素的 key=value 文本（# 开头为注释），不用 JSON：这里只有几十个标量
// 字段，多一层 JSON 解析只会多一处可能崩的地方。**任何一行坏了都只跳过**，绝不
// 让"配置文件写坏"变成"程序起不来"。
//
// 落盘位置由各前端通过 app_id 决定：
//   SDL_GetPrefPath("aquawius", app_id)（Windows: %APPDATA%\aquawius\<app_id>\）。
//   取不到用户目录时退回 exe 同目录。

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace aqua::gui {

struct SettingEntry {
    std::string key;
    std::string value;
};

// 用户数据目录（**带尾部分隔符**）：settings.ini 与日志导出都放这里。
std::string settings_data_dir(const char* app_id);

// settings.ini 的完整路径（首次调用时向 SDL 查询并缓存）。
std::string settings_file_path(const char* app_id);

// 逐行读 key=value；坏行（没有 '=' / 空行 / 注释）直接跳过。文件不存在 = 空表。
std::vector<SettingEntry> read_settings(const std::string& path);

// 在表里找 key（找不到返回 nullptr）。
const std::string* find_setting(const std::vector<SettingEntry>& entries, std::string_view key);

// 严格解析：整数 / 浮点要求整串匹配；浮点**拒绝 nan / inf**（std::clamp(NaN)
// 恒返回 NaN，会一路污染下游，见 2026-10-20 的教训）。
bool parse_setting_int(const std::string& text, long& out);
bool parse_setting_double(const std::string& text, double& out);
// 接受 1/0、true/false、on/off。
bool parse_setting_bool(const std::string& text, bool& out);

// 按 **UTF-8 码点边界**截断后拷贝到定长缓冲（协议字段 / 设备名可能带中文）。
void copy_cstr_utf8(char* dst, std::size_t cap, const std::string& src);

// 原子写：先写 <path>.tmp，再替换目标。失败返回 false（已清理临时文件）。
bool write_settings_atomic(const std::string& path, const std::string& content);

} // namespace aqua::gui

#endif // AQUA_APP_GUI_SETTINGS_IO_H
