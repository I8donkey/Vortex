// ============================================================
// render3d_module.cpp — CPU 光栅化实现，GPU 管线钩子已预留。
// ============================================================
#include "render3d_module.h"
#include "game2d_module.h"
#include "interpreter.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace vortex {
namespace render3d {

// ---------- Mat4 ----------
Mat4 Mat4::identity() { Mat4 r; return r; }
Mat4 Mat4::translate(const Vec3& t) {
    Mat4 r;
    r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z;
    return r;
}
Mat4 Mat4::scale(const Vec3& s) {
    Mat4 r;
    r.m[0]=s.x; r.m[5]=s.y; r.m[10]=s.z;
    return r;
}
Mat4 Mat4::rotateX(float a) {
    Mat4 r; float c=std::cos(a), s=std::sin(a);
    r.m[5]=c; r.m[6]=-s; r.m[9]=s; r.m[10]=c;
    return r;
}
Mat4 Mat4::rotateY(float a) {
    Mat4 r; float c=std::cos(a), s=std::sin(a);
    r.m[0]=c; r.m[2]=s; r.m[8]=-s; r.m[10]=c;
    return r;
}
Mat4 Mat4::rotateZ(float a) {
    Mat4 r; float c=std::cos(a), s=std::sin(a);
    r.m[0]=c; r.m[1]=-s; r.m[4]=s; r.m[5]=c;
    return r;
}
Mat4 Mat4::perspective(float fovy, float aspect, float n, float f) {
    Mat4 r;
    float tanHalf = std::tan(fovy/2.0f);
    r.m[0] = 1.0f / (aspect * tanHalf);
    r.m[5] = 1.0f / tanHalf;
    r.m[10] = (f+n) / (n-f);
    r.m[11] = -1.0f;
    r.m[14] = (2*f*n) / (n-f);
    r.m[15] = 0.0f;
    return r;
}
Mat4 Mat4::ortho(float l, float ri, float b, float t, float n, float f) {
    Mat4 r;
    r.m[0]  = 2/(ri-l);
    r.m[5]  = 2/(t-b);
    r.m[10] = -2/(f-n);
    r.m[12] = -(ri+l)/(ri-l);
    r.m[13] = -(t+b)/(t-b);
    r.m[14] = -(f+n)/(f-n);
    return r;
}
Mat4 Mat4::lookAt(const Vec3& eye, const Vec3& center, const Vec3& up) {
    Vec3 f = (center - eye).normalized();
    Vec3 s = f.cross(up).normalized();
    Vec3 u = s.cross(f);
    Mat4 r;
    r.m[0] =  s.x; r.m[4] =  s.y; r.m[8]  =  s.z;
    r.m[1] =  u.x; r.m[5] =  u.y; r.m[9]  =  u.z;
    r.m[2] = -f.x; r.m[6] = -f.y; r.m[10] = -f.z;
    r.m[12] = -s.dot(eye);
    r.m[13] = -u.dot(eye);
    r.m[14] =  f.dot(eye);
    return r;
}
Mat4 Mat4::operator*(const Mat4& o) const {
    Mat4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            r.m[i + j*4] = 0;
            for (int k = 0; k < 4; ++k)
                r.m[i + j*4] += m[i + k*4] * o.m[k + j*4];
        }
    return r;
}
Vec3 Mat4::transform(const Vec3& p) const {
    float w = m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15];
    float x = m[0] * p.x + m[4] * p.y + m[8]  * p.z + m[12];
    float y = m[1] * p.x + m[5] * p.y + m[9]  * p.z + m[13];
    float z = m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14];
    if (w != 1.0f && w != 0.0f) { x/=w; y/=w; z/=w; }
    return {x,y,z};
}
Vec3 Mat4::transform_dir(const Vec3& d) const {
    float x = m[0] * d.x + m[4] * d.y + m[8]  * d.z;
    float y = m[1] * d.x + m[5] * d.y + m[9]  * d.z;
    float z = m[2] * d.x + m[6] * d.y + m[10] * d.z;
    return {x,y,z};
}
Mat4 Mat4::transpose() const {
    Mat4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r.m[i + j*4] = m[j + i*4];
    return r;
}
// 简化：仅适用于 RT*S 合成的仿射逆（平移 + 旋转 + 各向同性缩放）
Mat4 Mat4::inverse() const {
    // 3x3 逆 + 平移调整
    float a = m[0], b = m[4], c = m[8];
    float d = m[1], e = m[5], f = m[9];
    float g = m[2], hh = m[6], i = m[10];
    float det = a*(e*i - f*hh) - b*(d*i - f*g) + c*(d*hh - e*g);
    if (std::fabs(det) < 1e-12f) return identity();
    float inv_det = 1.0f / det;
    Mat4 r;
    r.m[0] =  (e*i - f*hh) * inv_det;
    r.m[1] = -(b*i - c*hh) * inv_det;
    r.m[2] =  (b*f - c*e)  * inv_det;
    r.m[4] = -(d*i - f*g)  * inv_det;
    r.m[5] =  (a*i - c*g)  * inv_det;
    r.m[6] = -(a*f - c*d)  * inv_det;
    r.m[8] =  (d*hh - e*g) * inv_det;
    r.m[9] = -(a*hh - b*g) * inv_det;
    r.m[10]=  (a*e - b*d)  * inv_det;
    float tx = m[12], ty = m[13], tz = m[14];
    r.m[12] = -(r.m[0]*tx + r.m[4]*ty + r.m[8] *tz);
    r.m[13] = -(r.m[1]*tx + r.m[5]*ty + r.m[9] *tz);
    r.m[14] = -(r.m[2]*tx + r.m[6]*ty + r.m[10]*tz);
    return r;
}

