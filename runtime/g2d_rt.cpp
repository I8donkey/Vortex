// ============================================================
// g2d_rt.cpp — game2d 模块转发（编译 exe 侧，纯 C++ headless）
// 实现 visual 绘制、精灵、图像与游戏状态；对象以 void* 句柄传递。
// ============================================================
#include "r3d_g2d.h"
#include "runtime.h"
#include <cstring>

namespace vor {

// ---------- Game2D 绘制 ----------
void Game2D::draw_rect(int x,int y,int w,int h,int r,int g,int b,int a,bool fill){
    if(!framebuffer) return;
    int x0=std::clamp(x,0,framebuffer->width-1), y0=std::clamp(y,0,framebuffer->height-1);
    int x1=std::clamp(x+w-1,0,framebuffer->width-1), y1=std::clamp(y+h-1,0,framebuffer->height-1);
    if(x0>x1||y0>y1) return;
    if(fill){ for(int yy=y0;yy<=y1;++yy) for(int xx=x0;xx<=x1;++xx) framebuffer->set_pixel(xx,yy,r,g,b,a); }
    else {
        for(int xx=x0;xx<=x1;++xx){ framebuffer->set_pixel(xx,y0,r,g,b,a); framebuffer->set_pixel(xx,y1,r,g,b,a); }
        for(int yy=y0;yy<=y1;++yy){ framebuffer->set_pixel(x0,yy,r,g,b,a); framebuffer->set_pixel(x1,yy,r,g,b,a); }
    }
}
void Game2D::draw_circle(int cx,int cy,int radius,int r,int g,int b,int a,bool fill){
    if(!framebuffer) return;
    int rr=radius*radius;
    if(fill){ for(int y=cy-radius;y<=cy+radius;++y) for(int x=cx-radius;x<=cx+radius;++x){int dx=x-cx,dy=y-cy; if(dx*dx+dy*dy<=rr) framebuffer->set_pixel(x,y,r,g,b,a);} }
    else { for(int t=0;t<360;++t){ double th=t*3.1415926535/180.0; int x=(int)std::round(cx+std::cos(th)*radius), y=(int)std::round(cy+std::sin(th)*radius); framebuffer->set_pixel(x,y,r,g,b,a);} }
}
void Game2D::draw_line(int x1,int y1,int x2,int y2,int r,int g,int b,int a,int thickness){
    if(!framebuffer) return;
    int dx=std::abs(x2-x1), sx=x1<x2?1:-1;
    int dy=-std::abs(y2-y1), sy=y1<y2?1:-1;
    int err=dx+dy, x=x1, y=y1;
    while(true){
        for(int ty=-thickness/2;ty<=thickness/2;++ty) for(int tx=-thickness/2;tx<=thickness/2;++tx) framebuffer->set_pixel(x+tx,y+ty,r,g,b,a);
        if(x==x2&&y==y2) break;
        int e2=2*err;
        if(e2>=dy){err+=dy;x+=sx;} if(e2<=dx){err+=dx;y+=sy;}
    }
}
void Game2D::draw_text(const std::string& text,int x,int y,int r,int g,int b,int a,int size){
    if(!framebuffer) return;
    static const unsigned char font5x7[96][5]={
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
    const int cw=5,ch=7;
    double scale=std::max(1.0,(double)size/7.0);
    int cx=x;
    for(char ch0:text){
        unsigned char cc=(unsigned char)ch0; if(cc<32||cc>127) cc=32;
        const unsigned char* glyph=font5x7[cc-32];
        for(int row=0;row<ch;++row) for(int col=0;col<cw;++col)
            if(glyph[col]&(1<<row))
                for(int sy=0;sy<(int)scale;++sy) for(int sx=0;sx<(int)scale;++sx)
                    framebuffer->set_pixel(cx+col*(int)scale+sx, y+row*(int)scale+sy, r,g,b,a);
        cx += (int)((cw+1)*scale);
    }
}
void Game2D::draw_image(const Image* img,int x,int y,double angle_deg,double scale,double opacity){
    if(!img||!framebuffer) return;
    double rad=angle_deg*3.1415926535/180.0, cs=std::cos(rad), sn=std::sin(rad);
    int iw=img->width, ih=img->height, cx0=iw/2, cy0=ih/2;
    int bnd_w=(int)(std::max(std::abs(cs),std::abs(sn))*std::max(iw,ih)*scale)+4, bnd_h=bnd_w;
    for(int by=-bnd_h;by<bnd_h;++by) for(int bx=-bnd_w;bx<bnd_w;++bx){
        double sx0=(bx*cs+by*sn)/scale+cx0, sy0=(-bx*sn+by*cs)/scale+cy0;
        int isx=(int)std::round(sx0), isy=(int)std::round(sy0);
        if(isx>=0&&isx<iw&&isy>=0&&isy<ih){
            int rr,gg,bb,aa; img->get_pixel(isx,isy,rr,gg,bb,aa);
            int dstx=x+cx0*(int)scale+bx, dsty=y+cy0*(int)scale+by;
            int alpha=(int)(aa*opacity);
            if(alpha>0) framebuffer->set_pixel(dstx,dsty,rr,gg,bb,alpha);
        }
    }
}
void Game2D::draw_sprite(const Sprite* s){
    if(!s||!s->visible||!s->image) return;
    int dx=(int)(s->x - s->anchor_x*s->scale), dy=(int)(s->y - s->anchor_y*s->scale);
    if(s->angle==0&&s->scale==1.0&&s->opacity>=1.0){
        if(!framebuffer) return;
        for(int y0=0;y0<s->image->height;++y0) for(int x0=0;x0<s->image->width;++x0){int rr,gg,bb,aa; s->image->get_pixel(x0,y0,rr,gg,bb,aa); if(aa>0) framebuffer->set_pixel(dx+x0,dy+y0,rr,gg,bb,aa);}
    } else draw_image(s->image.get(),dx,dy,s->angle,s->scale,s->opacity);
}
void Game2D::draw_all_sprites(){ for(auto&s:sprites) if(s->visible) draw_sprite(s.get()); }

} // namespace vor

// ============================================================
// C 转发生成（实现在此，避免 header 膨胀）
// ============================================================
using namespace vor;

static inline Game2D& G(){ return Game2D::instance(); }

extern "C" {

void* vor_g2d_new_image(long long w,long long h){ return rt_store(std::make_shared<Image>((int)w,(int)h)); }
void* vor_g2d_checker_image(long long w,long long h,long long tile){ return rt_store(Image::make_checker((int)w,(int)h,(int)tile)); }
void* vor_g2d_solid_image(long long w,long long h,long long r,long long g,long long b){ return rt_store(Image::make_solid((int)w,(int)h,(int)r,(int)g,(int)b)); }
int vor_g2d_image_get_pixel(void* img,long long x,long long y,long long* out){
    auto im=rt_from<Image>(img);
    if(!im||!out) return 0;
    int r,g,b,a; im->get_pixel((int)x,(int)y,r,g,b,a);
    out[0]=r; out[1]=g; out[2]=b; out[3]=a;
    return im->px.empty()?0:1;
}
void* vor_g2d_image_size_pair(void* img){ auto im=rt_from<Image>(img); if(!im) return nullptr; return vor_pair_new(im->width,im->height); }
long long vor_g2d_image_w(void* img){ auto im=rt_from<Image>(img); return im?im->width:0; }
long long vor_g2d_image_h(void* img){ auto im=rt_from<Image>(img); return im?im->height:0; }

void* vor_g2d_new_sprite(void* img,double x,double y){
    auto sp=std::make_shared<Sprite>();
    sp->image=rt_from<Image>(img);
    sp->x=x; sp->y=y;
    if(sp->image){ sp->anchor_x=sp->image->width/2; sp->anchor_y=sp->image->height/2; }
    G().sprites.push_back(sp);
    return rt_store(sp);
}
void vor_g2d_sprite_setpos(void* sp,double x,double y){ auto s=rt_from<Sprite>(sp); if(s){s->x=x;s->y=y;} }
double vor_g2d_sprite_posx(void* sp){ auto s=rt_from<Sprite>(sp); return s?s->x:0; }
double vor_g2d_sprite_posy(void* sp){ auto s=rt_from<Sprite>(sp); return s?s->y:0; }
void vor_g2d_sprite_move(void* sp,double dx,double dy){ auto s=rt_from<Sprite>(sp); if(s){s->x+=dx;s->y+=dy;} }
void vor_g2d_sprite_setangle(void* sp,double a){ auto s=rt_from<Sprite>(sp); if(s)s->angle=a; }
double vor_g2d_sprite_angle(void* sp){ auto s=rt_from<Sprite>(sp); return s?s->angle:0; }
void vor_g2d_sprite_setscale(void* sp,double s){ auto s0=rt_from<Sprite>(sp); if(s0)s0->scale=s; }
double vor_g2d_sprite_scale(void* sp){ auto s=rt_from<Sprite>(sp); return s?s->scale:0; }
void vor_g2d_sprite_setvisible(void* sp,int v){ auto s=rt_from<Sprite>(sp); if(s)s->visible=v!=0; }
int vor_g2d_sprite_visible(void* sp){ auto s=rt_from<Sprite>(sp); return s&&s->visible?1:0; }
double vor_g2d_sprite_width(void* sp){ auto s=rt_from<Sprite>(sp); return s?s->width():0; }
double vor_g2d_sprite_height(void* sp){ auto s=rt_from<Sprite>(sp); return s?s->height():0; }
void vor_g2d_sprite_set_attr(void* sp,long long ax,long long ay,double sc){ auto s=rt_from<Sprite>(sp); if(s){s->anchor_x=(int)ax;s->anchor_y=(int)ay;s->scale=sc;} }
int vor_g2d_sprite_collides(void* a,void* b){
    auto sa=rt_from<Sprite>(a), sb=rt_from<Sprite>(b);
    if(!sa||!sb||!sa->visible||!sb->visible) return 0;
    auto ov=[&](double a0,double a1,double b0,double b1){ return a0<b1&&b0<a1; };
    return (ov(sa->left(),sa->right(),sb->left(),sb->right())&&ov(sa->top(),sa->bottom(),sb->top(),sb->bottom()))?1:0;
}
int vor_g2d_sprite_contains(void* sp,double px,double py){ auto s=rt_from<Sprite>(sp); if(!s||!s->visible) return 0; return (px>=s->left()&&px<s->right()&&py>=s->top()&&py<s->bottom())?1:0; }

void vor_g2d_set_window(long long w,long long h,const char* t){ G().ensure_window((int)w,(int)h,t?t:""); }
void vor_g2d_set_title(const char* t){ G().title=t?t:""; }
long long vor_g2d_width(void){ return G().width; }
long long vor_g2d_height(void){ return G().height; }
double vor_g2d_time(void){ return G().time; }
double vor_g2d_dt(void){ return G().dt; }
void vor_g2d_set_fps(long long fps){ G().fps=(int)fps; }
void vor_g2d_quit(void){ G().close_window(); }
int vor_g2d_running(void){ return G().running?1:0; }
void vor_g2d_clear(long long r,long long g,long long b){ G().clear((int)r,(int)g,(int)b); }
void vor_g2d_draw_rect(long long x,long long y,long long w,long long h,long long r,long long g,long long b,long long a,int fill){ G().draw_rect((int)x,(int)y,(int)w,(int)h,(int)r,(int)g,(int)b,(int)a,fill!=0); }
void vor_g2d_draw_circle(long long cx,long long cy,long long rad,long long r,long long g,long long b,long long a,int fill){ G().draw_circle((int)cx,(int)cy,(int)rad,(int)r,(int)g,(int)b,(int)a,fill!=0); }
void vor_g2d_draw_line(long long x1,long long y1,long long x2,long long y2,long long r,long long g,long long b,long long a,long long th){ G().draw_line((int)x1,(int)y1,(int)x2,(int)y2,(int)r,(int)g,(int)b,(int)a,(int)th); }
void vor_g2d_draw_text(const char* t,long long x,long long y,long long r,long long g,long long b,long long a,long long size){ G().draw_text(t?t:"",(int)x,(int)y,(int)r,(int)g,(int)b,(int)a,(int)size); }
void vor_g2d_draw_image(void* img,long long x,long long y,double angle,double scale,double opacity){ G().draw_image(rt_from<Image>(img).get(),(int)x,(int)y,angle,scale,opacity); }
void vor_g2d_draw_sprite(void* sp){ G().draw_sprite(rt_from<Sprite>(sp).get()); }
void vor_g2d_draw_all_sprites(void){ G().draw_all_sprites(); }
int vor_g2d_key_down(const char* k){ return G().key_down(k?k:"")?1:0; }
void vor_g2d_set_key(const char* k,int on){ if(k){ if(on) G().key_names.insert(k); else G().key_names.erase(k);} }
void vor_g2d_set_mouse(double x,double y,int l,int m,int r){ (void)x;(void)y;(void)l;(void)m;(void)r; }

} // extern "C"