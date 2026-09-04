// ============================================================
// render3d_module.h — 3D 渲染模块（支持 GPU 加速管线）
//
// 数据模型：
//   * Scene     — 场景图根
//   * Node      — 场景节点（transform + 子节点 + 可带 Mesh）
//   * Mesh      — 顶点（pos/normal/uv）+ 索引 + Material
//   * Material  — 颜色 / 贴图 / 金属度 / 粗糙度
//   * Camera    — 透视或正交相机
//   * Light     — 方向光 / 点光源
//   * Texture   — 2D 贴图
//
// 渲染后端：
//   * 默认 CPU 光栅化（headless/CI 保证能跑通），输出 Image。
//   * 启用 OpenGL (VORTEX_WITH_GL) 后切换为 GPU 着色管线，
//     并可与 CUDA 做资源互操作（在纹理/Buffer层面共享）。
//
// GPU 加速路径的钩子已经预埋：
//   Scene::render_gpu(Camera*) 会在可用时调用 OpenGL 管线。
// ============================================================
#ifndef VORTEX_RENDER3D_MODULE_H
#define VORTEX_RENDER3D_MODULE_H

#include "value.h"
#include "game2d_module.h"
#include <unordered_map>
#include <string>
#include <vector>
#include <memory>
#include <array>
#include <cmath>

namespace vortex {
namespace render3d {

// ---------- 线性代数（轻量，避免依赖 glm） ----------
struct Vec3 {
    float x = 0, y = 0, z = 0;
    Vec3() = default;
    Vec3(float a, float b, float c) : x(a), y(b), z(c) {}
    Vec3  operator+ (const Vec3& o) const { return {x+o.x, y+o.y, z+o.z}; }
    Vec3  operator- (const Vec3& o) const { return {x-o.x, y-o.y, z-o.z}; }
    Vec3  operator* (const Vec3& o) const { return {x*o.x, y*o.y, z*o.z}; } // Hadamard（颜色调制）
    Vec3  operator* (float s)       const { return {x*s, y*s, z*s}; }
    Vec3  operator/ (float s)       const { return {x/s, y/s, z/s}; }
    float dot(const Vec3& o) const { return x*o.x + y*o.y + z*o.z; }
    Vec3  cross(const Vec3& o) const { return {y*o.z-z*o.y, z*o.x-x*o.z, x*o.y-y*o.x}; }
    float length2() const { return dot(*this); }
    float length()  const { return std::sqrt(length2()); }
    Vec3  normalized() const { float l = length(); return l > 1e-6f ? *this/l : Vec3(); }
};
inline Vec3 operator*(float s, const Vec3& v) { return v * s; }

struct Vec2 { float u = 0, v = 0; };

struct Mat4 {
    float m[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    static Mat4 identity();
    static Mat4 perspective(float fovy_rad, float aspect, float znear, float zfar);
    static Mat4 ortho(float left, float right, float bottom, float top, float znear, float zfar);
    static Mat4 lookAt(const Vec3& eye, const Vec3& center, const Vec3& up);
    static Mat4 translate(const Vec3& t);
    static Mat4 scale(const Vec3& s);
    static Mat4 rotateX(float a);
    static Mat4 rotateY(float a);
    static Mat4 rotateZ(float a);
    Mat4 operator*(const Mat4& o) const;
    Vec3 transform(const Vec3& p) const;     // 点：w=1
    Vec3 transform_dir(const Vec3& d) const; // 方向：w=0
    Mat4  inverse() const;
    Mat4  transpose() const;
};

// ---------- 贴图 ----------
struct Texture {
    int width = 1, height = 1;
    std::vector<unsigned char> rgba; // 4*w*h

    Texture() : rgba(4, 255) {}
    Texture(int w, int h) : width(w), height(h), rgba((size_t)w*h*4, 200) {}