// ---------- Texture ----------
void Texture::sample(float u, float v, int& r, int& g, int& b, int& a) const {
    // wrap
    u = u - std::floor(u);
    v = v - std::floor(v);
    int x = std::clamp((int)(u * width), 0, width-1);
    int y = std::clamp((int)((1.0f-v) * height), 0, height-1);
    size_t off = ((size_t)y * width + x) * 4;
    r = rgba[off]; g = rgba[off+1]; b = rgba[off+2]; a = rgba[off+3];
}

// ---------- Mesh ----------
void Mesh::compute_normals() {
    for (auto& v : vertices) v.normal = {0,0,0};
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        unsigned ia = indices[i], ib = indices[i+1], ic = indices[i+2];
        auto& A = vertices[ia].pos;
        auto& B = vertices[ib].pos;
        auto& C = vertices[ic].pos;
        Vec3 n = (B - A).cross(C - A);
        vertices[ia].normal = vertices[ia].normal + n;
        vertices[ib].normal = vertices[ib].normal + n;
        vertices[ic].normal = vertices[ic].normal + n;
    }
    for (auto& v : vertices) v.normal = v.normal.normalized();
}

MeshPtr Mesh::make_box(float sx, float sy, float sz, MaterialPtr mat) {
    auto m = std::make_shared<Mesh>();
    m->material = mat ? mat : std::make_shared<Material>();
    float x = sx/2, y = sy/2, z = sz/2;
    // 6 faces, 2 triangles each, simple winding
    auto face = [&](const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d, const Vec3& n) {
        size_t base = m->vertices.size();
        m->vertices.push_back({a, n, {0,0}});
        m->vertices.push_back({b, n, {1,0}});
        m->vertices.push_back({c, n, {1,1}});
        m->vertices.push_back({d, n, {0,1}});
        m->indices.push_back((unsigned)(base+0)); m->indices.push_back((unsigned)(base+1)); m->indices.push_back((unsigned)(base+2));
        m->indices.push_back((unsigned)(base+0)); m->indices.push_back((unsigned)(base+2)); m->indices.push_back((unsigned)(base+3));
    };
    // +X
    face({x,-y,-z}, {x,y,-z}, {x,y,z}, {x,-y,z}, {1,0,0});
    // -X
    face({-x,-y,z}, {-x,y,z}, {-x,y,-z}, {-x,-y,-z}, {-1,0,0});
    // +Y
    face({-x,y,-z}, {-x,y,z}, {x,y,z}, {x,y,-z}, {0,1,0});
    // -Y
    face({-x,-y,z}, {-x,-y,-z}, {x,-y,-z}, {x,-y,z}, {0,-1,0});
    // +Z
    face({-x,-y,z}, {x,-y,z}, {x,y,z}, {-x,y,z}, {0,0,1});
    // -Z
    face({x,-y,-z}, {-x,-y,-z}, {-x,y,-z}, {x,y,-z}, {0,0,-1});
    return m;
}
MeshPtr Mesh::make_sphere(float r, int stacks, int slices, MaterialPtr mat) {
    auto m = std::make_shared<Mesh>();
    m->material = mat ? mat : std::make_shared<Material>();
    for (int i = 0; i <= stacks; ++i) {
        float t = (float)i / stacks;
        float phi = 3.14159265f * t; // 0..pi
        for (int j = 0; j <= slices; ++j) {
            float s_ = (float)j / slices;
            float th = 2.0f * 3.14159265f * s_;
            float x = r * std::sin(phi) * std::cos(th);
            float y = r * std::cos(phi);
            float z = r * std::sin(phi) * std::sin(th);
            Mesh::Vertex v;
            v.pos = {x,y,z};
            v.normal = Vec3(x,y,z).normalized();
            v.uv = {s_, t};
            m->vertices.push_back(v);
        }
    }
    for (int i = 0; i < stacks; ++i)
        for (int j = 0; j < slices; ++j) {
            unsigned a = (unsigned)(i*(slices+1) + j);
            unsigned b = a + (unsigned)(slices+1);
            m->indices.push_back(a);   m->indices.push_back(b);   m->indices.push_back(a+1);
            m->indices.push_back(b);   m->indices.push_back(b+1); m->indices.push_back(a+1);
        }
    return m;
}
MeshPtr Mesh::make_plane(float size, int subdiv, MaterialPtr mat) {
    auto m = std::make_shared<Mesh>();
    m->material = mat ? mat : std::make_shared<Material>();
    int n = subdiv + 1;
    float step = size / (float)subdiv;
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            Mesh::Vertex v;
            v.pos = {-size/2 + i*step, 0, -size/2 + j*step};
            v.normal = {0,1,0};
            v.uv = {(float)i/subdiv, (float)j/subdiv};
            m->vertices.push_back(v);
        }
    for (int j = 0; j < subdiv; ++j)
        for (int i = 0; i < subdiv; ++i) {
            unsigned a = (unsigned)(j*n + i);
            unsigned b = a + (unsigned)n;
            m->indices.push_back(a); m->indices.push_back(b); m->indices.push_back(a+1);
            m->indices.push_back(b); m->indices.push_back(b+1); m->indices.push_back(a+1);
        }
    return m;
}
MeshPtr Mesh::make_cylinder(float radius, float h, int slices, MaterialPtr mat) {
    auto m = std::make_shared<Mesh>();
    m->material = mat ? mat : std::make_shared<Material>();
    float y0 = -h/2, y1 = h/2;
    for (int i = 0; i <= slices; ++i) {
        float a = 2.0f * 3.14159265f * (float)i / slices;
        float x = radius * std::cos(a);
        float z = radius * std::sin(a);
        Vec3 normal = Vec3(x,0,z).normalized();
        float u = (float)i / slices;
        m->vertices.push_back({{x, y0, z}, normal, {u, 0}});
        m->vertices.push_back({{x, y1, z}, normal, {u, 1}});
    }
    for (int i = 0; i < slices; ++i) {
        unsigned a = (unsigned)i * 2;
        m->indices.push_back(a); m->indices.push_back(a+1); m->indices.push_back(a+2);
        m->indices.push_back(a+1); m->indices.push_back(a+3); m->indices.push_back(a+2);
    }
    // caps (top/bottom as fans)
    unsigned base_top = (unsigned)m->vertices.size();
    m->vertices.push_back({{0, y1, 0}, {0,1,0}, {0.5f, 0.5f}});
    for (int i = 0; i <= slices; ++i) {
        float a = 2.0f * 3.14159265f * (float)i / slices;
        m->vertices.push_back({{radius*std::cos(a), y1, radius*std::sin(a)}, {0,1,0}, {0.5f+0.5f*std::cos(a), 0.5f+0.5f*std::sin(a)}});
    }
    for (int i = 0; i < slices; ++i) {
        m->indices.push_back(base_top);
        m->indices.push_back(base_top + 1 + i);
        m->indices.push_back(base_top + 2 + i);
    }
    unsigned base_bot = (unsigned)m->vertices.size();
    m->vertices.push_back({{0, y0, 0}, {0,-1,0}, {0.5f, 0.5f}});
    for (int i = 0; i <= slices; ++i) {
        float a = 2.0f * 3.14159265f * (float)i / slices;
        m->vertices.push_back({{radius*std::cos(a), y0, radius*std::sin(a)}, {0,-1,0}, {0.5f+0.5f*std::cos(a), 0.5f+0.5f*std::sin(a)}});
    }
    for (int i = 0; i < slices; ++i) {
        m->indices.push_back(base_bot);
        m->indices.push_back(base_bot + 2 + i);
        m->indices.push_back(base_bot + 1 + i);
    }
    return m;
}

