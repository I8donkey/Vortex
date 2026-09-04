// ============================================================
// game2d_module.h — 2D 游戏模块（参考 Pygame Zero 简化接口）
// 包含 Window（窗口）、Sprite（精灵）、Rect、Image、Sound、
// 键盘/鼠标状态查询，以及 AABB 碰撞检测。
//
// 实现策略：
//   * 启用 SDL2（VORTEX_WITH_SDL2）时使用真实硬件加速窗口
//   * 未启用时走 "headless 模拟器"：所有操作纯 CPU 模拟，
//     用于离线测试或 CI 环境，保证脚本层 API 一致。
// ============================================================
#ifndef VORTEX_GAME2D_MODULE_H
#define VORTEX_GAME2D_MODULE_H

#include "value.h"
#include <unordered_map>
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <array>

namespace vortex {

// 提前声明主解释器，避免与 game2d 名字空间中 class Interpreter* 参数的前向声明歧义
class Interpreter;

namespace game2d {

// ---------- 简单 RGBA 像素图 ----------
struct Image {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> pixels; // RGBA, stride=4*width

    Image() = default;
    Image(int w, int h) : width(w), height(h), pixels((size_t)w * h * 4, 0) {}

    void fill(int r, int g, int b, int a = 255);
    void set_pixel(int x, int y, int r, int g, int b, int a = 255);
    void get_pixel(int x, int y, int& r, int& g, int& b, int& a) const;
    // 生成测试图像
    static std::shared_ptr<Image> make_checker(int w, int h, int tile = 32);
    static std::shared_ptr<Image> make_solid(int w, int h, int r, int g, int b, int a = 255);
};
using ImagePtr = std::shared_ptr<Image>;

// ---------- 精灵 ----------
struct Sprite {
    std::string name;
    ImagePtr image;
    double x = 0, y = 0;        // 左上角位置
    double angle = 0;           // 旋转角度（度）
    double scale = 1;           // 缩放
    int anchor_x = 0;           // 锚点（相对图像左上角偏移）
    int anchor_y = 0;
    double  opacity = 1.0;      // 0~1
    bool    visible = true;

    // 碰撞盒（AABB，按图像尺寸+位置计算）
    double width()  const { return image ? image->width  * scale : 0; }
    double height() const { return image ? image->height * scale : 0; }
    double left()   const { return x - anchor_x * scale; }
    double top()    const { return y - anchor_y * scale; }
    double right()  const { return left() + width(); }
    double bottom() const { return top()  + height(); }

    bool collides_rect(const Sprite& o) const;
    bool collides_point(double px, double py) const;
};
using SpritePtr = std::shared_ptr<Sprite>;

// ---------- 音频（占位，headless 保持 API） ----------
struct Sound {
    std::string path;
    double duration = 0;
    bool   loaded = false;
};
using SoundPtr = std::shared_ptr<Sound>;

// ---------- 键盘/鼠标快照 ----------
struct InputState {
    std::unordered_set<int> keys_down;   // SDL keycode 或 ASCII
    std::unordered_set<std::string> key_names; // 如 "left", "space", "a"
    double mouse_x = 0, mouse_y = 0;
    bool mouse_left = false;
    bool mouse_middle = false;
    bool mouse_right = false;
    int  mouse_wheel = 0;
};

// ---------- 音乐（占位） ----------
struct MusicState {
    std::string current;
    bool playing = false;
    double volume = 1.0;
};

// ---------- 全局游戏状态 ----------
struct Game {
    bool   sdl_active = false;    // 是否使用真实 SDL2 窗口
    bool   running = false;
    int    width = 800;
    int    height = 600;
    std::string title = "Vortex Game2D";
    ImagePtr framebuffer;         // headless 模式：屏幕画布（或 SDL 回读镜像）

    std::vector<SpritePtr> sprites;
    std::unordered_map<std::string, ImagePtr> images;
    std::unordered_map<std::string, SoundPtr> sounds;
    InputState input;
    MusicState music;

    double time = 0;              // 秒，累计
    double dt = 0.016;            // 上一帧时长
    int    fps = 60;

    // 用户回调名称（在解释器全局作用域中查找并调用）
    bool has_update_fn = false;
    bool has_draw_fn = false;

    // 单例
    static Game& instance();

    // 与主解释器关联：让 run() 能调用用户的 update/draw 脚本函数
    static void bind_interpreter(vortex::Interpreter* interp);
    static vortex::Interpreter* get_interpreter();

    void ensure_window(int w, int h, const std::string& t);
    void close_window();

    // 每帧步骤：
    //   step_begin — 处理事件、推进时间、调用 update(dt)
    //   step_render — 清屏、调用 draw()、翻转 buffer
    bool step_begin(vortex::Interpreter* interp);
    bool step_render(vortex::Interpreter* interp);

    // 主循环入口：循环调用 step_begin / step_render 直到退出
    void run_loop(vortex::Interpreter* interp, int max_frames = 0);

    // ===== 绘图 API（直接画到 framebuffer / SDL 渲染器） =====
    void clear(int r, int g, int b);
    void draw_rect(int x, int y, int w, int h, int r, int g, int b, int a = 255, bool fill = true);
    void draw_circle(int cx, int cy, int radius, int r, int g, int b, int a = 255, bool fill = true);
    void draw_line(int x1, int y1, int x2, int y2, int r, int g, int b, int a = 255, int thickness = 1);
    void draw_text(const std::string& text, int x, int y,
                   int r = 255, int g = 255, int b = 255, int a = 255,
                   int size = 16, const std::string& font = "mono");
    void draw_image(const Image* img, int x, int y, double angle_deg = 0, double scale = 1, double opacity = 1.0);
    void draw_sprite(const Sprite* s);

    // 获取按键状态
    bool key_down(const std::string& name) const;
    bool key_down_code(int code) const;
};

} // namespace game2d

void register_game2d_module(std::unordered_map<std::string, ValuePtr>& std_modules);

} // namespace vortex
#endif
