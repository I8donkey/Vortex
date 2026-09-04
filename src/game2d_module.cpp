// ============================================================
// game2d_module.cpp — 实现。当前使用 headless 软件渲染版本，
// 保证所有 Vortex 脚本调用均可执行；若启用 SDL2 可在
// ensure_window / flip 等函数中直接调用 SDL2 函数并保持
// 外部 API 不变。
// ============================================================
#include "game2d_module.h"
#include "interpreter.h"
#include <cmath>
#include <algorithm>
#include <chrono>
#include <thread>
#include <sstream>
#include <cstring>

namespace vortex {
namespace game2d {

// ---------- Image ----------
void Image::fill(int r, int g, int b, int a) {
    for (size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i]   = (unsigned char)std::clamp(r, 0, 255);
        pixels[i+1] = (unsigned char)std::clamp(g, 0, 255);
        pixels[i+2] = (unsigned char)std::clamp(b, 0, 255);
        pixels[i+3] = (unsigned char)std::clamp(a, 0, 255);
    }
}
void Image::set_pixel(int x, int y, int r, int g, int b, int a) {
    if (x < 0 || x >= width || y < 0 || y >= height) return;
    size_t off = ((size_t)y * width + x) * 4;
    pixels[off]   = (unsigned char)std::clamp(r, 0, 255);
    pixels[off+1] = (unsigned char)std::clamp(g, 0, 255);
    pixels[off+2] = (unsigned char)std::clamp(b, 0, 255);
    pixels[off+3] = (unsigned char)std::clamp(a, 0, 255);
}
void Image::get_pixel(int x, int y, int& r, int& g, int& b, int& a) const {
    if (x < 0 || x >= width || y < 0 || y >= height) { r = g = b = a = 0; return; }
    size_t off = ((size_t)y * width + x) * 4;
    r = pixels[off]; g = pixels[off+1]; b = pixels[off+2]; a = pixels[off+3];
}
ImagePtr Image::make_checker(int w, int h, int tile) {
    auto p = std::make_shared<Image>(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            bool d = ((x / tile) + (y / tile)) % 2 == 0;
            if (d) p->set_pixel(x, y, 0x22, 0x22, 0x22);
            else   p->set_pixel(x, y, 0xCC, 0xCC, 0xCC);
        }
    return p;
}
ImagePtr Image::make_solid(int w, int h, int r, int g, int b, int a) {
    auto p = std::make_shared<Image>(w, h);
    p->fill(r, g, b, a);
    return p;
}

// ---------- Sprite ----------
static inline bool overlap(double a0, double a1, double b0, double b1) {
    return a0 < b1 && b0 < a1;
}
bool Sprite::collides_rect(const Sprite& o) const {
    if (!visible || !o.visible) return false;
    return overlap(left(), right(), o.left(), o.right())
        && overlap(top(),  bottom(), o.top(),  o.bottom());
}
bool Sprite::collides_point(double px, double py) const {
    if (!visible) return false;
    return px >= left() && px < right() && py >= top() && py < bottom();
}

// ---------- Game ----------
Game& Game::instance() {
    static Game g;
    return g;
}

// 与主解释器绑定：run() 内需要通过解释器执行用户的 update/draw 脚本函数
static vortex::Interpreter* s_active_interpreter = nullptr;
void Game::bind_interpreter(vortex::Interpreter* interp) { s_active_interpreter = interp; }
vortex::Interpreter* Game::get_interpreter() { return s_active_interpreter; }
void Game::ensure_window(int w, int h, const std::string& t) {
    width = w; height = h; title = t;
    if (!framebuffer || framebuffer->width != w || framebuffer->height != h)
        framebuffer = std::make_shared<Image>(w, h);
    framebuffer->fill(0, 0, 0);
    running = true;
    // 此处启用 SDL2 的话可 SDL_Init / SDL_CreateWindow。
}
void Game::close_window() {
    running = false;
}

// ---------- 帧调度 ----------
bool Game::step_begin(vortex::Interpreter* interp) {
    if (!running) return false;
    using clock = std::chrono::steady_clock;
    static auto last = clock::now();
    auto now = clock::now();
    dt = std::chrono::duration<double>(now - last).count();
    if (dt <= 0) dt = 1.0 / fps;
    last = now;
    time += dt;

    // 模拟：偶尔按下 ESC 的条件不触发（headless 不输入）
    // 用户脚本可通过 game2d.set_key / set_mouse 注入事件

    if (has_update_fn && interp) {
        try {
            std::ostringstream src;
            src << "update(" << dt << ");";
            interp->exec_source(src.str());
        } catch (RuntimeError&) {}
    }
    return true;
}

bool Game::step_render(vortex::Interpreter* interp) {
    if (!running) return false;
    if (has_draw_fn && interp) {
        try {
            interp->exec_source("draw();");
        } catch (RuntimeError&) {}
    }
    // headless 不翻转到屏幕；SDL2 版本在此 RenderPresent
    if (fps > 0) {
        double frame_dt = 1.0 / fps;
        double sleep_for = frame_dt - dt;
        if (sleep_for > 0)
            std::this_thread::sleep_for(std::chrono::duration<double>(sleep_for));
    }
    return true;
}

void Game::run_loop(vortex::Interpreter* interp, int max_frames) {
    ensure_window(width, height, title);
    int frames = 0;
    auto check_fn = [&](const char* nm) {
        try { (void)interp->globals().lookup(nm); return true; }
        catch (...) { return false; }
    };
    has_update_fn = check_fn("update");
    has_draw_fn   = check_fn("draw");
    while (running) {
        if (!step_begin(interp)) break;
        if (!step_render(interp)) break;
        if (max_frames > 0 && ++frames >= max_frames) break;
    }
}

// ---------- 绘图（软件渲染到 framebuffer） ----------
void Game::clear(int r, int g, int b) {
    if (!framebuffer) ensure_window(width, height, title);
    framebuffer->fill(r, g, b);
}
void Game::draw_rect(int x, int y, int w, int h, int r, int g, int b, int a, bool fill) {
    if (!framebuffer) return;
    int x0 = std::clamp(x, 0, framebuffer->width - 1);
    int y0 = std::clamp(y, 0, framebuffer->height - 1);
    int x1 = std::clamp(x + w - 1, 0, framebuffer->width - 1);
    int y1 = std::clamp(y + h - 1, 0, framebuffer->height - 1);
    if (x0 > x1 || y0 > y1) return;
    if (fill) {
        for (int yy = y0; yy <= y1; ++yy)
            for (int xx = x0; xx <= x1; ++xx)
                framebuffer->set_pixel(xx, yy, r, g, b, a);
    } else {
        for (int xx = x0; xx <= x1; ++xx) { framebuffer->set_pixel(xx, y0, r,g,b,a); framebuffer->set_pixel(xx, y1, r,g,b,a); }
        for (int yy = y0; yy <= y1; ++yy) { framebuffer->set_pixel(x0, yy, r,g,b,a); framebuffer->set_pixel(x1, yy, r,g,b,a); }
    }
}
void Game::draw_circle(int cx, int cy, int radius, int r, int g, int b, int a, bool fill) {
    if (!framebuffer) return;
    int rr = radius * radius;
    if (fill) {
        for (int y = cy - radius; y <= cy + radius; ++y) {
            for (int x = cx - radius; x <= cx + radius; ++x) {
                int dx = x - cx, dy = y - cy;
                if (dx * dx + dy * dy <= rr) framebuffer->set_pixel(x, y, r, g, b, a);
            }
        }
    } else {
        // Bresenham-like
        for (int t = 0; t < 360; ++t) {
            double th = t * 3.1415926535 / 180.0;
            int x = (int)std::round(cx + std::cos(th) * radius);
            int y = (int)std::round(cy + std::sin(th) * radius);
            framebuffer->set_pixel(x, y, r, g, b, a);
        }
    }
}
void Game::draw_line(int x1, int y1, int x2, int y2, int r, int g, int b, int a, int thickness) {
    if (!framebuffer) return;
    int dx = std::abs(x2 - x1), sx = x1 < x2 ? 1 : -1;
    int dy = -std::abs(y2 - y1), sy = y1 < y2 ? 1 : -1;
    int err = dx + dy;
    int x = x1, y = y1;
    while (true) {
        for (int ty = -thickness/2; ty <= thickness/2; ++ty)
            for (int tx = -thickness/2; tx <= thickness/2; ++tx)
                framebuffer->set_pixel(x+tx, y+ty, r, g, b, a);
        if (x == x2 && y == y2) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x += sx; }
        if (e2 <= dx) { err += dx; y += sy; }
    }
}
void Game::draw_text(const std::string& text, int x, int y, int r, int g, int b, int a, int size, const std::string&) {
    if (!framebuffer) return;
    // 极简 5x7 点阵字体（仅可打印 ASCII），用于 headless 文本显示
    static const unsigned char font5x7[96][5] = {
        {0,0,0,0,0},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},
        {0x14,0x7F,0x14,0x7F,0x14},{0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},
        {0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},{0x00,0x1C,0x22,0x41,0x00},
        {0x00,0x41,0x22,0x1C,0x00},{0x08,0x2A,0x1C,0x2A,0x08},{0x08,0x08,0x3E,0x08,0x08},
        {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},
        {0x20,0x10,0x08,0x04,0x02},{0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},
        {0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},{0x18,0x14,0x12,0x7F,0x10},
        {0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
        {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},{0x00,0x36,0x36,0x00,0x00},
        {0x00,0x56,0x36,0x00,0x00},{0x00,0x08,0x14,0x22,0x41},{0x14,0x14,0x14,0x14,0x14},
        {0x41,0x22,0x14,0x08,0x00},{0x02,0x01,0x51,0x09,0x06},{0x32,0x49,0x79,0x41,0x3E},
        {0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
        {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x01,0x01},
        {0x3E,0x41,0x41,0x51,0x32},{0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},
        {0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},{0x7F,0x40,0x40,0x40,0x40},
        {0x7F,0x02,0x04,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
        {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},
        {0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},
        {0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},{0x63,0x14,0x08,0x14,0x63},
        {0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},{0x00,0x00,0x7F,0x41,0x41},
        {0x02,0x04,0x08,0x10,0x20},{0x41,0x41,0x7F,0x00,0x00},{0x04,0x02,0x01,0x02,0x04},
        {0x40,0x40,0x40,0x40,0x40},{0x00,0x00,0x01,0x02,0x04},{0x20,0x54,0x54,0x54,0x78},
        {0x7F,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20},{0x38,0x44,0x44,0x48,0x7F},
        {0x38,0x54,0x54,0x54,0x18},{0x08,0x7E,0x09,0x01,0x02},{0x08,0x14,0x54,0x54,0x3C},
        {0x7F,0x08,0x04,0x04,0x78},{0x00,0x44,0x7D,0x40,0x00},{0x20,0x40,0x44,0x3D,0x00},
        {0x00,0x41,0x7F,0x40,0x00},{0x7C,0x04,0x18,0x04,0x78},{0x7C,0x08,0x04,0x04,0x78},
        {0x38,0x44,0x44,0x44,0x38},{0x7C,0x14,0x14,0x14,0x08},{0x08,0x14,0x14,0x18,0x7C},
        {0x7C,0x08,0x04,0x04,0x08},{0x48,0x54,0x54,0x54,0x20},{0x04,0x3F,0x44,0x40,0x20},
        {0x3C,0x40,0x40,0x20,0x7C},{0x1C,0x20,0x40,0x20,0x1C},{0x3C,0x40,0x30,0x40,0x3C},
        {0x44,0x28,0x10,0x28,0x44},{0x0C,0x50,0x50,0x50,0x3C},{0x44,0x64,0x54,0x4C,0x44}
    };
    const int cw = 5, ch = 7;
    double scale = std::max(1.0, (double)size / 7.0);
    int cx = x;
    for (char ch0 : text) {
        unsigned char ch = (unsigned char)ch0;
        if (ch < 32 || ch > 127) ch = 32;
        const unsigned char* glyph = font5x7[ch - 32];
        for (int row = 0; row < ch; ++row) {
            for (int col = 0; col < cw; ++col) {
                if (glyph[col] & (1 << row)) {
                    for (int sy = 0; sy < (int)scale; ++sy)
                        for (int sx = 0; sx < (int)scale; ++sx)
                            framebuffer->set_pixel(cx + col*(int)scale + sx,
                                                   y + row*(int)scale + sy,
                                                   r, g, b, a);
                }
            }
        }
        cx += (int)((cw + 1) * scale);
    }
}
void Game::draw_image(const Image* img, int x, int y, double angle_deg, double scale, double opacity) {
    if (!img || !framebuffer) return;
    // 最近邻采样旋转+缩放+位移
    double rad = angle_deg * 3.1415926535 / 180.0;
    double cs = std::cos(rad), sn = std::sin(rad);
    int iw = img->width, ih = img->height;
    int cx0 = iw / 2, cy0 = ih / 2;
    int bnd_w = (int)(std::max(std::abs(cs), std::abs(sn)) * std::max(iw, ih) * scale) + 4;
    int bnd_h = bnd_w;
    for (int by = -bnd_h; by < bnd_h; ++by) {
        for (int bx = -bnd_w; bx < bnd_w; ++bx) {
            // inverse transform to source space (centered)
            double sx0 = (bx * cs + by * sn) / scale + cx0;
            double sy0 = (-bx * sn + by * cs) / scale + cy0;
            int isx = (int)std::round(sx0), isy = (int)std::round(sy0);
            if (isx >= 0 && isx < iw && isy >= 0 && isy < ih) {
                int rr, gg, bb, aa;
                img->get_pixel(isx, isy, rr, gg, bb, aa);
                int dstx = x + cx0*(int)scale + bx;
                int dsty = y + cy0*(int)scale + by;
                int alpha = (int)(aa * opacity);
                if (alpha > 0) framebuffer->set_pixel(dstx, dsty, rr, gg, bb, alpha);
            }
        }
    }
}
void Game::draw_sprite(const Sprite* s) {
    if (!s || !s->visible || !s->image) return;
    // 计算锚点偏移到左上角
    int dx = (int)(s->x - s->anchor_x * s->scale);
    int dy = (int)(s->y - s->anchor_y * s->scale);
    if (s->angle == 0 && s->scale == 1.0 && s->opacity >= 1.0) {
        // 快速 blit
        if (!framebuffer) return;
        for (int y = 0; y < s->image->height; ++y)
            for (int x = 0; x < s->image->width; ++x) {
                int rr, gg, bb, aa;
                s->image->get_pixel(x, y, rr, gg, bb, aa);
                if (aa > 0) framebuffer->set_pixel(dx + x, dy + y, rr, gg, bb, aa);
            }
    } else {
        draw_image(s->image.get(), dx, dy, s->angle, s->scale, s->opacity);
    }
}

bool Game::key_down(const std::string& name) const {
    return input.key_names.count(name) > 0;
}
bool Game::key_down_code(int code) const {
    return input.keys_down.count(code) > 0;
}

} // namespace game2d

// ============================================================
// 模块注册：game2d
// ============================================================
void register_game2d_module(std::unordered_map<std::string, ValuePtr>& std_modules) {
    using namespace game2d;
    auto mod = Value::make_module();
    auto& u = *mod->module_rep;

    auto mk_fn = [](const std::string& mname, const std::string& name, size_t min_a, size_t max_a,
                    std::function<ValuePtr(const ValueVec&, Environment&)> fn) {
        auto fv = std::make_shared<FunctionValue>();
        fv->name = name; fv->is_builtin = true;
        fv->builtin_fn = [mname, name, min_a, max_a, fn](const ValueVec& args, Environment& env) -> ValuePtr {
            if (args.size() < min_a || (max_a != (size_t)-1 && args.size() > max_a))
                throw RuntimeError(mname + "." + name + " expects " +
                    std::to_string(min_a) + "~" + std::to_string(max_a) + " args, got " +
                    std::to_string(args.size()));
            return fn(args, env);
        };
        auto v = Value::make_none(); v->type = ValueType::Function; v->fn_rep = fv;
        return v;
    };

    auto as_sprite = [](const ValuePtr& v) -> SpritePtr {
        if (v->type != ValueType::Opaque || !v->opaque_rep || v->opaque_rep->kind != "g2d_sprite")
            throw RuntimeError("expected g2d_sprite");
        auto* p = v->opaque_rep->try_as<SpritePtr>();
        return p ? *p : nullptr;
    };
    auto wrap_sprite = [](SpritePtr s) {
        auto res = std::make_shared<OpaqueResource>("g2d_sprite", std::move(s));
        return Value::make_opaque(res);
    };
    auto as_image = [](const ValuePtr& v) -> ImagePtr {
        if (v->type != ValueType::Opaque || !v->opaque_rep || v->opaque_rep->kind != "g2d_image")
            throw RuntimeError("expected g2d_image");
        auto* p = v->opaque_rep->try_as<ImagePtr>();
        return p ? *p : nullptr;
    };
    auto wrap_image = [](ImagePtr s) {
        auto res = std::make_shared<OpaqueResource>("g2d_image", std::move(s));
        return Value::make_opaque(res);
    };

    auto add = [&](const std::string& n, size_t a0, size_t a1,
                   std::function<ValuePtr(const ValueVec&, Environment&)> f) {
        u[n] = mk_fn("game2d", n, a0, a1, std::move(f));
    };
    auto I = [](const ValuePtr& v) { return value_to_int(v)->int_val; };
    auto D = [](const ValuePtr& v) { return value_to_float(v)->float_val; };

    // ---- 常量 ----
    auto& g = Game::instance();
    u["running"] = Value::make_bool(g.running);
    u["backend"] = Value::make_str(g.sdl_active ? "sdl2" : "headless_soft");

    // ---- 窗口 ----
    add("set_window", 2, 3, [&](const ValueVec& a, Environment&) {
        int w = (int)I(a[0]); int h = (int)I(a[1]);
        std::string t = a.size() >= 3 ? a[2]->to_string() : "Vortex Game2D";
        g.ensure_window(w, h, t);
        return Value::make_none();
    });
    add("set_title", 1, 1, [&](const ValueVec& a, Environment&) {
        g.title = a[0]->to_string(); return Value::make_none();
    });
    add("width", 0, 0, [&](const ValueVec&, Environment&) { return Value::make_int(g.width); });
    add("height", 0, 0, [&](const ValueVec&, Environment&) { return Value::make_int(g.height); });
    add("time", 0, 0, [&](const ValueVec&, Environment&) { return Value::make_float(g.time); });
    add("dt", 0, 0, [&](const ValueVec&, Environment&) { return Value::make_float(g.dt); });
    add("set_fps", 1, 1, [&](const ValueVec& a, Environment&) { g.fps = (int)I(a[0]); return Value::make_none(); });

    // ---- 主循环 ----
    add("run", 0, 1, [&](const ValueVec& a, Environment&) {
        // 找到当前解释器（通过 Game 绑定的接口）
        auto* interp = Game::get_interpreter();
        int max_f = a.empty() ? 0 : (int)I(a[0]);
        g.run_loop(interp, max_f);
        return Value::make_none();
    });
    add("quit", 0, 0, [&](const ValueVec&, Environment&) { g.close_window(); return Value::make_none(); });

    // ---- 清屏 ----
    add("clear", 0, 3, [&](const ValueVec& a, Environment&) {
        int r = a.size() >= 1 ? (int)I(a[0]) : 0;
        int g2 = a.size() >= 2 ? (int)I(a[1]) : 0;
        int b = a.size() >= 3 ? (int)I(a[2]) : 0;
        g.clear(r, g2, b); return Value::make_none();
    });

    // ---- 图元 ----
    add("draw_rect", 4, 8, [&](const ValueVec& a, Environment&) {
        int x = (int)I(a[0]), y = (int)I(a[1]), w = (int)I(a[2]), h = (int)I(a[3]);
        int r = a.size() >= 5 ? (int)I(a[4]) : 255;
        int g2 = a.size() >= 6 ? (int)I(a[5]) : 255;
        int b = a.size() >= 7 ? (int)I(a[6]) : 255;
        bool fill = a.size() < 8 ? true : (a[7]->truthy());
        g.draw_rect(x, y, w, h, r, g2, b, 255, fill); return Value::make_none();
    });
    add("draw_circle", 3, 7, [&](const ValueVec& a, Environment&) {
        int cx = (int)I(a[0]), cy = (int)I(a[1]), rad = (int)I(a[2]);
        int r = a.size() >= 4 ? (int)I(a[3]) : 255;
        int g2 = a.size() >= 5 ? (int)I(a[4]) : 255;
        int b = a.size() >= 6 ? (int)I(a[5]) : 255;
        bool fill = a.size() < 7 ? true : (a[6]->truthy());
        g.draw_circle(cx, cy, rad, r, g2, b, 255, fill); return Value::make_none();
    });
    add("draw_line", 4, 8, [&](const ValueVec& a, Environment&) {
        int x1 = (int)I(a[0]), y1 = (int)I(a[1]), x2 = (int)I(a[2]), y2 = (int)I(a[3]);
        int r = a.size() >= 5 ? (int)I(a[4]) : 255;
        int g2 = a.size() >= 6 ? (int)I(a[5]) : 255;
        int b = a.size() >= 7 ? (int)I(a[6]) : 255;
        int th = a.size() >= 8 ? (int)I(a[7]) : 1;
        g.draw_line(x1, y1, x2, y2, r, g2, b, 255, th); return Value::make_none();
    });
    add("draw_text", 3, 8, [&](const ValueVec& a, Environment&) {
        std::string t = a[0]->to_string();
        int x = (int)I(a[1]), y = (int)I(a[2]);
        int r = a.size() >= 4 ? (int)I(a[3]) : 255;
        int g2 = a.size() >= 5 ? (int)I(a[4]) : 255;
        int b = a.size() >= 6 ? (int)I(a[5]) : 255;
        int sz = a.size() >= 7 ? (int)I(a[6]) : 16;
        g.draw_text(t, x, y, r, g2, b, 255, sz); return Value::make_none();
    });

    // ---- 图像 ----
    add("new_image", 2, 2, [&](const ValueVec& a, Environment&) {
        return wrap_image(std::make_shared<Image>((int)I(a[0]), (int)I(a[1])));
    });
    add("checker_image", 2, 3, [&](const ValueVec& a, Environment&) {
        int tile = a.size() >= 3 ? (int)I(a[2]) : 32;
        return wrap_image(Image::make_checker((int)I(a[0]), (int)I(a[1]), tile));
    });
    add("solid_image", 5, 5, [&](const ValueVec& a, Environment&) {
        return wrap_image(Image::make_solid((int)I(a[0]), (int)I(a[1]),
                                            (int)I(a[2]), (int)I(a[3]), (int)I(a[4])));
    });
    add("image_size", 1, 1, [&](const ValueVec& a, Environment&) {
        auto im = as_image(a[0]);
        return Value::make_pair(Value::make_int(im->width), Value::make_int(im->height));
    });
    add("draw_image", 3, 6, [&](const ValueVec& a, Environment&) {
        auto im = as_image(a[0]);
        int x = (int)I(a[1]), y = (int)I(a[2]);
        double ang = a.size() >= 4 ? D(a[3]) : 0.0;
        double sc = a.size() >= 5 ? D(a[4]) : 1.0;
        double op = a.size() >= 6 ? D(a[5]) : 1.0;
        g.draw_image(im.get(), x, y, ang, sc, op);
        return Value::make_none();
    });

    // ---- 精灵 ----
    add("new_sprite", 1, 3, [&](const ValueVec& a, Environment&) {
        auto sp = std::make_shared<Sprite>();
        sp->image = as_image(a[0]);
        if (a.size() >= 2) sp->x = D(a[1]);
        if (a.size() >= 3) sp->y = value_to_int(a[2]) ? D(a[2]) : 0; // 兼容：若传 pair 也可
        // 自动居中锚
        if (sp->image) { sp->anchor_x = sp->image->width / 2; sp->anchor_y = sp->image->height / 2; }
        g.sprites.push_back(sp);
        return wrap_sprite(sp);
    });
    auto sprite_setter = [&](const char* name,
                             std::function<void(Sprite*, const ValueVec&, size_t)> fn,
                             size_t req) {
        add(name, req, req, [fn, as_sprite](const ValueVec& a, Environment&) {
            auto sp = as_sprite(a[0]);
            fn(sp.get(), a, 1);
            return Value::make_none();
        });
    };
    add("sprite_pos", 1, 3, [&](const ValueVec& a, Environment&) {
        auto sp = as_sprite(a[0]);
        if (a.size() >= 3) { sp->x = D(a[1]); sp->y = D(a[2]); return Value::make_none(); }
        return Value::make_pair(Value::make_float(sp->x), Value::make_float(sp->y));
    });
    add("sprite_move", 3, 3, [&](const ValueVec& a, Environment&) {
        auto sp = as_sprite(a[0]); sp->x += D(a[1]); sp->y += D(a[2]); return Value::make_none();
    });
    add("sprite_angle", 1, 2, [&](const ValueVec& a, Environment&) {
        auto sp = as_sprite(a[0]);
        if (a.size() >= 2) { sp->angle = D(a[1]); return Value::make_none(); }
        return Value::make_float(sp->angle);
    });
    add("sprite_scale", 1, 2, [&](const ValueVec& a, Environment&) {
        auto sp = as_sprite(a[0]);
        if (a.size() >= 2) { sp->scale = D(a[1]); return Value::make_none(); }
        return Value::make_float(sp->scale);
    });
    add("sprite_visible", 1, 2, [&](const ValueVec& a, Environment&) {
        auto sp = as_sprite(a[0]);
        if (a.size() >= 2) { sp->visible = a[1]->truthy(); return Value::make_none(); }
        return Value::make_bool(sp->visible);
    });
    add("draw_sprite", 1, 1, [&](const ValueVec& a, Environment&) {
        g.draw_sprite(as_sprite(a[0]).get()); return Value::make_none();
    });
    add("draw_all_sprites", 0, 0, [&](const ValueVec&, Environment&) {
        for (auto& s : g.sprites) if (s->visible) g.draw_sprite(s.get());
        return Value::make_none();
    });
    add("collides", 2, 2, [&](const ValueVec& a, Environment&) {
        return Value::make_bool(as_sprite(a[0])->collides_rect(*as_sprite(a[1])));
    });
    add("sprite_contains_point", 3, 3, [&](const ValueVec& a, Environment&) {
        return Value::make_bool(as_sprite(a[0])->collides_point(D(a[1]), D(a[2])));
    });

    // ---- 输入 ----
    add("key_down", 1, 1, [&](const ValueVec& a, Environment&) {
        return Value::make_bool(g.key_down(a[0]->to_string()));
    });
    add("mouse_pos", 0, 0, [&](const ValueVec&, Environment&) {
        return Value::make_pair(Value::make_float(g.input.mouse_x), Value::make_float(g.input.mouse_y));
    });
    add("mouse_left", 0, 0, [&](const ValueVec&, Environment&) { return Value::make_bool(g.input.mouse_left); });
    add("mouse_right", 0, 0, [&](const ValueVec&, Environment&) { return Value::make_bool(g.input.mouse_right); });
    add("set_key", 2, 2, [&](const ValueVec& a, Environment&) {
        // 测试时注入按键事件
        if (a[1]->truthy()) g.input.key_names.insert(a[0]->to_string());
        else g.input.key_names.erase(a[0]->to_string());
        return Value::make_none();
    });
    add("set_mouse", 3, 5, [&](const ValueVec& a, Environment&) {
        g.input.mouse_x = D(a[0]); g.input.mouse_y = D(a[1]);
        if (a.size() >= 3) g.input.mouse_left = a[2]->truthy();
        if (a.size() >= 4) g.input.mouse_middle = a[3]->truthy();
        if (a.size() >= 5) g.input.mouse_right = a[4]->truthy();
        return Value::make_none();
    });

    // ---- 音效（占位 API） ----
    add("load_sound", 1, 1, [&](const ValueVec& a, Environment&) {
        auto s = std::make_shared<Sound>();
        s->path = a[0]->to_string();
        s->loaded = true;
        auto res = std::make_shared<OpaqueResource>("g2d_sound", s);
        return Value::make_opaque(res);
    });
    add("play_sound", 1, 1, [&](const ValueVec&, Environment&) { return Value::make_none(); });
    add("music_play", 1, 1, [&](const ValueVec& a, Environment&) {
        g.music.current = a[0]->to_string(); g.music.playing = true; return Value::make_none();
    });
    add("music_stop", 0, 0, [&](const ValueVec&, Environment&) { g.music.playing = false; return Value::make_none(); });
    add("music_volume", 1, 1, [&](const ValueVec& a, Environment&) {
        g.music.volume = std::clamp(D(a[0]), 0.0, 1.0); return Value::make_none();
    });

    std_modules["game2d"] = mod;
}

} // namespace vortex