// ---------- Node ----------
Mat4 Node::local_matrix() const {
    Mat4 T = Mat4::translate(pos);
    Mat4 Rx = Mat4::rotateX(euler.x * 3.14159265f/180.0f);
    Mat4 Ry = Mat4::rotateY(euler.y * 3.14159265f/180.0f);
    Mat4 Rz = Mat4::rotateZ(euler.z * 3.14159265f/180.0f);
    Mat4 S  = Mat4::scale(scale3);
    return T * Ry * Rx * Rz * S;
}
Mat4 Node::world_matrix() const {
    auto p = parent.lock();
    if (!p) return local_matrix();
    return p->world_matrix() * local_matrix();
}

// ---------- Camera ----------
Mat4 Camera::view() const { return Mat4::lookAt(position, target, up); }
Mat4 Camera::proj(int w, int h) const {
    float asp = aspect > 0 ? aspect : (float)w / std::max(1, h);
    if (kind == CameraKind::Perspective)
        return Mat4::perspective(fov_deg * 3.14159265f/180.0f, asp, znear, zfar);
    else {
        float s = ortho_size * 0.5f;
        return Mat4::ortho(-s*asp, s*asp, -s, s, znear, zfar);
    }
}

// ---------- Scene ----------
static inline Vec3 barycentric(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c) {
    Vec3 v0 = b - a, v1 = c - a, v2 = p - a;
    float d00 = v0.dot(v0), d01 = v0.dot(v1), d11 = v1.dot(v1), d20 = v2.dot(v0), d21 = v2.dot(v1);
    float denom = d00 * d11 - d01 * d01;
    if (std::fabs(denom) < 1e-8f) return {-1,0,0};
    float v = (d11 * d20 - d01 * d21) / denom;
    float w = (d00 * d21 - d01 * d20) / denom;
    float u = 1.0f - v - w;
    return {u, v, w};
}

