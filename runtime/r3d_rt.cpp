// ============================================================
// r3d_rt.cpp — render3d 模块转发（编译 exe 侧，纯 C++ CPU 光栅化）
// 抽取自 vortex_core 的 render3d_module.cpp 核心算法。
// ============================================================
#include "r3d_g2d.h"

namespace vor {

// ---------- Mat4 ----------
Mat4 Mat4::perspective(float fovy,float asp,float n,float f){
    Mat4 r; float th=std::tan(fovy/2.0f);
    r.m[0]=1.0f/(asp*th); r.m[5]=1.0f/th;
    r.m[10]=(f+n)/(n-f); r.m[11]=-1.0f; r.m[14]=(2*f*n)/(n-f); r.m[15]=0.0f; return r;
}
Mat4 Mat4::ortho(float l,float ri,float b,float t,float n,float f){
    Mat4 r;
    r.m[0]=2/(ri-l); r.m[5]=2/(t-b); r.m[10]=-2/(f-n);
    r.m[12]=-(ri+l)/(ri-l); r.m[13]=-(t+b)/(t-b); r.m[14]=-(f+n)/(f-n); return r;
}
Mat4 Mat4::lookAt(const Vec3& e,const Vec3& c,const Vec3& up){
    Vec3 f=(c-e).normalized(), s=f.cross(up).normalized(), u=s.cross(f); Mat4 r;
    r.m[0]=s.x; r.m[4]=s.y; r.m[8]=s.z; r.m[1]=u.x; r.m[5]=u.y; r.m[9]=u.z;
    r.m[2]=-f.x; r.m[6]=-f.y; r.m[10]=-f.z;
    r.m[12]=-s.dot(e); r.m[13]=-u.dot(e); r.m[14]=f.dot(e); return r;
}
Mat4 Mat4::translate(const Vec3& t){ Mat4 r; r.m[12]=t.x; r.m[13]=t.y; r.m[14]=t.z; return r; }
Mat4 Mat4::scale(const Vec3& s){ Mat4 r; r.m[0]=s.x; r.m[5]=s.y; r.m[10]=s.z; return r; }
Mat4 Mat4::rotateX(float a){ Mat4 r; float c=std::cos(a),s=std::sin(a); r.m[5]=c; r.m[6]=-s; r.m[9]=s; r.m[10]=c; return r; }
Mat4 Mat4::rotateY(float a){ Mat4 r; float c=std::cos(a),s=std::sin(a); r.m[0]=c; r.m[2]=s; r.m[8]=-s; r.m[10]=c; return r; }
Mat4 Mat4::rotateZ(float a){ Mat4 r; float c=std::cos(a),s=std::sin(a); r.m[0]=c; r.m[1]=-s; r.m[4]=s; r.m[5]=c; return r; }
Mat4 Mat4::operator*(const Mat4&o)const{
    Mat4 r; for(int i=0;i<4;++i) for(int j=0;j<4;++j){ r.m[i+j*4]=0; for(int k=0;k<4;++k) r.m[i+j*4]+=m[i+k*4]*o.m[k+j*4]; } return r;
}
Vec3 Mat4::transform(const Vec3&p)const{
    float w=m[3]*p.x+m[7]*p.y+m[11]*p.z+m[15];
    float x=m[0]*p.x+m[4]*p.y+m[8]*p.z+m[12];
    float y=m[1]*p.x+m[5]*p.y+m[9]*p.z+m[13];
    float z=m[2]*p.x+m[6]*p.y+m[10]*p.z+m[14];
    if(w!=1.0f&&w!=0.0f){x/=w;y/=w;z/=w;} return {x,y,z};
}
Vec3 Mat4::transform_dir(const Vec3&d)const{
    float x=m[0]*d.x+m[4]*d.y+m[8]*d.z;
    float y=m[1]*d.x+m[5]*d.y+m[9]*d.z;
    float z=m[2]*d.x+m[6]*d.y+m[10]*d.z; return {x,y,z};
}

void Texture::sample(float u,float v,int&r,int&g,int&b,int&a)const{
    u=u-std::floor(u); v=v-std::floor(v);
    int x=std::clamp((int)(u*width),0,width-1), y=std::clamp((int)((1.0f-v)*height),0,height-1);
    size_t o=((size_t)y*width+x)*4; r=rgba[o]; g=rgba[o+1]; b=rgba[o+2]; a=rgba[o+3];
}

// ---------- Mesh ----------
MeshPtr Mesh::make_box(float sx,float sy,float sz,MaterialPtr mat){
    auto m=std::make_shared<Mesh>();
    m->material=mat?mat:std::make_shared<Material>();
    float x=sx/2,y=sy/2,z=sz/2;
    auto face=[&](const Vec3&a,const Vec3&b,const Vec3&c,const Vec3&d,const Vec3&n){
        size_t base=m->vertices.size();
        m->vertices.push_back({a,n,{0,0}}); m->vertices.push_back({b,n,{1,0}});
        m->vertices.push_back({c,n,{1,1}}); m->vertices.push_back({d,n,{0,1}});
        m->indices.push_back((unsigned)(base+0)); m->indices.push_back((unsigned)(base+1)); m->indices.push_back((unsigned)(base+2));
        m->indices.push_back((unsigned)(base+0)); m->indices.push_back((unsigned)(base+2)); m->indices.push_back((unsigned)(base+3));
    };
    face({x,-y,-z},{x,y,-z},{x,y,z},{x,-y,z},{1,0,0});
    face({-x,-y,z},{-x,y,z},{-x,y,-z},{-x,-y,-z},{-1,0,0});
    face({-x,y,-z},{-x,y,z},{x,y,z},{x,y,-z},{0,1,0});
    face({-x,-y,z},{-x,-y,-z},{x,-y,-z},{x,-y,z},{0,-1,0});
    face({-x,-y,z},{x,-y,z},{x,y,z},{-x,y,z},{0,0,1});
    face({x,-y,-z},{-x,-y,-z},{-x,y,-z},{x,y,-z},{0,0,-1});
    return m;
}
MeshPtr Mesh::make_sphere(float r,int stacks,int slices,MaterialPtr mat){
    auto m=std::make_shared<Mesh>();
    m->material=mat?mat:std::make_shared<Material>();
    for(int i=0;i<=stacks;++i){ float t=(float)i/stacks, phi=3.14159265f*t;
        for(int j=0;j<=slices;++j){ float s=(float)j/slices, th=2.0f*3.14159265f*s;
            float x=r*std::sin(phi)*std::cos(th), yy=r*std::cos(phi), zz=r*std::sin(phi)*std::sin(th);
            Mesh::Vertex v; v.pos={x,yy,zz}; v.normal=Vec3(x,yy,zz).normalized(); v.uv={s,t}; m->vertices.push_back(v);
        }
    }
    for(int i=0;i<stacks;++i) for(int j=0;j<slices;++j){
        unsigned a=(unsigned)(i*(slices+1)+j), b=a+(unsigned)(slices+1);
        m->indices.push_back(a); m->indices.push_back(b); m->indices.push_back(a+1);
        m->indices.push_back(b); m->indices.push_back(b+1); m->indices.push_back(a+1);
    }
    return m;
}
MeshPtr Mesh::make_plane(float size,int subdiv,MaterialPtr mat){
    auto m=std::make_shared<Mesh>();
    m->material=mat?mat:std::make_shared<Material>();
    int n=subdiv+1; float step=size/(float)subdiv;
    for(int j=0;j<n;++j) for(int i=0;i<n;++i){
        Mesh::Vertex v; v.pos={-size/2+i*step,0,-size/2+j*step}; v.normal={0,1,0}; v.uv={(float)i/subdiv,(float)j/subdiv}; m->vertices.push_back(v);
    }
    for(int j=0;j<subdiv;++j) for(int i=0;i<subdiv;++i){
        unsigned a=(unsigned)(j*n+i), b=a+(unsigned)n;
        m->indices.push_back(a); m->indices.push_back(b); m->indices.push_back(a+1);
        m->indices.push_back(b); m->indices.push_back(b+1); m->indices.push_back(a+1);
    }
    return m;
}
MeshPtr Mesh::make_cylinder(float radius,float h,int slices,MaterialPtr mat){
    auto m=std::make_shared<Mesh>();
    m->material=mat?mat:std::make_shared<Material>();
    float y0=-h/2,y1=h/2;
    for(int i=0;i<=slices;++i){ float a=2.0f*3.14159265f*(float)i/slices; float x=radius*std::cos(a),z=radius*std::sin(a);
        Vec3 n=Vec3(x,0,z).normalized(); float u=(float)i/slices;
        m->vertices.push_back({{x,y0,z},n,{u,0}}); m->vertices.push_back({{x,y1,z},n,{u,1}});
    }
    for(int i=0;i<slices;++i){ unsigned a=(unsigned)i*2;
        m->indices.push_back(a); m->indices.push_back(a+1); m->indices.push_back(a+2);
        m->indices.push_back(a+1); m->indices.push_back(a+3); m->indices.push_back(a+2);
    }
    unsigned bt=(unsigned)m->vertices.size(); m->vertices.push_back({{0,y1,0},{0,1,0},{0.5f,0.5f}});
    for(int i=0;i<=slices;++i){ float a=2.0f*3.14159265f*(float)i/slices; m->vertices.push_back({{radius*std::cos(a),y1,radius*std::sin(a)},{0,1,0},{0.5f+0.5f*std::cos(a),0.5f+0.5f*std::sin(a)}}); }
    for(int i=0;i<slices;++i){ m->indices.push_back(bt); m->indices.push_back(bt+1+i); m->indices.push_back(bt+2+i); }
    unsigned bb=(unsigned)m->vertices.size(); m->vertices.push_back({{0,y0,0},{0,-1,0},{0.5f,0.5f}});
    for(int i=0;i<=slices;++i){ float a=2.0f*3.14159265f*(float)i/slices; m->vertices.push_back({{radius*std::cos(a),y0,radius*std::sin(a)},{0,-1,0},{0.5f+0.5f*std::cos(a),0.5f+0.5f*std::sin(a)}}); }
    for(int i=0;i<slices;++i){ m->indices.push_back(bb); m->indices.push_back(bb+2+i); m->indices.push_back(bb+1+i); }
    return m;
}

// ---------- Node ----------
Mat4 Node::local_matrix()const{
    Mat4 T=Mat4::translate(pos);
    Mat4 Rx=Mat4::rotateX(euler.x*3.14159265f/180.0f), Ry=Mat4::rotateY(euler.y*3.14159265f/180.0f), Rz=Mat4::rotateZ(euler.z*3.14159265f/180.0f);
    Mat4 S=Mat4::scale(scale3);
    return T*Ry*Rx*Rz*S;
}
Mat4 Node::world_matrix()const{ auto p=parent.lock(); if(!p) return local_matrix(); return p->world_matrix()*local_matrix(); }

// ---------- Scene render ----------
static inline Vec3 bary(Vec3 p,Vec3 a,Vec3 b,Vec3 c){
    Vec3 v0=b-a,v1=c-a,v2=p-a;
    float d00=v0.dot(v0),d01=v0.dot(v1),d11=v1.dot(v1),d20=v2.dot(v0),d21=v2.dot(v1);
    float denom=d00*d11-d01*d01; if(std::fabs(denom)<1e-8f) return {-1,0,0};
    float v=(d11*d20-d01*d21)/denom, w=(d00*d21-d01*d20)/denom, u=1.0f-v-w; return {u,v,w};
}

std::shared_ptr<Image> Scene::render(const CameraPtr& cam,int w,int h) const {
    auto img=std::make_shared<Image>(w,h);
    img->fill((int)(fog_color.x*255),(int)(fog_color.y*255),(int)(fog_color.z*255));
    std::vector<float> zbuf((size_t)w*h,1e18f);
    Mat4 V=cam->view(), P=cam->proj(w,h), VP=P*V;
    std::vector<NodePtr> draw;
    std::function<void(const NodePtr&)> collect=[&](const NodePtr& n){ if(!n) return; if(n->mesh) draw.push_back(n); for(auto&c:n->children) collect(c); };
    for(auto&r:roots) collect(r);
    auto shade=[&](const Vec3& wp,const Vec3& N,const MaterialPtr& M,const Vec2& uv)->Vec3{
        Vec3 base=M->albedo;
        if(M->texture){ int tr,tg,tb,ta; M->texture->sample(uv.u,uv.v,tr,tg,tb,ta); base.x*=tr/255.0f; base.y*=tg/255.0f; base.z*=tb/255.0f; }
        Vec3 color=Vec3(base.x*ambient.x,base.y*ambient.y,base.z*ambient.z);
        Vec3 Vdir=(cam->position-wp).normalized();
        for(auto&L:lights){
            Vec3 Ldir; float atten=1.0f;
            if(L->kind==LightKind::Directional) Ldir=L->pd.normalized();
            else { Ldir=L->pd-wp; float d=Ldir.length(); Ldir=Ldir/std::max(d,1e-6f); atten=1.0f/(1.0f+0.09f*d+0.032f*d*d); if(d>L->range) atten=0; }
            float ndl=std::max(0.0f,N.dot(Ldir));
            Vec3 H=(Ldir+Vdir).normalized();
            float spec_pow=std::pow(std::max(0.0f,N.dot(H)),M->shininess);
            Vec3 spec=M->metallic>0.5f?L->color*Vec3(base.x,base.y,base.z):L->color;
            Vec3 diff=base*L->color;
            color=color+(diff*ndl+spec*spec_pow*(1.0f-M->roughness))*(L->intensity*atten);
        }
        float dist=(wp-cam->position).length();
        if(fog_enabled){ float t=std::clamp((dist-fog_start)/std::max(1e-3f,fog_end-fog_start),0.0f,1.0f); color=color*(1-t)+fog_color*t; }
        return color;
    };
    for(auto&dn:draw){
        auto&mesh=dn->mesh; auto&mat=mesh->material; if(!mat) continue;
        Mat4 W=dn->world_matrix(), WVP=VP*W, NMat=W;
        auto index_count=mesh->indices.empty()?mesh->vertices.size():mesh->indices.size();
        auto get_idx=[&](size_t k)->unsigned{ return mesh->indices.empty()?(unsigned)k:mesh->indices[k]; };
        for(size_t tri=0;tri+2<index_count;tri+=3){
            unsigned ia=get_idx(tri),ib=get_idx(tri+1),ic=get_idx(tri+2);
            auto&VA=mesh->vertices[ia]; auto&VB=mesh->vertices[ib]; auto&VC=mesh->vertices[ic];
            Vec3 wA=W.transform(VA.pos),wB=W.transform(VB.pos),wC=W.transform(VC.pos);
            Vec3 cA=WVP.transform(VA.pos),cB=WVP.transform(VB.pos),cC=WVP.transform(VC.pos);
            if(cA.z<-1||cB.z<-1||cC.z<-1) continue; if(cA.z>1||cB.z>1||cC.z>1) continue;
            auto ts=[&](const Vec3&p)->Vec3{ return {(p.x+1)*0.5f*w,(1-p.y)*0.5f*h,(p.z+1)*0.5f}; };
            Vec3 sA=ts(cA),sB=ts(cB),sC=ts(cC);
            int minx=std::max(0,(int)std::floor(std::min({sA.x,sB.x,sC.x}))), maxx=std::min(w-1,(int)std::ceil(std::max({sA.x,sB.x,sC.x})));
            int miny=std::max(0,(int)std::floor(std::min({sA.y,sB.y,sC.y}))), maxy=std::min(h-1,(int)std::ceil(std::max({sA.y,sB.y,sC.y})));
            if(minx>maxx||miny>maxy) continue;
            Vec3 nA=NMat.transform_dir(VA.normal).normalized(), nB=NMat.transform_dir(VB.normal).normalized(), nC=NMat.transform_dir(VC.normal).normalized();
            float area=(sB.x-sA.x)*(sC.y-sA.y)-(sC.x-sA.x)*(sB.y-sA.y); if(area<=0) continue;
            for(int y=miny;y<=maxy;++y) for(int x=minx;x<=maxx;++x){
                Vec3 p{(float)x+0.5f,(float)y+0.5f,0}; Vec3 bc=bary(p,sA,sB,sC);
                if(bc.x<0||bc.y<0||bc.z<0) continue;
                float z=bc.x*sA.z+bc.y*sB.z+bc.z*sC.z; size_t zidx=(size_t)y*w+x; if(z>=zbuf[zidx]) continue; zbuf[zidx]=z;
                Vec3 wpos=wA*bc.x+wB*bc.y+wC*bc.z, N=(nA*bc.x+nB*bc.y+nC*bc.z).normalized();
                Vec2 uv={VA.uv.u*bc.x+VB.uv.u*bc.y+VC.uv.u*bc.z, VA.uv.v*bc.x+VB.uv.v*bc.y+VC.uv.v*bc.z};
                Vec3 col=shade(wpos,N,mat,uv);
                img->set_pixel(x,y,std::clamp((int)(col.x*255),0,255),std::clamp((int)(col.y*255),0,255),std::clamp((int)(col.z*255),0,255));
            }
        }
    }
    return img;
}

} // namespace vor

