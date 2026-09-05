// ============================================================
// r3d_g2d.h — game2d / render3d 模块转发层（编译 exe 侧）
//
// vortex 编译产物的 exe 只静态链接 vortex_runtime（不含解释器）。
// 本层承载 game2d/render3d 的纯 C++ 数据与算法，并对 LLVM 后端
// 暴露 C ABI 转发函数；对象以不透明 void* 句柄（经仓库保活）
// 在脚本层传递，跨模块（如 render3d.render -> game2d.image_size）
// 共用同一 Image。
//
// 说明：算法自 vortex_core 对应模块抽取，保证 API 行为一致；
// 渲染按"尺寸 + API 可用"验收，非逐像素 golden。
// ============================================================
#ifndef VORTEX_RUNTIME_R3D_G2D_H
#define VORTEX_RUNTIME_R3D_G2D_H

#include <vector>
#include <string>
#include <cmath>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <functional>

// ============================================================
// game2d 核心（纯 C++，headless）
// ============================================================
namespace vor {

// RGBA 像素图
struct Image {
    int width = 0, height = 0;
    std::vector<unsigned char> px; // 4*width*height

    Image() = default;
    Image(int w, int h) : width(w), height(h), px((size_t)w * h * 4, 0) {}

    void fill(int r, int g, int b, int a = 255) {
        for (size_t i = 0; i < px.size(); i += 4) {
            px[i] = (unsigned char)std::clamp(r,0,255);
            px[i+1] = (unsigned char)std::clamp(g,0,255);
            px[i+2] = (unsigned char)std::clamp(b,0,255);
            px[i+3] = (unsigned char)std::clamp(a,0,255);
        }
    }
    void set_pixel(int x, int y, int r, int g, int b, int a = 255) {
        if (x < 0 || x >= width || y < 0 || y >= height) return;
        size_t o = ((size_t)y * width + x) * 4;
        px[o]   = (unsigned char)std::clamp(r,0,255);
        px[o+1] = (unsigned char)std::clamp(g,0,255);
        px[o+2] = (unsigned char)std::clamp(b,0,255);
        px[o+3] = (unsigned char)std::clamp(a,0,255);
    }
    void get_pixel(int x, int y, int& r, int& g, int& b, int& a) const {
        if (x < 0 || x >= width || y < 0 || y >= height) { r=g=b=a=0; return; }
        size_t o = ((size_t)y * width + x) * 4;
        r = px[o]; g = px[o+1]; b = px[o+2]; a = px[o+3];
    }
    static std::shared_ptr<Image> make_checker(int w, int h, int tile = 32) {
        auto p = std::make_shared<Image>(w, h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                bool d = ((x/tile) + (y/tile)) % 2 == 0;
                if (d) p->set_pixel(x, y, 0x22, 0x22, 0x22);
                else   p->set_pixel(x, y, 0xCC, 0xCC, 0xCC);
            }
        return p;
    }
    static std::shared_ptr<Image> make_solid(int w, int h, int r, int g, int b, int a = 255) {
        auto p = std::make_shared<Image>(w, h);
        p->fill(r, g, b, a);
        return p;
    }
};

// 精灵
struct Sprite {
    std::shared_ptr<Image> image;
    double x = 0, y = 0, angle = 0, scale = 1, opacity = 1.0;
    bool visible = true;
    int anchor_x = 0, anchor_y = 0;
    double width()  const { return image ? image->width  * scale : 0; }
    double height() const { return image ? image->height * scale : 0; }
    double left()   const { return x - anchor_x * scale; }
    double top()    const { return y - anchor_y * scale; }
    double right()  const { return left() + width(); }
    double bottom() const { return top()  + height(); }
};

// 全局游戏状态（headless：软件 framebuffer）
struct Game2D {
    bool running = false;
    int width = 800, height = 600;
    std::string title = "Vortex Game2D";
    std::shared_ptr<Image> framebuffer;
    std::vector<std::shared_ptr<Sprite>> sprites;
    std::unordered_set<std::string> key_names;
    double time = 0, dt = 0.016; int fps = 60;
    bool sdl_active = false;

    static Game2D& instance() { static Game2D g; return g; }
    void ensure_window(int w, int h, const std::string& t) {
        width = w; height = h; title = t;
        if (!framebuffer || framebuffer->width != w || framebuffer->height != h)
            framebuffer = std::make_shared<Image>(w, h);
        framebuffer->fill(0,0,0);
        running = true;
    }
    void close_window() { running = false; }