std::shared_ptr<game2d::Image> Scene::render_software(const CameraPtr& cam, int w, int h) const {
    auto img = std::make_shared<game2d::Image>(w, h);
    img->fill((int)(fog_color.x*255), (int)(fog_color.y*255), (int)(fog_color.z*255));
    std::vector<float> zbuf((size_t)w * h, 1e18f);

    Mat4 V = cam->view();
    Mat4 P = cam->proj(w, h);
    Mat4 VP = P * V;

    // 汇总所有可渲染节点（简单递归收集）
    struct Drawable { NodePtr node; };
    std::vector<Drawable> draw;
    std::function<void(const NodePtr&)> collect = [&](const NodePtr& n) {
        if (!n) return;
        if (n->mesh) draw.push_back({n});
        for (auto& c : n->children) collect(c);
    };
    for (auto& r : roots) collect(r);

    auto shade = [&](const Vec3& world_pos, const Vec3& N, const MaterialPtr& M, const Vec2& uv) -> Vec3 {
        Vec3 base = M->albedo;
        // texture modulate
        if (M->texture) {
            int tr, tg, tb, ta;
            M->texture->sample(uv.u, uv.v, tr, tg, tb, ta);
            base.x *= tr/255.0f; base.y *= tg/255.0f; base.z *= tb/255.0f;
        }
        Vec3 color = Vec3(base.x*ambient.x, base.y*ambient.y, base.z*ambient.z);
        Vec3 Vdir = (cam->position - world_pos).normalized();
        for (auto& L : lights) {
            Vec3 Ldir;
            float atten = 1.0f;
            if (L->kind == LightKind::Directional) {
                Ldir = L->position_or_direction.normalized();
            } else {
                Ldir = L->position_or_direction - world_pos;
                float d = Ldir.length();
                Ldir = Ldir / std::max(d, 1e-6f);
                atten = 1.0f / (1.0f + 0.09f*d + 0.032f*d*d);
                if (d > L->range) atten = 0;
            }
            float ndl = std::max(0.0f, N.dot(Ldir));
            Vec3 H = (Ldir + Vdir).normalized();
            float spec_pow = std::pow(std::max(0.0f, N.dot(H)), M->shininess);
            Vec3 spec = M->metallic > 0.5f ? L->color * Vec3(base.x,base.y,base.z) : L->color;
            Vec3 diff = base * L->color;
            color = color + (diff * ndl + spec * spec_pow * (1.0f - M->roughness)) * (L->intensity * atten);
        }
        // fog
        float dist = (world_pos - cam->position).length();
        if (fog_enabled) {
            float t = std::clamp((dist - fog_start) / std::max(1e-3f, fog_end - fog_start), 0.0f, 1.0f);
            color = color * (1 - t) + fog_color * t;
        }
        return color;
    };

    // 每节点变换并光栅化
    for (auto& d : draw) {
        auto& mesh = d.node->mesh;
        auto& mat = mesh->material;
        if (!mat) continue;
        Mat4 W = d.node->world_matrix();
        Mat4 WVP = VP * W;
        // 法线矩阵（不考虑非均匀缩放的近似）
        Mat4 NMat = W;

        auto index_count = mesh->indices.empty() ? mesh->vertices.size() : mesh->indices.size();
        auto get_idx = [&](size_t k) -> unsigned {
            return mesh->indices.empty() ? (unsigned)k : mesh->indices[k];
        };
        for (size_t tri = 0; tri + 2 < index_count; tri += 3) {
            unsigned ia = get_idx(tri), ib = get_idx(tri+1), ic = get_idx(tri+2);
            auto& VA = mesh->vertices[ia];
            auto& VB = mesh->vertices[ib];
            auto& VC = mesh->vertices[ic];
            Vec3 wA = W.transform(VA.pos);
            Vec3 wB = W.transform(VB.pos);
            Vec3 wC = W.transform(VC.pos);
            Vec3 cA = WVP.transform(VA.pos);
            Vec3 cB = WVP.transform(VB.pos);
            Vec3 cC = WVP.transform(VC.pos);
            // backface cull (camera space normal.z < 0 => visible for CCW)
            // 跳过近裁剪
            if (cA.z < -1 || cB.z < -1 || cC.z < -1) continue;
            if (cA.z > 1 || cB.z > 1 || cC.z > 1) continue;
            // NDC -> screen
            auto to_screen = [&](const Vec3& p) -> Vec3 {
                return { (p.x+1)*0.5f*w, (1-p.y)*0.5f*h, (p.z+1)*0.5f };
            };
            Vec3 sA = to_screen(cA);
            Vec3 sB = to_screen(cB);
            Vec3 sC = to_screen(cC);

            // triangle bbox
            int minx = std::max(0, (int)std::floor(std::min({sA.x, sB.x, sC.x})));
            int maxx = std::min(w-1, (int)std::ceil(std::max({sA.x, sB.x, sC.x})));
            int miny = std::max(0, (int)std::floor(std::min({sA.y, sB.y, sC.y})));
            int maxy = std::min(h-1, (int)std::ceil(std::max({sA.y, sB.y, sC.y})));
            if (minx > maxx || miny > maxy) continue;

            Vec3 nA = NMat.transform_dir(VA.normal).normalized();
            Vec3 nB = NMat.transform_dir(VB.normal).normalized();
            Vec3 nC = NMat.transform_dir(VC.normal).normalized();
            // 背面剔除：屏幕面积<0
            float area = (sB.x - sA.x) * (sC.y - sA.y) - (sC.x - sA.x) * (sB.y - sA.y);
            if (area <= 0) continue;

            for (int y = miny; y <= maxy; ++y) {
                for (int x = minx; x <= maxx; ++x) {
                    Vec3 p{(float)x+0.5f, (float)y+0.5f, 0};
                    Vec3 bc = barycentric(p, sA, sB, sC);
                    if (bc.x < 0 || bc.y < 0 || bc.z < 0) continue;
                    float z = bc.x*sA.z + bc.y*sB.z + bc.z*sC.z;
                    size_t zidx = (size_t)y * w + x;
                    if (z >= zbuf[zidx]) continue;
                    zbuf[zidx] = z;

                    Vec3 wpos = wA*bc.x + wB*bc.y + wC*bc.z;
                    Vec3 N    = (nA*bc.x + nB*bc.y + nC*bc.z).normalized();
                    Vec2 uv   = {VA.uv.u*bc.x + VB.uv.u*bc.y + VC.uv.u*bc.z,
                                 VA.uv.v*bc.x + VB.uv.v*bc.y + VC.uv.v*bc.z};
                    Vec3 col = shade(wpos, N, mat, uv);
                    int R = std::clamp((int)(col.x*255), 0, 255);
                    int G = std::clamp((int)(col.y*255), 0, 255);
                    int B = std::clamp((int)(col.z*255), 0, 255);
                    img->set_pixel(x, y, R, G, B);
                }
            }
        }
    }
    return img;
}