    void sample(float u, float v, int& r, int& g, int& b, int& a) const;
};
using TexturePtr = std::shared_ptr<Texture>;

// ---------- 材质 ----------
struct Material {
    Vec3  albedo = {0.8f, 0.8f, 0.8f};
    float metallic  = 0.0f;
    float roughness = 0.5f;
    float opacity   = 1.0f;
    TexturePtr texture;
    float shininess = 32.0f; // Blinn-Phong
};
using MaterialPtr = std::shared_ptr<Material>;

// ---------- 网格 ----------
struct Mesh;
using MeshPtr = std::shared_ptr<Mesh>;

struct Mesh {
    struct Vertex { Vec3 pos; Vec3 normal; Vec2 uv; };
    std::vector<Vertex> vertices;
    std::vector<unsigned int> indices;
    MaterialPtr material;

    // 若启用 GPU 管线则预先上传为 VBO/VAO（句柄以 std::any 存储）
    std::any gpu_resource;
    bool gpu_uploaded = false;

    void compute_normals();
    static MeshPtr make_box(float sx, float sy, float sz, MaterialPtr mat = nullptr);
    static MeshPtr make_sphere(float radius, int stacks, int slices, MaterialPtr mat = nullptr);
    static MeshPtr make_plane(float size, int subdiv, MaterialPtr mat = nullptr);
    static MeshPtr make_cylinder(float r, float h, int slices, MaterialPtr mat = nullptr);
};

// ---------- 光源 ----------
enum class LightKind { Directional, Point, Spot };
struct Light {
    LightKind kind = LightKind::Directional;
    Vec3  position_or_direction = {0, -1, 0};
    Vec3  color = {1, 1, 1};
    float intensity = 1.0f;
    float range = 100.0f;         // point/spot
    float spot_cutoff = 0.5f;     // spot (cos)
};
using LightPtr = std::shared_ptr<Light>;

// ---------- 相机 ----------
enum class CameraKind { Perspective, Orthographic };
struct Camera {
    CameraKind kind = CameraKind::Perspective;
    Vec3  position = {0, 0, 5};
    Vec3  target   = {0, 0, 0};
    Vec3  up       = {0, 1, 0};
    float fov_deg  = 60.0f;
    float aspect   = 1.0f;
    float znear    = 0.1f;
    float zfar     = 500.0f;
    float ortho_size = 10.0f;

    Mat4 view() const;
    Mat4 proj(int target_width, int target_height) const;
};
using CameraPtr = std::shared_ptr<Camera>;

// ---------- 场景节点 ----------
struct Node : std::enable_shared_from_this<Node> {
    std::string name;
    Vec3    pos = {0,0,0};
    Vec3    euler = {0,0,0}; // degrees
    Vec3    scale3 = {1,1,1};
    MeshPtr mesh;
    std::vector<std::shared_ptr<Node>> children;
    std::weak_ptr<Node> parent;

    Mat4 local_matrix() const;
    Mat4 world_matrix() const;
    void add_child(std::shared_ptr<Node> c) { c->parent = shared_from_this(); children.push_back(c); }
};
using NodePtr = std::shared_ptr<Node>;

// ---------- 场景 ----------
struct Scene {
    std::vector<NodePtr>   roots;
    std::vector<LightPtr>  lights;
    Vec3  ambient = {0.1f, 0.1f, 0.15f};
    Vec3  fog_color = {0.02f, 0.03f, 0.08f};
    float fog_start = 50.0f;
    float fog_end   = 300.0f;
    bool  fog_enabled = true;

    // ---- 渲染 ----
    // 软件光栅化：逐三角、深度缓冲 + Blinn-Phong + 纹理采样
    std::shared_ptr<game2d::Image> render_software(const CameraPtr& cam, int w, int h) const;
    // GPU 加速渲染钩子（启用 OpenGL 时实现）；返回 true 表示已走 GPU 路径。
    bool render_gpu(const CameraPtr& cam, int w, int h, unsigned int fbo = 0) const;
    // 对外统一：优先 GPU，失败则 CPU
    std::shared_ptr<game2d::Image> render(const CameraPtr& cam, int w, int h);

    // 是否有可用 GPU 上下文
    static bool is_gpu_available();
};
using ScenePtr = std::shared_ptr<Scene>;

} // namespace render3d

void register_render3d_module(std::unordered_map<std::string, ValuePtr>& std_modules);

} // namespace vortex
#endif