    void clear(int r, int g, int b) { if (!framebuffer) ensure_window(width,height,title); framebuffer->fill(r,g,b); }
    void draw_rect(int x,int y,int w,int h,int r,int g,int b,int a,bool fill);
    void draw_circle(int cx,int cy,int radius,int r,int g,int b,int a,bool fill);
    void draw_line(int x1,int y1,int x2,int y2,int r,int g,int b,int a,int thickness);
    void draw_text(const std::string& t,int x,int y,int r,int g,int b,int a,int size);
    void draw_image(const Image* img,int x,int y,double angle_deg,double scale,double opacity);
    void draw_sprite(const Sprite* s);
    void draw_all_sprites();
    bool key_down(const std::string& name) const { return key_names.count(name) > 0; }
};

// ============================================================
// render3d 核心（纯 C++，CPU 光栅化）
// ============================================================
struct Vec3 {
    float x=0,y=0,z=0;
    Vec3()=default; Vec3(float a,float b,float c):x(a),y(b),z(c){}
    Vec3 operator+(const Vec3&o)const{return{x+o.x,y+o.y,z+o.z};}
    Vec3 operator-(const Vec3&o)const{return{x-o.x,y-o.y,z-o.z};}
    Vec3 operator*(const Vec3&o)const{return{x*o.x,y*o.y,z*o.z};}
    Vec3 operator*(float s)const{return{x*s,y*s,z*s};}
    Vec3 operator/(float s)const{return{x/s,y/s,z/s};}
    float dot(const Vec3&o)const{return x*o.x+y*o.y+z*o.z;}
    Vec3 cross(const Vec3&o)const{return{y*o.z-z*o.y,z*o.x-x*o.z,x*o.y-y*o.x};}
    float length()const{return std::sqrt(dot(*this));}
    Vec3 normalized()const{float l=length();return l>1e-6f?*this/l:Vec3();}
};
struct Vec2 { float u=0,v=0; };

struct Mat4 {
    float m[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    static Mat4 perspective(float fovy,float asp,float n,float f);
    static Mat4 ortho(float l,float r1,float b,float t,float n,float f);
    static Mat4 lookAt(const Vec3&e,const Vec3&c,const Vec3&u);
    static Mat4 translate(const Vec3&t);
    static Mat4 scale(const Vec3&s);
    static Mat4 rotateX(float a); static Mat4 rotateY(float a); static Mat4 rotateZ(float a);
    Mat4 operator*(const Mat4&o)const;
    Vec3 transform(const Vec3&p)const;
    Vec3 transform_dir(const Vec3&d)const;
};

struct Texture { int width=1,height=1; std::vector<unsigned char> rgba;
    Texture():rgba(4,255){} Texture(int w,int h):width(w),height(h),rgba((size_t)w*h*4,200){}
    void sample(float u,float v,int&r,int&g,int&b,int&a)const; };
using TexturePtr=std::shared_ptr<Texture>;

struct Material { Vec3 albedo={0.8f,0.8f,0.8f}; float metallic=0,roughness=0.5f,opacity=1.0f,shininess=32.0f; TexturePtr texture; };
using MaterialPtr=std::shared_ptr<Material>;

struct Mesh;
using MeshPtr=std::shared_ptr<Mesh>;
struct Mesh {
    struct Vertex { Vec3 pos,normal; Vec2 uv; };
    std::vector<Vertex> vertices;
    std::vector<unsigned int> indices;
    MaterialPtr material;
    static MeshPtr make_box(float sx,float sy,float sz,MaterialPtr mat=nullptr);
    static MeshPtr make_sphere(float r,int stacks,int slices,MaterialPtr mat=nullptr);
    static MeshPtr make_plane(float size,int subdiv,MaterialPtr mat=nullptr);
    static MeshPtr make_cylinder(float r,float h,int slices,MaterialPtr mat=nullptr);
};

enum class LightKind { Directional, Point, Spot };
struct Light { LightKind kind=LightKind::Directional; Vec3 pd={0,-1,0}; Vec3 color={1,1,1}; float intensity=1.0f,range=100.0f,spot_cutoff=0.5f; };
using LightPtr=std::shared_ptr<Light>;

enum class CamKind { Perspective, Orthographic };
struct Camera { CamKind kind=CamKind::Perspective; Vec3 position={0,0,5},target={0,0,0},up={0,1,0}; float fov_deg=60.0f,aspect=1.0f,znear=0.1f,zfar=500.0f,ortho_size=10.0f;
    Mat4 view()const{return Mat4::lookAt(position,target,up);}
    Mat4 proj(int w,int h)const {
        float asp = aspect>0 ? aspect : (float)w/std::max(1,h);
        if (kind==CamKind::Perspective) return Mat4::perspective(fov_deg*3.14159265f/180.0f,asp,znear,zfar);
        float s=ortho_size*0.5f; return Mat4::ortho(-s*asp,s*asp,-s,s,znear,zfar);
    }
};
using CameraPtr=std::shared_ptr<Camera>;

struct Node; using NodePtr=std::shared_ptr<Node>;
struct Node : std::enable_shared_from_this<Node> {
    Vec3 pos={0,0,0}, euler={0,0,0}, scale3={1,1,1};
    MeshPtr mesh;
    std::vector<NodePtr> children;
    std::weak_ptr<Node> parent;
    Mat4 local_matrix()const;
    Mat4 world_matrix()const;
    void add_child(NodePtr c){ c->parent=shared_from_this(); children.push_back(c); }
};

struct Scene {
    std::vector<NodePtr> roots;
    std::vector<LightPtr> lights;
    Vec3 ambient={0.1f,0.1f,0.15f};
    Vec3 fog_color={0.02f,0.03f,0.08f};
    float fog_start=50.0f, fog_end=300.0f;
    bool fog_enabled=true;
    std::shared_ptr<Image> render(const CameraPtr& cam,int w,int h) const;
};
using ScenePtr=std::shared_ptr<Scene>;

// ============================================================
// 对象仓库：把 shared_ptr 对象以 void* 句柄保活到进程退出
// ============================================================
struct RtKeeper {
    std::mutex m;
    std::vector<std::shared_ptr<void>> objs;
    void keep(std::shared_ptr<void> p){ if(p) { std::lock_guard<std::mutex> lk(m); objs.push_back(std::move(p)); } }
} inline g_rt_keep;

// 把 shared_ptr<T> 存入仓库并返回裸指针句柄
template<class T> void* rt_store(std::shared_ptr<T> p) {
    void* raw = p.get();
    g_rt_keep.keep(std::shared_ptr<void>(p)); // 保活（共享所有权）
    return raw;
}

// 从 handle 取 shared_ptr<T>（对象首指针强转）
template<class T> std::shared_ptr<T> rt_from(void* h) {
    if (!h) return nullptr;
    return std::shared_ptr<T>(static_cast<T*>(h), [](T*){/*不释放：由仓库保活*/});
}

} // namespace vor

// ============================================================
// C ABI 转发（供 LLVM 后端调用）
// ============================================================
#ifdef __cplusplus
extern "C" {
#endif

// ---- game2d ----
void*  vor_g2d_new_image(long long w, long long h);
void*  vor_g2d_checker_image(long long w, long long h, long long tile);
void*  vor_g2d_solid_image(long long w, long long h, long long r, long long g, long long b);
int    vor_g2d_image_get_pixel(void* img, long long x, long long y, long long* out); // 0/1
void*  vor_g2d_image_size_pair(void* img); // VPair*(w,h)
long long vor_g2d_image_w(void* img);
long long vor_g2d_image_h(void* img);
void*  vor_g2d_new_sprite(void* img, double x, double y);
void   vor_g2d_sprite_setpos(void* sp, double x, double y);
double vor_g2d_sprite_posx(void* sp);
double vor_g2d_sprite_posy(void* sp);
void   vor_g2d_sprite_move(void* sp, double dx, double dy);
void   vor_g2d_sprite_setangle(void* sp, double a);
double vor_g2d_sprite_angle(void* sp);
void   vor_g2d_sprite_setscale(void* sp, double s);
double vor_g2d_sprite_scale(void* sp);
void   vor_g2d_sprite_setvisible(void* sp, int v);
int    vor_g2d_sprite_visible(void* sp);
int    vor_g2d_sprite_collides(void* a, void* b);
int    vor_g2d_sprite_contains(void* sp, double px, double py);
void   vor_g2d_sprite_set_attr(void* sp, long long anchor_x, long long anchor_y, double scale);
double vor_g2d_sprite_width(void* sp);
double vor_g2d_sprite_height(void* sp);
// 窗口 / 绘制 / 状态
void   vor_g2d_set_window(long long w, long long h, const char* t);
void   vor_g2d_set_title(const char* t);
long long vor_g2d_width(void);
long long vor_g2d_height(void);
double vor_g2d_time(void);
double vor_g2d_dt(void);
void   vor_g2d_set_fps(long long fps);
void   vor_g2d_quit(void);
void   vor_g2d_clear(long long r, long long g, long long b);
void   vor_g2d_draw_rect(long long x,long long y,long long w,long long h,long long r,long long g,long long b,long long a,int fill);
void   vor_g2d_draw_circle(long long cx,long long cy,long long rad,long long r,long long g,long long b,long long a,int fill);
void   vor_g2d_draw_line(long long x1,long long y1,long long x2,long long y2,long long r,long long g,long long b,long long a,long long th);
void   vor_g2d_draw_text(const char* t,long long x,long long y,long long r,long long g,long long b,long long a,long long size);
void   vor_g2d_draw_image(void* img,long long x,long long y,double angle,double scale,double opacity);
void   vor_g2d_draw_sprite(void* sp);
void   vor_g2d_draw_all_sprites(void);
int    vor_g2d_key_down(const char* k);
void   vor_g2d_set_key(const char* k, int on);
void   vor_g2d_set_mouse(double x,double y,int l,int m,int r);
int    vor_g2d_running(void);

// ---- render3d ----
void*  vor_r3d_new_scene(void);
void   vor_r3d_scene_add_node(void* sc, void* n);
void   vor_r3d_scene_add_light(void* sc, void* l);
void   vor_r3d_scene_set_ambient(void* sc, double r, double g, double b);
void   vor_r3d_scene_set_fog(void* sc, int on, double start, double end, long long hasColor, double cr, double cg, double cb);
void*  vor_r3d_new_camera_persp(void);
void*  vor_r3d_new_camera_ortho(void);
void   vor_r3d_camera_lookat(void* c, double px,double py,double pz,double tx,double ty,double tz);
void   vor_r3d_camera_set(void* c, double px,double py,double pz,double tx,double ty,double tz);
void   vor_r3d_camera_setscope(void* c, double posX,double posY,double posZ,
                               double targetX,double targetY,double targetZ, double* up, double fov, double znear, double zfar);
double vor_r3d_camera_fov_get(void* c);
void   vor_r3d_camera_fov_set(void* c, double f);
void*  vor_r3d_new_material(double r,double g,double b,double metallic,double roughness, int have);
void   vor_r3d_material_set_texture(void* m, void* t);
void*  vor_r3d_new_texture_solid(long long w,long long h,long long r,long long g,long long b);
void*  vor_r3d_new_texture_checker(long long w,long long h,long long tile,long long hasColor,double cr,double cg,double cb);
void*  vor_r3d_mesh_box(double sx,double sy,double sz, void* mat);
void*  vor_r3d_mesh_sphere(double r,long long st,long long sl, void* mat);
void*  vor_r3d_mesh_plane(double size,long long subd, void* mat);
void*  vor_r3d_mesh_cylinder(double r,double h,long long sl, void* mat);
void*  vor_r3d_new_node(void* mesh);
void   vor_r3d_node_attach_mesh(void* n, void* mesh);
void   vor_r3d_node_set_pos(void* n, double x,double y,double z);
void   vor_r3d_node_set_rot(void* n, double x,double y,double z);
void   vor_r3d_node_set_scale(void* n, double x,double y,double z);
void   vor_r3d_node_add_child(void* n, void* c);
void*  vor_r3d_new_light_dir(double x,double y,double z,long long hasColor,double cr,double cg,double cb,double intensity);
void*  vor_r3d_new_light_point(double x,double y,double z,long long hasColor,double cr,double cg,double cb,double intensity,double range);
void*  vor_r3d_render(void* sc, void* cam, long long w, long long h);
void   vor_r3d_render_to_window(void* sc, void* cam, long long w, long long h);

#ifdef __cplusplus
}
#endif

#endif // VORTEX_RUNTIME_R3D_G2D_H