using namespace vor;

static ScenePtr scene_of(void* h){ return rt_from<Scene>(h); }

extern "C" {

void* vor_r3d_new_scene(void){
    auto s=std::make_shared<Scene>();
    auto L=std::make_shared<Light>(); L->kind=LightKind::Directional; L->pd=Vec3(-0.5f,-1.0f,-0.3f).normalized(); L->intensity=1.0f;
    s->lights.push_back(L);
    return rt_store(s);
}
void vor_r3d_scene_add_node(void* sc,void* n){ auto s=scene_of(sc); if(s&&n) s->roots.push_back(rt_from<Node>(n)); }
void vor_r3d_scene_add_light(void* sc,void* l){ auto s=scene_of(sc); if(s&&l) s->lights.push_back(rt_from<Light>(l)); }
void vor_r3d_scene_set_ambient(void* sc,double r,double g,double b){ auto s=scene_of(sc); if(s) s->ambient=Vec3((float)r,(float)g,(float)b); }
void vor_r3d_scene_set_fog(void* sc,int on,double start,double end,long long hasColor,double cr,double cg,double cb){
    auto s=scene_of(sc); if(!s) return;
    s->fog_enabled=on!=0; s->fog_start=(float)start; s->fog_end=(float)end;
    if(hasColor) s->fog_color=Vec3((float)cr,(float)cg,(float)cb);
}
void* vor_r3d_new_camera_persp(void){ return rt_store(std::make_shared<Camera>()); }
void* vor_r3d_new_camera_ortho(void){ auto c=std::make_shared<Camera>(); c->kind=CamKind::Orthographic; return rt_store(c); }
void vor_r3d_camera_lookat(void* c,double px,double py,double pz,double tx,double ty,double tz){ auto cam=rt_from<Camera>(c); if(cam){cam->position={ (float)px,(float)py,(float)pz}; cam->target={(float)tx,(float)ty,(float)tz};} }
void vor_r3d_camera_set(void* c,double px,double py,double pz,double tx,double ty,double tz){ vor_r3d_camera_lookat(c,px,py,pz,tx,ty,tz); }
void vor_r3d_camera_setscope(void* c,double px,double py,double pz,double tx,double ty,double tz, double* up, double fov, double znear, double zfar){ auto cam=rt_from<Camera>(c); if(cam){ cam->position={(float)px,(float)py,(float)pz}; cam->target={(float)tx,(float)ty,(float)tz}; if(up) cam->up={ (float)up[0],(float)up[1],(float)up[2]}; cam->fov_deg=(float)fov; cam->znear=(float)znear; cam->zfar=(float)zfar; } }
double vor_r3d_camera_fov_get(void* c){ auto cam=rt_from<Camera>(c); return cam?cam->fov_deg:0; }
void vor_r3d_camera_fov_set(void* c,double f){ auto cam=rt_from<Camera>(c); if(cam) cam->fov_deg=(float)f; }
void* vor_r3d_new_material(double r,double g,double b,double metallic,double roughness,int have){
    auto m=std::make_shared<Material>();
    if(have>=3) m->albedo=Vec3((float)r,(float)g,(float)b);
    if(have>=4) m->metallic=(float)metallic;
    if(have>=5) m->roughness=(float)roughness;
    return rt_store(m);
}
void vor_r3d_material_set_texture(void* m,void* t){ auto mat=rt_from<Material>(m); if(mat) mat->texture=rt_from<Texture>(t); }
void* vor_r3d_new_texture_solid(long long w,long long h,long long r,long long g,long long b){
    auto t=std::make_shared<Texture>((int)w,(int)h);
    for(size_t i=0;i<t->rgba.size();i+=4){ t->rgba[i]=(unsigned char)r; t->rgba[i+1]=(unsigned char)g; t->rgba[i+2]=(unsigned char)b; t->rgba[i+3]=255; }
    return rt_store(t);
}
void* vor_r3d_new_texture_checker(long long w,long long h,long long tile,long long hasColor,double cr,double cg,double cb){
    auto t=std::make_shared<Texture>((int)w,(int)h);
    int r0=(int)cr,g0=(int)cg,b0=(int)cb;
    for(int y=0;y<h;++y) for(int x=0;x<w;++x){ bool d=((x/(int)tile)+(y/(int)tile))%2==0; size_t o=((size_t)y*w+x)*4;
        if(d){ t->rgba[o]=(unsigned char)r0; t->rgba[o+1]=(unsigned char)g0; t->rgba[o+2]=(unsigned char)b0; }
        else { t->rgba[o]=0x22; t->rgba[o+1]=0x22; t->rgba[o+2]=0x22; }
        t->rgba[o+3]=255; }
    return rt_store(t);
}
void* vor_r3d_mesh_box(double sx,double sy,double sz,void* mat){ return rt_store(Mesh::make_box((float)sx,(float)sy,(float)sz,rt_from<Material>(mat))); }
void* vor_r3d_mesh_sphere(double r,long long st,long long sl,void* mat){ return rt_store(Mesh::make_sphere((float)r,(int)st,(int)sl,rt_from<Material>(mat))); }
void* vor_r3d_mesh_plane(double size,long long subd,void* mat){ return rt_store(Mesh::make_plane((float)size,(int)subd,rt_from<Material>(mat))); }
void* vor_r3d_mesh_cylinder(double r,double h,long long sl,void* mat){ return rt_store(Mesh::make_cylinder((float)r,(float)h,(int)sl,rt_from<Material>(mat))); }
void* vor_r3d_new_node(void* mesh){ NodePtr n=std::make_shared<Node>(); if(mesh) n->mesh=rt_from<Mesh>(mesh); return rt_store(n); }
void vor_r3d_node_attach_mesh(void* n,void* mesh){ auto no=rt_from<Node>(n); if(no) no->mesh=rt_from<Mesh>(mesh); }
void vor_r3d_node_set_pos(void* n,double x,double y,double z){ auto no=rt_from<Node>(n); if(no) no->pos={(float)x,(float)y,(float)z}; }
void vor_r3d_node_set_rot(void* n,double x,double y,double z){ auto no=rt_from<Node>(n); if(no) no->euler={(float)x,(float)y,(float)z}; }
void vor_r3d_node_set_scale(void* n,double x,double y,double z){ auto no=rt_from<Node>(n); if(no) no->scale3={(float)x,(float)y,(float)z}; }
void vor_r3d_node_add_child(void* n,void* c){ auto no=rt_from<Node>(n); if(no&&c) no->add_child(rt_from<Node>(c)); }
void* vor_r3d_new_light_dir(double x,double y,double z,long long hasColor,double cr,double cg,double cb,double intensity){
    auto L=std::make_shared<Light>(); L->kind=LightKind::Directional; L->pd=Vec3((float)x,(float)y,(float)z).normalized(); L->intensity=(float)intensity; L->color={1,1,1};
    if(hasColor) L->color=Vec3((float)cr,(float)cg,(float)cb);
    return rt_store(L);
}
void* vor_r3d_new_light_point(double x,double y,double z,long long hasColor,double cr,double cg,double cb,double intensity,double range){
    auto L=std::make_shared<Light>(); L->kind=LightKind::Point; L->pd={ (float)x,(float)y,(float)z}; L->intensity=(float)intensity; L->range=(float)range; L->color={1,1,1};
    if(hasColor) L->color=Vec3((float)cr,(float)cg,(float)cb);
    return rt_store(L);
}
void* vor_r3d_render(void* sc,void* cam,long long w,long long h){
    auto s=scene_of(sc); auto c=rt_from<Camera>(cam);
    if(!s||!c) return nullptr;
    auto img=s->render(c,(int)w,(int)h);
    return rt_store(img);
}
void vor_r3d_render_to_window(void* sc,void* cam,long long w,long long h){
    auto s=scene_of(sc); auto c=rt_from<Camera>(cam);
    if(!s||!c) return;
    auto img=s->render(c,(int)w,(int)h);
    auto& g=Game2D::instance(); g.ensure_window((int)w,(int)h,g.title); g.framebuffer=img;
}

} // extern "C"