bool Scene::render_gpu(const CameraPtr&, int, int, unsigned int) const {
    // GPU 钩子：如果集成了 OpenGL 上下文，这里负责 VAO / shader / FBO 绑定
    // 以及 CUDA-OpenGL 互操作资源注册。未启用 OpenGL 时直接返回 false。
#if defined(VORTEX_WITH_OPENGL_REAL)
    // GPU 管线实现占位：真实项目会在此调用 glDrawElements 并返回 true
#endif
    return false;
}
bool Scene::is_gpu_available() {
#if defined(VORTEX_WITH_OPENGL_REAL)
    return true;
#endif
    return false;
}
std::shared_ptr<game2d::Image> Scene::render(const CameraPtr& cam, int w, int h) {
    if (is_gpu_available()) {
        // GPU + FBO -> 回读（此处示意，默认走 CPU fallback）
        if (render_gpu(cam, w, h)) return render_software(cam, w, h); // 占位：GPU 路径成功后也可返回空 frame
    }
    return render_software(cam, w, h);
}

} // namespace render3d

// ============================================================
// 模块注册：render3d
// ============================================================
void register_render3d_module(std::unordered_map<std::string, ValuePtr>& std_modules) {
    using namespace render3d;
    auto mod = Value::make_module();
    auto& u = *mod->module_rep;

    auto mk_fn = [](const std::string& mname, const std::string& name, size_t min_a, size_t max_a,
                    std::function<ValuePtr(const ValueVec&)> fn) {
        auto fv = std::make_shared<FunctionValue>();
        fv->name = name; fv->is_builtin = true;
        fv->builtin_fn = [mname, name, min_a, max_a, fn](const ValueVec& args, Environment&) -> ValuePtr {
            if (args.size() < min_a || (max_a != (size_t)-1 && args.size() > max_a))
                throw RuntimeError(mname + "." + name + " expects " +
                    std::to_string(min_a) + "~" + std::to_string(max_a) + " args, got " +
                    std::to_string(args.size()));
            return fn(args);
        };
        auto v = Value::make_none(); v->type = ValueType::Function; v->fn_rep = fv;
        return v;
    };
    auto I = [](const ValuePtr& v) { return value_to_int(v)->int_val; };
    auto D = [](const ValuePtr& v) { return value_to_float(v)->float_val; };

    // ---- 资源解包 ----
    using OpaquePtr = std::shared_ptr<OpaqueResource>;
    auto wrap = [&](const std::string& kind, auto payload) {
        auto r = std::make_shared<OpaqueResource>(kind, std::move(payload));
        return Value::make_opaque(r);
    };
    auto as_mesh = [](const ValuePtr& v) -> MeshPtr {
        if (v->type != ValueType::Opaque || !v->opaque_rep || v->opaque_rep->kind != "r3d_mesh")
            throw RuntimeError("expected r3d_mesh");
        auto* p = v->opaque_rep->try_as<MeshPtr>();
        return p ? *p : nullptr;
    };
    auto as_node = [](const ValuePtr& v) -> NodePtr {
        if (v->type != ValueType::Opaque || !v->opaque_rep || v->opaque_rep->kind != "r3d_node")
            throw RuntimeError("expected r3d_node");
        auto* p = v->opaque_rep->try_as<NodePtr>();
        return p ? *p : nullptr;
    };
    auto as_scene = [](const ValuePtr& v) -> ScenePtr {
        if (v->type != ValueType::Opaque || !v->opaque_rep || v->opaque_rep->kind != "r3d_scene")
            throw RuntimeError("expected r3d_scene");
        auto* p = v->opaque_rep->try_as<ScenePtr>();
        return p ? *p : nullptr;
    };
    auto as_cam = [](const ValuePtr& v) -> CameraPtr {
        if (v->type != ValueType::Opaque || !v->opaque_rep || v->opaque_rep->kind != "r3d_camera")
            throw RuntimeError("expected r3d_camera");
        auto* p = v->opaque_rep->try_as<CameraPtr>();
        return p ? *p : nullptr;
    };
    auto as_mat = [](const ValuePtr& v) -> MaterialPtr {
        if (v->type != ValueType::Opaque || !v->opaque_rep || v->opaque_rep->kind != "r3d_material")
            throw RuntimeError("expected r3d_material");
        auto* p = v->opaque_rep->try_as<MaterialPtr>();
        return p ? *p : nullptr;
    };
    auto as_light = [](const ValuePtr& v) -> LightPtr {
        if (v->type != ValueType::Opaque || !v->opaque_rep || v->opaque_rep->kind != "r3d_light")
            throw RuntimeError("expected r3d_light");
        auto* p = v->opaque_rep->try_as<LightPtr>();
        return p ? *p : nullptr;
    };
    auto as_tex = [](const ValuePtr& v) -> TexturePtr {
        if (v->type != ValueType::Opaque || !v->opaque_rep || v->opaque_rep->kind != "r3d_texture")
            throw RuntimeError("expected r3d_texture");
        auto* p = v->opaque_rep->try_as<TexturePtr>();
        return p ? *p : nullptr;
    };

    auto add = [&](const std::string& n, size_t a0, size_t a1,
                   std::function<ValuePtr(const ValueVec&)> f) {
        u[n] = mk_fn("render3d", n, a0, a1, std::move(f));
    };

    // ---- 功能标志 ----
    u["gpu_available"] = Value::make_bool(Scene::is_gpu_available());

    // ---- Scene ----
    add("new_scene", 0, 0, [&](const ValueVec&) {
        auto s = std::make_shared<Scene>();
        // 默认添加一盏方向光
        auto L = std::make_shared<Light>();
        L->kind = LightKind::Directional;
        L->position_or_direction = Vec3(-0.5f, -1.0f, -0.3f).normalized();
        L->intensity = 1.0f;
        s->lights.push_back(L);
        return wrap("r3d_scene", s);
    });
    add("scene_add_node", 2, 2, [&](const ValueVec& a) {
        as_scene(a[0])->roots.push_back(as_node(a[1])); return Value::make_none();
    });
    add("scene_add_light", 2, 2, [&](const ValueVec& a) {
        as_scene(a[0])->lights.push_back(as_light(a[1])); return Value::make_none();
    });
    add("scene_set_ambient", 4, 4, [&](const ValueVec& a) {
        auto s = as_scene(a[0]);
        s->ambient = Vec3((float)D(a[1]), (float)D(a[2]), (float)D(a[3]));
        return Value::make_none();
    });
    add("scene_set_fog", 4, 5, [&](const ValueVec& a) {
        auto s = as_scene(a[0]);
        s->fog_enabled = a[1]->truthy();
        s->fog_start = (float)D(a[2]);
        s->fog_end   = (float)D(a[3]);
        if (a.size() >= 5 && a[4]->type == ValueType::List && a[4]->list_rep->size() >= 3) {
            std::vector<ValuePtr> v(a[4]->list_rep->begin(), a[4]->list_rep->end());
            s->fog_color = Vec3((float)D(v[0]), (float)D(v[1]), (float)D(v[2]));
        }
        return Value::make_none();
    });

    // ---- Camera ----
    add("new_camera_perspective", 0, 0, [&](const ValueVec&) { return wrap("r3d_camera", std::make_shared<Camera>()); });
    add("new_camera_ortho", 0, 0, [&](const ValueVec&) {
        auto c = std::make_shared<Camera>();
        c->kind = CameraKind::Orthographic; return wrap("r3d_camera", c);
    });
    add("camera_set", 2, 7, [&](const ValueVec& a) {
        auto c = as_cam(a[0]);
        if (a.size() >= 5) {
            c->position = Vec3((float)D(a[1]),(float)D(a[2]),(float)D(a[3]));
            c->target   = Vec3((float)D(a[4]),(float)0,(float)0);
            if (a.size() >= 7) c->target = Vec3((float)D(a[4]),(float)D(a[5]),(float)D(a[6]));
        }
        return Value::make_none();
    });
    add("camera_lookat", 7, 7, [&](const ValueVec& a) {
        auto c = as_cam(a[0]);
        c->position = Vec3((float)D(a[1]),(float)D(a[2]),(float)D(a[3]));
        c->target   = Vec3((float)D(a[4]),(float)D(a[5]),(float)D(a[6]));
        return Value::make_none();
    });
    add("camera_fov", 1, 2, [&](const ValueVec& a) {
        auto c = as_cam(a[0]);
        if (a.size() >= 2) { c->fov_deg = (float)D(a[1]); return Value::make_none(); }
        return Value::make_float(c->fov_deg);
    });

    // ---- Material ----
    add("new_material", 0, 5, [&](const ValueVec& a) {
        auto m = std::make_shared<Material>();
        if (a.size() >= 3) m->albedo = Vec3((float)D(a[0]),(float)D(a[1]),(float)D(a[2]));
        if (a.size() >= 4) m->metallic = (float)D(a[3]);
        if (a.size() >= 5) m->roughness = (float)D(a[4]);
        return wrap("r3d_material", m);
    });
    add("material_set_texture", 2, 2, [&](const ValueVec& a) {
        as_mat(a[0])->texture = as_tex(a[1]); return Value::make_none();
    });

    // ---- Texture ----
    add("new_texture_solid", 5, 5, [&](const ValueVec& a) {
        auto t = std::make_shared<Texture>((int)I(a[0]),(int)I(a[1]));
        int r=(int)I(a[2]), g=(int)I(a[3]), b=(int)I(a[4]);
        for (size_t i=0; i<t->rgba.size(); i+=4) {
            t->rgba[i]=(unsigned char)r; t->rgba[i+1]=(unsigned char)g; t->rgba[i+2]=(unsigned char)b; t->rgba[i+3]=255;
        }
        return wrap("r3d_texture", t);
    });
    add("new_texture_checker", 4, 4, [&](const ValueVec& a) {
        int w=(int)I(a[0]), h=(int)I(a[1]), tile=std::max(1,(int)I(a[2]));
        auto t = std::make_shared<Texture>(w, h);
        int r0=255, g0=255, b0=255;
        if (a[3]->type == ValueType::List && a[3]->list_rep->size() >= 3) {
            std::vector<ValuePtr> v(a[3]->list_rep->begin(), a[3]->list_rep->end());
            r0 = (int)D(v[0]); g0 = (int)D(v[1]); b0 = (int)D(v[2]);
        }
        for (int y=0;y<h;++y) for (int x=0;x<w;++x) {
            bool d = ((x/tile)+(y/tile))%2==0;
            size_t o=((size_t)y*w+x)*4;
            if (d) { t->rgba[o]=(unsigned char)r0; t->rgba[o+1]=(unsigned char)g0; t->rgba[o+2]=(unsigned char)b0; }
            else   { t->rgba[o]=0x22; t->rgba[o+1]=0x22; t->rgba[o+2]=0x22; }
            t->rgba[o+3]=255;
        }
        return wrap("r3d_texture", t);
    });

    // ---- Mesh ----
    add("mesh_box", 3, 4, [&](const ValueVec& a) {
        MaterialPtr m = a.size() >= 4 ? as_mat(a[3]) : nullptr;
        return wrap("r3d_mesh", Mesh::make_box((float)D(a[0]),(float)D(a[1]),(float)D(a[2]),m));
    });
    add("mesh_sphere", 1, 4, [&](const ValueVec& a) {
        int st = a.size()>=3 ? (int)I(a[1]) : 24;
        int sl = a.size()>=3 ? (int)I(a[2]) : 32;
        MaterialPtr m = a.size() >= 4 ? as_mat(a[3]) : nullptr;
        return wrap("r3d_mesh", Mesh::make_sphere((float)D(a[0]), st, sl, m));
    });
    add("mesh_plane", 2, 3, [&](const ValueVec& a) {
        int subd = a.size()>=3 ? (int)I(a[2]) : 4;
        MaterialPtr m = a.size() >= 4 ? as_mat(a[3]) : nullptr;
        return wrap("r3d_mesh", Mesh::make_plane((float)D(a[0]), subd, m));
    });
    add("mesh_cylinder", 2, 4, [&](const ValueVec& a) {
        int sl = a.size()>=3 ? (int)I(a[2]) : 24;
        MaterialPtr m = a.size() >= 4 ? as_mat(a[3]) : nullptr;
        return wrap("r3d_mesh", Mesh::make_cylinder((float)D(a[0]),(float)D(a[1]),sl,m));
    });

    // ---- Node ----
    add("new_node", 0, 1, [&](const ValueVec& a) {
        auto n = std::make_shared<Node>();
        if (a.size() >= 1) n->mesh = as_mesh(a[0]);
        return wrap("r3d_node", n);
    });
    add("node_attach_mesh", 2, 2, [&](const ValueVec& a) {
        as_node(a[0])->mesh = as_mesh(a[1]); return Value::make_none();
    });
    add("node_set_pos", 4, 4, [&](const ValueVec& a) {
        auto n = as_node(a[0]);
        n->pos = Vec3((float)D(a[1]),(float)D(a[2]),(float)D(a[3]));
        return Value::make_none();
    });
    add("node_set_rot", 4, 4, [&](const ValueVec& a) {
        auto n = as_node(a[0]);
        n->euler = Vec3((float)D(a[1]),(float)D(a[2]),(float)D(a[3]));
        return Value::make_none();
    });
    add("node_set_scale", 4, 4, [&](const ValueVec& a) {
        auto n = as_node(a[0]);
        n->scale3 = Vec3((float)D(a[1]),(float)D(a[2]),(float)D(a[3]));
        return Value::make_none();
    });
    add("node_add_child", 2, 2, [&](const ValueVec& a) {
        as_node(a[0])->add_child(as_node(a[1])); return Value::make_none();
    });

    // ---- Light ----
    add("new_light_dir", 4, 5, [&](const ValueVec& a) {
        auto L = std::make_shared<Light>();
        L->kind = LightKind::Directional;
        L->position_or_direction = Vec3((float)D(a[0]),(float)D(a[1]),(float)D(a[2])).normalized();
        L->intensity = a.size()>=5 ? (float)D(a[4]) : 1.0f;
        L->color = Vec3(1,1,1);
        if (a.size()>=4 && a[3]->type == ValueType::List && a[3]->list_rep->size()>=3) {
            std::vector<ValuePtr> v(a[3]->list_rep->begin(), a[3]->list_rep->end());
            L->color = Vec3((float)D(v[0]),(float)D(v[1]),(float)D(v[2]));
        }
        return wrap("r3d_light", L);
    });
    add("new_light_point", 4, 6, [&](const ValueVec& a) {
        auto L = std::make_shared<Light>();
        L->kind = LightKind::Point;
        L->position_or_direction = Vec3((float)D(a[0]),(float)D(a[1]),(float)D(a[2]));
        L->intensity = a.size()>=5 ? (float)D(a[4]) : 1.0f;
        L->range = a.size()>=6 ? (float)D(a[5]) : 100.0f;
        if (a.size()>=4 && a[3]->type == ValueType::List && a[3]->list_rep->size()>=3) {
            std::vector<ValuePtr> v(a[3]->list_rep->begin(), a[3]->list_rep->end());
            L->color = Vec3((float)D(v[0]),(float)D(v[1]),(float)D(v[2]));
        }
        return wrap("r3d_light", L);
    });

    // ---- Render ----
    // render(scene, camera, width, height) -> image (opaque g2d_image)
    add("render", 4, 4, [&](const ValueVec& a) {
        auto s = as_scene(a[0]);
        auto c = as_cam(a[1]);
        int w = (int)I(a[2]);
        int h = (int)I(a[3]);
        auto img = s->render(c, w, h);
        auto res = std::make_shared<OpaqueResource>("g2d_image", img);
        return Value::make_opaque(res);
    });
    // render_to_window(scene, camera, width, height) — 将画面提交到 game2d 窗口
    add("render_to_window", 4, 4, [&](const ValueVec& a) {
        auto s = as_scene(a[0]);
        auto c = as_cam(a[1]);
        int w = (int)I(a[2]);
        int h = (int)I(a[3]);
        std::shared_ptr<game2d::Image> img = s->render(c, w, h);
        // 提交到 game2d framebuffer（若存在尺寸匹配则直接替换）
        auto& g = game2d::Game::instance();
        g.ensure_window(w, h, g.title);
        g.framebuffer = img;
        return Value::make_none();
    });

    std_modules["render3d"] = mod;
}

} // namespace vortex
