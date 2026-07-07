// ハムスターFPSアリーナ — C++ネイティブ版(macOS / Linux)
// Web版(index.html)の射撃訓練場をネイティブ移植したもの。
// 依存: GLFW3 + OpenGL 2.1(全Macで動作する互換プロファイル)
//
// 操作: WASD=移動 Shift=ダッシュ C=匍匐 マウス=視点 左クリック=射撃
//       右クリック=覗く(ADS) R=リロード 1/2/3=武器 ESC=マウス解放

#include <GLFW/glfw3.h>
#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

// ---------------- 定数 ----------------
static const float PLAY_R  = 110.0f;
static const float WATER_Y = -2.3f;
static const float EYE     = 1.35f;
static const float PI      = 3.14159265358979f;

static float clampf(float v, float a, float b){ return v<a?a:(v>b?b:v); }

// ---------------- 乱数(Web版と同じmulberry32) ----------------
struct Rng {
  uint32_t s;
  explicit Rng(uint32_t seed):s(seed){}
  float next(){
    s += 0x6D2B79F5u;
    uint32_t t = s;
    t = (t ^ (t >> 15)) * (t | 1u);
    t ^= t + (t ^ (t >> 7)) * (t | 61u);
    return float((t ^ (t >> 14)) ) / 4294967296.0f;
  }
};

// ---------------- 地形ノイズ ----------------
struct Noise {
  float g[64*64];
  void init(Rng& r){ for(int i=0;i<64*64;i++) g[i]=r.next(); }
  float at(int x,int y) const { return g[((y%64)+64)%64*64 + ((x%64)+64)%64]; }
  float sample(float x,float y) const {
    int xi=(int)floorf(x), yi=(int)floorf(y);
    float u=x-xi, v=y-yi;
    u=u*u*(3-2*u); v=v*v*(3-2*v);
    float a=at(xi,yi), b=at(xi+1,yi), c=at(xi,yi+1), d=at(xi+1,yi+1);
    return a+(b-a)*u+(c-a)*v+(a-b-c+d)*u*v;
  }
};

static Noise gNoise, gNoise2;
struct Peak { float x,z,h,s; };
static std::vector<Peak> gPeaks;

static float groundY(float x, float z){
  float h = gNoise.sample(x*0.028f, z*0.028f)*6.6f
          + gNoise.sample(x*0.07f+31, z*0.07f+17)*2.4f
          + gNoise.sample(x*0.18f+7,  z*0.18f+51)*0.8f - 5.0f;
  for(const Peak& p : gPeaks){
    float dx=x-p.x, dz=z-p.z;
    h += p.h * expf(-(dx*dx+dz*dz)/(2*p.s*p.s));
  }
  float r = sqrtf(x*x+z*z);
  if(r > PLAY_R-3){ float e=r-(PLAY_R-3); h += e*e*0.22f; }
  if(h > 30) h = 30 + (h-30)*0.05f;
  return h;
}

// ---------------- 障害物(円柱判定) ----------------
struct Obstacle { float x,z,rr,top; };
static std::vector<Obstacle> gObstacles;

// ---------------- 武器 ----------------
struct Weapon {
  const char* name; float dmg, cd, spd, spread, recoil, rel;
  int magSize, reserveMax;
  int mag, reserve;
};
static Weapon WPN[3] = {
  {"G18",   20, 0.42f,  70, 0.008f, 0.025f, 1.1f, 12, 36, 12, 36},
  {"M416",  13, 0.14f,  75, 0.018f, 0.013f, 1.6f, 30, 60, 30, 60},
  {"Kar98", 42, 0.90f, 120, 0.003f, 0.060f, 2.0f,  5, 15,  5, 15},
};

// ---------------- ゲーム状態 ----------------
struct Player {
  float x=0, z=10, yaw=PI, pitch=0, eyeY=0, feetY=0;
  bool prone=false, zoom=false;
  int sel=0;
  float cd=0; int reloading=-1;
  float bobPhase=0, walkAmp=0, adsT=0, gunKick=0;
} me;

struct Projectile { float px,py,pz, vx,vy,vz, life; float dmg; };
static std::vector<Projectile> gProj;

struct Balloon { float x,y,z,phase; int id; };
static std::vector<Balloon> gBalloons;
static int gBalloonSeq=0; static float gBalloonTimer=0.5f;
static int gScore=0;

struct Dummy { float x,z,hp,down,dying; };
static std::vector<Dummy> gDummies;

struct Puff { float x,y,z,size,life; float r,g,b; };
static std::vector<Puff> gPuffs;

static double gMouseX=0, gMouseY=0; static bool gMouseCaptured=true;
static bool gFiring=false;
static float gFov=72;
static Rng gFx(12345); // 演出用(スプレッドなど)

// ---------------- 入力 ----------------
static GLFWwindow* gWin=nullptr;

static void startReload(){
  Weapon& w=WPN[me.sel];
  if(me.reloading>=0 || w.mag>=w.magSize || w.reserve<=0) return;
  me.reloading=me.sel;
  me.cd=std::max(me.cd, w.rel);
}

static void keyCB(GLFWwindow*, int key, int, int action, int){
  if(action!=GLFW_PRESS) return;
  if(key==GLFW_KEY_ESCAPE){
    gMouseCaptured=!gMouseCaptured;
    glfwSetInputMode(gWin, GLFW_CURSOR, gMouseCaptured?GLFW_CURSOR_DISABLED:GLFW_CURSOR_NORMAL);
  }
  if(key==GLFW_KEY_C) me.prone=!me.prone;
  if(key==GLFW_KEY_R) startReload();
  if(key>=GLFW_KEY_1 && key<=GLFW_KEY_3){
    me.sel=key-GLFW_KEY_1; me.reloading=-1; me.cd=std::min(me.cd,0.25f); me.zoom=false;
  }
}
static void mouseBtnCB(GLFWwindow*, int btn, int action, int){
  if(!gMouseCaptured) return;
  if(btn==GLFW_MOUSE_BUTTON_LEFT)  gFiring = (action==GLFW_PRESS);
  if(btn==GLFW_MOUSE_BUTTON_RIGHT && action==GLFW_PRESS) me.zoom=!me.zoom;
}

// ---------------- 射撃 ----------------
static void puffAt(float x,float y,float z,float size,float r,float g,float b){
  gPuffs.push_back({x,y,z,size,0.3f,r,g,b});
}

static void fire(){
  Weapon& w=WPN[me.sel];
  if(me.cd>0) return;
  if(w.mag<=0){
    if(w.reserve>0){ startReload(); }
    else me.cd=0.3f;
    return;
  }
  me.cd=w.cd; w.mag--;
  if(w.mag==0 && w.reserve>0) startReload();
  me.gunKick=1;
  // 反動
  float rec=w.recoil*(me.prone?0.55f:1.0f)*(me.zoom?0.85f:1.0f);
  me.pitch=clampf(me.pitch+rec,-1.15f,1.15f);
  me.yaw += (gFx.next()-0.5f)*rec*0.6f;
  // 弾道
  float spr=w.spread*(me.zoom?0.5f:1.0f);
  float cy=cosf(me.pitch), dx=-sinf(me.yaw)*cy, dy=sinf(me.pitch), dz=-cosf(me.yaw)*cy;
  dx+=(gFx.next()-0.5f)*spr*2; dy+=(gFx.next()-0.5f)*spr*2; dz+=(gFx.next()-0.5f)*spr*2;
  float L=sqrtf(dx*dx+dy*dy+dz*dz); dx/=L; dy/=L; dz/=L;
  Projectile p;
  p.px=me.x+dx*0.7f; p.py=me.eyeY-0.12f+dy*0.7f; p.pz=me.z+dz*0.7f;
  p.vx=dx*w.spd; p.vy=dy*w.spd; p.vz=dz*w.spd;
  p.life=2.2f; p.dmg=w.dmg;
  gProj.push_back(p);
}

static float segPointDist(float ax,float ay,float az,float bx,float by,float bz,float px,float py,float pz){
  float dx=bx-ax, dy=by-ay, dz=bz-az;
  float L2=dx*dx+dy*dy+dz*dz;
  float t = L2>0 ? ((px-ax)*dx+(py-ay)*dy+(pz-az)*dz)/L2 : 0;
  t=clampf(t,0,1);
  float cx=ax+dx*t-px, cy2=ay+dy*t-py, cz=az+dz*t-pz;
  return sqrtf(cx*cx+cy2*cy2+cz*cz);
}

static void updateProjectiles(float dt){
  for(size_t i=gProj.size(); i-->0;){
    Projectile& p=gProj[i];
    float ax=p.px, ay=p.py, az=p.pz;
    p.vy-=11*dt;
    p.px+=p.vx*dt; p.py+=p.vy*dt; p.pz+=p.vz*dt;
    p.life-=dt;
    bool dead=false;
    // 風船
    for(size_t b=0;b<gBalloons.size()&&!dead;b++){
      Balloon& bl=gBalloons[b];
      if(segPointDist(ax,ay,az,p.px,p.py,p.pz, bl.x,bl.y,bl.z)<0.58f){
        puffAt(bl.x,bl.y,bl.z,0.6f, 1,0.4f,0.4f);
        gBalloons.erase(gBalloons.begin()+b);
        gScore++;
        dead=true;
      }
    }
    // 的
    for(Dummy& d : gDummies){
      if(dead||d.hp<=0) continue;
      float cy=groundY(d.x,d.z)+0.8f;
      if(segPointDist(ax,ay,az,p.px,p.py,p.pz, d.x,cy,d.z)<0.78f){
        d.hp-=p.dmg;
        puffAt(p.px,p.py,p.pz,0.35f, 1,0.82f,0.82f);
        if(d.hp<=0){ d.down=2.5f; d.dying=0.55f; }
        dead=true;
      }
    }
    // 地面・障害物(3分割サンプル)
    if(!dead){
      for(int k=1;k<=3&&!dead;k++){
        float t=k/3.0f;
        float sx=ax+(p.px-ax)*t, sy=ay+(p.py-ay)*t, sz=az+(p.pz-az)*t;
        float gh=groundY(sx,sz);
        if(gh<WATER_Y && sy<WATER_Y){ puffAt(sx,WATER_Y+0.05f,sz,0.35f, 0.66f,0.83f,0.92f); dead=true; break; }
        if(sy<gh){ puffAt(sx,sy,sz,0.3f, 0.54f,0.5f,0.42f); dead=true; break; }
        for(const Obstacle& o : gObstacles){
          float dx2=sx-o.x, dz2=sz-o.z;
          if(dx2*dx2+dz2*dz2<o.rr*o.rr && sy<o.top){ puffAt(sx,sy,sz,0.25f, 0.6f,0.58f,0.53f); dead=true; break; }
        }
      }
    }
    if(p.life<=0) dead=true;
    if(dead) gProj.erase(gProj.begin()+i);
  }
  for(size_t i=gPuffs.size(); i-->0;){
    gPuffs[i].life-=dt;
    gPuffs[i].size*=(1+dt*7);
    if(gPuffs[i].life<=0) gPuffs.erase(gPuffs.begin()+i);
  }
}

// ---------------- 風船・的 ----------------
static void spawnBalloon(Rng& r){
  float ang=r.next()*6.28f, rad=4+r.next()*95;
  float x=cosf(ang)*rad, z=sinf(ang)*rad;
  float h=groundY(x,z);
  float y=std::max(h,WATER_Y)+1.4f+r.next()*2.6f;
  gBalloons.push_back({x,y,z, r.next()*6.28f, gBalloonSeq++});
}
static Rng gBalloonRng(99999);

// ---------------- 描画ヘルパー(OpenGL 2.1 即時モード) ----------------
static void drawSphere(float r,int seg=10,int rings=8){
  for(int i=0;i<rings;i++){
    float t0=PI*(-0.5f+(float)i/rings),  t1=PI*(-0.5f+(float)(i+1)/rings);
    glBegin(GL_QUAD_STRIP);
    for(int j=0;j<=seg;j++){
      float a=2*PI*j/seg;
      for(float t : {t0,t1}){
        float nx=cosf(t)*cosf(a), ny=sinf(t), nz=cosf(t)*sinf(a);
        glNormal3f(nx,ny,nz);
        glVertex3f(nx*r,ny*r,nz*r);
      }
    }
    glEnd();
  }
}
static void drawCylinder(float r0,float r1,float h,int seg=8){
  glBegin(GL_QUAD_STRIP);
  for(int j=0;j<=seg;j++){
    float a=2*PI*j/seg, ca=cosf(a), sa=sinf(a);
    glNormal3f(ca,0,sa);
    glVertex3f(ca*r1,0,sa*r1);
    glVertex3f(ca*r0,h,sa*r0);
  }
  glEnd();
}
static void drawCone(float r,float h,int seg=9){
  glBegin(GL_TRIANGLES);
  for(int j=0;j<seg;j++){
    float a0=2*PI*j/seg, a1=2*PI*(j+1)/seg;
    float nx=cosf((a0+a1)/2), nz=sinf((a0+a1)/2);
    glNormal3f(nx,0.5f,nz);
    glVertex3f(0,h,0);
    glVertex3f(cosf(a0)*r,0,sinf(a0)*r);
    glVertex3f(cosf(a1)*r,0,sinf(a1)*r);
  }
  glEnd();
}

// ---------------- .hamモデル(GLB変換済み)読み込み ----------------
#ifndef GL_GENERATE_MIPMAP
#define GL_GENERATE_MIPMAP 0x8191
#endif
struct Model { GLuint list=0, tex=0; bool ok=false; };
static Model loadHam(const char* name){
  Model m;
  const char* prefixes[]={"assets_native/","../assets_native/","native/assets_native/"};
  FILE* f=nullptr; char path[512];
  for(const char* p : prefixes){
    snprintf(path,sizeof(path),"%s%s",p,name);
    f=fopen(path,"rb");
    if(f) break;
  }
  if(!f){ fprintf(stderr,"model missing: %s (手作りモデルで続行)\n", name); return m; }
  char magic[4]; uint32_t vc=0,ic=0,tw=0,th=0;
  if(fread(magic,1,4,f)!=4 || memcmp(magic,"HAM1",4)!=0){ fclose(f); return m; }
  fread(&vc,4,1,f); fread(&ic,4,1,f); fread(&tw,4,1,f); fread(&th,4,1,f);
  std::vector<float> vtx((size_t)vc*8);
  std::vector<uint32_t> idx(ic);
  fread(vtx.data(),4,(size_t)vc*8,f);
  fread(idx.data(),4,ic,f);
  std::vector<uint8_t> tex;
  if(tw&&th){ tex.resize((size_t)tw*th*3); fread(tex.data(),1,tex.size(),f); }
  fclose(f);
  if(tw&&th){
    glGenTextures(1,&m.tex);
    glBindTexture(GL_TEXTURE_2D,m.tex);
    glTexParameteri(GL_TEXTURE_2D,GL_GENERATE_MIPMAP,GL_TRUE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_REPEAT);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGB,tw,th,0,GL_RGB,GL_UNSIGNED_BYTE,tex.data());
  }
  m.list=glGenLists(1);
  glNewList(m.list, GL_COMPILE);
  glBegin(GL_TRIANGLES);
  for(uint32_t i : idx){
    const float* v=&vtx[(size_t)i*8];
    glNormal3fv(v+3);
    glTexCoord2fv(v+6);
    glVertex3fv(v);
  }
  glEnd();
  glEndList();
  m.ok=true;
  return m;
}
static void drawModel(const Model& m){
  glColor3f(1,1,1);
  glEnable(GL_TEXTURE_2D);
  glBindTexture(GL_TEXTURE_2D,m.tex);
  glCallList(m.list);
  glDisable(GL_TEXTURE_2D);
}
static Model gMHam,gMPine,gMLeafy,gMRockA,gMRockB,gMBush,gMLog,gMStump;
struct DecorInst { const Model* m; float x,y,z,s,rot; };
static std::vector<DecorInst> gDecorInst;

// ---------------- ワールド構築(表示リスト) ----------------
static GLuint gTerrainList=0, gDecorList=0, gHamList=0;

static void buildWorld(uint32_t seed){
  Rng rng(seed);
  gNoise.init(rng); gNoise2.init(rng);
  gPeaks.clear();
  for(int i=0;i<7;i++){
    float a=rng.next()*6.28f, r=20+rng.next()*70;
    gPeaks.push_back({cosf(a)*r, sinf(a)*r, 8+rng.next()*9, 10+rng.next()*10});
  }

  // 地形メッシュ
  const int SEG=170; const float SIZE=340;
  gTerrainList=glGenLists(1);
  glNewList(gTerrainList, GL_COMPILE);
  for(int i=0;i<SEG;i++){
    glBegin(GL_TRIANGLE_STRIP);
    for(int j=0;j<=SEG;j++){
      for(int di=1;di>=0;di--){
        float fx=-SIZE/2+SIZE*(i+di)/SEG;
        float fz=-SIZE/2+SIZE*j/SEG;
        float h=groundY(fx,fz);
        // Web版と同じ色ルール(簡略版)
        float cr,cg,cb;
        if(h<WATER_Y){ cr=0.42f;cg=0.48f;cb=0.42f; }
        else if(h<WATER_Y+0.9f){ float t=(h-WATER_Y)/0.9f; cr=0.42f+0.32f*t; cg=0.48f+0.19f*t; cb=0.42f+0.05f*t; }
        else if(h<WATER_Y+1.6f){ float t=(h-WATER_Y-0.9f)/0.7f; cr=0.74f-0.44f*t; cg=0.67f-0.20f*t; cb=0.47f-0.23f*t; }
        else { cr=0.30f;cg=0.47f;cb=0.24f; }
        if(h>9.5f){ float t=clampf((h-9.5f)/2.5f,0,1); cr+= (0.92f-cr)*t; cg+=(0.93f-cg)*t; cb+=(0.95f-cb)*t; }
        float wob=(gNoise2.sample(fx*0.6f,fz*0.6f)-0.5f)*0.06f;
        glColor3f(cr+wob,cg+wob,cb+wob);
        // 法線(近傍差分)
        float hx=groundY(fx+1,fz)-h, hz=groundY(fx,fz+1)-h;
        float nx=-hx, ny=1, nz=-hz;
        float L=sqrtf(nx*nx+ny*ny+nz*nz);
        glNormal3f(nx/L,ny/L,nz/L);
        glVertex3f(fx,h,fz);
      }
    }
    glEnd();
  }
  glEndList();

  // 装飾(木・岩・茂み・倒木・切り株)+障害物
  // GLB変換モデルがあればインスタンス、なければ手作りプリミティブを表示リストへ
  gObstacles.clear();
  gDecorInst.clear();
  gDecorList=glGenLists(1);
  glNewList(gDecorList, GL_COMPILE);
  for(int i=0;i<90;i++){ // 木
    float ang=rng.next()*6.28f, r=6+rng.next()*96;
    float x=cosf(ang)*r, z=sinf(ang)*r, h=groundY(x,z);
    float s=0.8f+rng.next()*0.7f;
    bool conifer = rng.next()<0.55f;
    float rot=rng.next()*6.28f;
    if(h<WATER_Y+1.2f || h>10) continue;
    const Model* mdl = conifer ? (gMPine.ok?&gMPine:nullptr) : (gMLeafy.ok?&gMLeafy:nullptr);
    if(mdl){
      gDecorInst.push_back({mdl,x,h,z, (conifer?3.9f:3.6f)*s, rot});
    } else {
      glPushMatrix();
      glTranslatef(x,h,z);
      glScalef(s,s,s);
      glColor3f(0.48f,0.36f,0.23f);
      drawCylinder(0.12f,0.22f,1.6f,7);
      if(conifer){
        for(int k=0;k<3;k++){
          glPushMatrix();
          glTranslatef(0,1.1f+k*0.75f,0);
          glColor3f(0.16f,0.42f+k*0.03f,0.16f);
          drawCone(1.55f-k*0.42f,1.55f,9);
          glPopMatrix();
        }
      } else {
        glPushMatrix();
        glTranslatef(0,2.2f,0);
        glColor3f(0.24f,0.5f,0.2f);
        drawSphere(1.1f,9,7);
        glPopMatrix();
      }
      glPopMatrix();
    }
    gObstacles.push_back({x,z,0.4f*s,h+1.6f*s});
  }
  for(int i=0;i<26;i++){ // 岩
    float ang=rng.next()*6.28f, r=7+rng.next()*96;
    float x=cosf(ang)*r, z=sinf(ang)*r, h=groundY(x,z);
    if(h<WATER_Y+0.3f) continue;
    float rad=1.0f+rng.next()*1.8f;
    bool typeA = rng.next()<0.5f;
    float rot=rng.next()*6.28f;
    const Model* mdl = typeA ? (gMRockA.ok?&gMRockA:(gMRockB.ok?&gMRockB:nullptr))
                             : (gMRockB.ok?&gMRockB:(gMRockA.ok?&gMRockA:nullptr));
    if(mdl){
      gDecorInst.push_back({mdl,x,h-0.05f,z, (mdl==&gMRockA?0.9f:1.6f)*rad, rot});
    } else {
      glPushMatrix();
      glTranslatef(x,h+rad*0.25f,z);
      glScalef(1,0.75f,1);
      glColor3f(0.45f,0.44f,0.42f);
      drawSphere(rad,8,6);
      glPopMatrix();
    }
    gObstacles.push_back({x,z,rad*0.9f,h+rad*1.1f});
  }
  for(int i=0;i<34;i++){ // 茂み(装飾のみ)
    float ang=rng.next()*6.28f, r=5+rng.next()*96;
    float x=cosf(ang)*r, z=sinf(ang)*r, h=groundY(x,z);
    float s=0.4f+rng.next()*0.5f;
    float rot=rng.next()*6.28f;
    if(h<WATER_Y+0.5f) continue;
    if(gMBush.ok){
      gDecorInst.push_back({&gMBush,x,h-0.03f,z, 1.4f*s, rot});
    } else {
      glPushMatrix();
      glTranslatef(x,h+s*0.5f,z);
      glScalef(1,0.7f,1);
      glColor3f(0.2f,0.42f,0.17f);
      drawSphere(s,8,6);
      glPopMatrix();
    }
  }
  for(int i=0;i<8;i++){ // 倒木
    float ang=rng.next()*6.28f, r=6+rng.next()*88;
    float x=cosf(ang)*r, z=sinf(ang)*r, h=groundY(x,z);
    float len=1.8f+rng.next()*1.6f;
    float rot=rng.next()*6.28f;
    if(h<WATER_Y+0.8f || h>5) continue;
    if(gMLog.ok) gDecorInst.push_back({&gMLog,x,h-0.02f,z, len/2.3f, rot});
    gObstacles.push_back({x,z,0.55f,h+0.5f});
  }
  for(int i=0;i<6;i++){ // 切り株
    float ang=rng.next()*6.28f, r=6+rng.next()*88;
    float x=cosf(ang)*r, z=sinf(ang)*r, h=groundY(x,z);
    if(h<WATER_Y+0.8f || h>5) continue;
    if(gMStump.ok) gDecorInst.push_back({&gMStump,x,h-0.03f,z, 0.62f, ang*3});
    else {
      glPushMatrix();
      glTranslatef(x,h,z);
      glColor3f(0.4f,0.29f,0.18f);
      drawCylinder(0.24f,0.3f,0.45f,8);
      glPopMatrix();
    }
    gObstacles.push_back({x,z,0.38f,h+0.55f});
  }
  glEndList();

  // 的(簡易ハムスター)の表示リスト
  gHamList=glGenLists(1);
  glNewList(gHamList, GL_COMPILE);
  glColor3f(0.85f,0.64f,0.37f);
  glPushMatrix(); glTranslatef(0,0.6f,0); glScalef(0.8f,0.95f,1.14f); drawSphere(0.52f,12,9); glPopMatrix();
  glPushMatrix(); glTranslatef(0,0.98f,0.34f); glScalef(0.86f,0.92f,1.0f); drawSphere(0.34f,10,8); glPopMatrix();
  glColor3f(0.98f,0.95f,0.9f);
  glPushMatrix(); glTranslatef(0,0.46f,0.26f); glScalef(0.66f,0.72f,0.6f); drawSphere(0.4f,10,8); glPopMatrix();
  glColor3f(0.11f,0.10f,0.09f);
  glPushMatrix(); glTranslatef( 0.13f,1.03f,0.58f); drawSphere(0.065f,7,6); glPopMatrix();
  glPushMatrix(); glTranslatef(-0.13f,1.03f,0.58f); drawSphere(0.065f,7,6); glPopMatrix();
  glColor3f(0.69f,0.50f,0.24f);
  glPushMatrix(); glTranslatef( 0.17f,1.3f,0.22f); glScalef(1,1.15f,0.5f); drawSphere(0.13f,8,6); glPopMatrix();
  glPushMatrix(); glTranslatef(-0.17f,1.3f,0.22f); glScalef(1,1.15f,0.5f); drawSphere(0.13f,8,6); glPopMatrix();
  glEndList();

  // 的の配置
  gDummies.clear();
  const float offs[5][2]={{0,-10},{-6,-9},{6,-9},{-3,-14},{3,-14}};
  for(auto& o : offs) gDummies.push_back({me.x+o[0], me.z+o[1], 100, 0, 0});
}

// ---------------- メイン ----------------
int main(){
  if(!glfwInit()){ fprintf(stderr,"glfw init failed\n"); return 1; }
  glfwWindowHint(GLFW_SAMPLES, 4);
  gWin=glfwCreateWindow(1280,720,"ハムスターFPS ネイティブ版", nullptr,nullptr);
  if(!gWin){ glfwTerminate(); return 1; }
  glfwMakeContextCurrent(gWin);
  glfwSwapInterval(1);
  glfwSetKeyCallback(gWin,keyCB);
  glfwSetMouseButtonCallback(gWin,mouseBtnCB);
  glfwSetInputMode(gWin, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
  glfwGetCursorPos(gWin,&gMouseX,&gMouseY);

  glEnable(GL_DEPTH_TEST);
  glEnable(GL_CULL_FACE); glCullFace(GL_BACK);
  glEnable(GL_LIGHTING); glEnable(GL_LIGHT0); glEnable(GL_COLOR_MATERIAL);
  glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
  float lightDir[4]={0.45f,0.8f,0.3f,0};
  float lightCol[4]={1.0f,0.96f,0.85f,1};
  float ambCol[4]={0.55f,0.6f,0.62f,1};
  glLightfv(GL_LIGHT0,GL_POSITION,lightDir);
  glLightfv(GL_LIGHT0,GL_DIFFUSE,lightCol);
  glLightfv(GL_LIGHT0,GL_AMBIENT,ambCol);
  glEnable(GL_FOG);
  float fogCol[4]={0.82f,0.89f,0.93f,1};
  glFogfv(GL_FOG_COLOR,fogCol);
  glFogi(GL_FOG_MODE,GL_LINEAR);
  glFogf(GL_FOG_START,60); glFogf(GL_FOG_END,265);

  // GLB変換モデル(assets_native/*.ham)を読み込み — 無ければ手作りモデルで続行
  gMHam  =loadHam("hamster.ham");
  gMPine =loadHam("pine.ham");
  gMLeafy=loadHam("leafy.ham");
  gMRockA=loadHam("rock_a.ham");
  gMRockB=loadHam("rock_b.ham");
  gMBush =loadHam("bush.ham");
  gMLog  =loadHam("log.ham");
  gMStump=loadHam("stump.ham");

  buildWorld(1234567u);
  me.feetY=groundY(me.x,me.z);
  me.eyeY=me.feetY+EYE;

  double lastT=glfwGetTime(), titleT=0;
  int frames=0; double fpsT=lastT; float fps=0;

  while(!glfwWindowShouldClose(gWin)){
    double now=glfwGetTime();
    float dt=(float)std::min(0.05,(now-lastT)); lastT=now;
    frames++;
    if(now-fpsT>=0.5){ fps=(float)(frames/(now-fpsT)); frames=0; fpsT=now; }

    glfwPollEvents();

    // 視点
    if(gMouseCaptured){
      double mx,my; glfwGetCursorPos(gWin,&mx,&my);
      float sens=0.0022f*(gFov/72.0f)*(me.zoom?0.5f:1.0f);
      me.yaw   -= (float)(mx-gMouseX)*sens;
      me.pitch  = clampf(me.pitch-(float)(my-gMouseY)*sens,-1.15f,1.15f);
      gMouseX=mx; gMouseY=my;
    }

    // 移動
    float f=0,s=0;
    if(glfwGetKey(gWin,GLFW_KEY_W)==GLFW_PRESS) f+=1;
    if(glfwGetKey(gWin,GLFW_KEY_S)==GLFW_PRESS) f-=1;
    if(glfwGetKey(gWin,GLFW_KEY_A)==GLFW_PRESS) s-=1;
    if(glfwGetKey(gWin,GLFW_KEY_D)==GLFW_PRESS) s+=1;
    bool dash = glfwGetKey(gWin,GLFW_KEY_LEFT_SHIFT)==GLFW_PRESS;
    bool wading = me.feetY < WATER_Y+0.4f;
    float adsMul = me.zoom ? (me.sel==2?0.3f : me.sel==1?0.55f : 0.7f) : 1.0f;
    float SP=(wading?3.8f:(dash?7.8f:5.0f))*(me.prone?0.35f:1.0f)*adsMul;
    float nx=me.x+(-sinf(me.yaw)*f+cosf(me.yaw)*s)*SP*dt;
    float nz=me.z+(-cosf(me.yaw)*f-sinf(me.yaw)*s)*SP*dt;
    float rr=sqrtf(nx*nx+nz*nz);
    if(rr>PLAY_R){ nx*=PLAY_R/rr; nz*=PLAY_R/rr; }
    if(groundY(nx,nz)<-3.1f){
      if(groundY(nx,me.z)>=-3.1f) nz=me.z;
      else if(groundY(me.x,nz)>=-3.1f) nx=me.x;
      else { nx=me.x; nz=me.z; }
    }
    for(const Obstacle& o : gObstacles){
      float dx=nx-o.x, dz=nz-o.z, d=sqrtf(dx*dx+dz*dz);
      if(d<o.rr+0.45f && d>0.001f){ float k=(o.rr+0.45f)/d; nx=o.x+dx*k; nz=o.z+dz*k; }
    }
    float moved=sqrtf((nx-me.x)*(nx-me.x)+(nz-me.z)*(nz-me.z));
    me.x=nx; me.z=nz;
    me.bobPhase+=std::min(moved/std::max(dt,0.001f),7.0f)*dt*1.7f;
    me.walkAmp+=((moved>0.008f?1.0f:0.0f)-me.walkAmp)*std::min(1.0f,dt*8);
    me.feetY=groundY(me.x,me.z);
    float targetY=std::max(me.feetY, me.feetY<WATER_Y?WATER_Y-0.5f:me.feetY)+(me.prone?0.5f:EYE);
    me.eyeY+=(targetY-me.eyeY)*std::min(1.0f,dt*14);

    // 武器
    me.cd=std::max(0.0f,me.cd-dt);
    me.gunKick=std::max(0.0f,me.gunKick-dt*7);
    if(me.reloading>=0 && me.cd<=0){
      Weapon& w=WPN[me.reloading];
      int take=std::min(w.magSize-w.mag, w.reserve);
      w.mag+=take; w.reserve-=take;
      me.reloading=-1;
    }
    if(gFiring) fire();
    me.adsT+=((me.zoom?1.0f:0.0f)-me.adsT)*std::min(1.0f,dt*10);
    float fovT = me.zoom?48.0f:72.0f;
    gFov+=(fovT-gFov)*std::min(1.0f,dt*12);

    updateProjectiles(dt);

    // 風船
    gBalloonTimer-=dt;
    if((int)gBalloons.size()<6 && gBalloonTimer<=0){
      spawnBalloon(gBalloonRng);
      gBalloonTimer=(gBalloons.size()<4)?0.4f+gBalloonRng.next()*0.5f:1.2f+gBalloonRng.next()*2.2f;
    }
    for(Balloon& b : gBalloons) b.phase+=dt;

    // 的
    for(Dummy& d : gDummies){
      if(d.dying>0){ d.dying-=dt; }
      if(d.hp<=0){
        d.down-=dt;
        if(d.down<=0){ d.hp=100; d.dying=0; }
      }
    }

    // ---------------- 描画 ----------------
    int fbw,fbh; glfwGetFramebufferSize(gWin,&fbw,&fbh);
    glViewport(0,0,fbw,fbh);
    glClearColor(0.55f,0.74f,0.91f,1);
    glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    {
      float aspect=(float)fbw/fbh;
      float nearP=0.1f, farP=700.0f;
      float t=nearP*tanf(gFov*PI/360.0f);
      glFrustum(-t*aspect,t*aspect,-t,t,nearP,farP);
    }
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    float bobY=fabsf(sinf(me.bobPhase))*0.05f*me.walkAmp*(me.prone?0.35f:1.0f);
    glRotatef(-me.pitch*180/PI,1,0,0);
    glRotatef(-me.yaw*180/PI+180,0,1,0);
    glTranslatef(-me.x,-(me.eyeY+bobY),-me.z);
    glLightfv(GL_LIGHT0,GL_POSITION,lightDir);

    glCallList(gTerrainList);
    glCallList(gDecorList);
    // GLB変換モデルのインスタンス
    for(const DecorInst& di : gDecorInst){
      glPushMatrix();
      glTranslatef(di.x,di.y,di.z);
      glRotatef(di.rot*180/PI,0,1,0);
      glScalef(di.s,di.s,di.s);
      drawModel(*di.m);
      glPopMatrix();
    }

    // 的
    for(const Dummy& d : gDummies){
      if(d.hp<=0 && d.dying<=0) continue;
      glPushMatrix();
      float gy=groundY(d.x,d.z);
      glTranslatef(d.x,gy,d.z);
      float ang=atan2f(me.x-d.x, me.z-d.z);
      glRotatef(ang*180/PI,0,1,0);
      if(d.dying>0){
        float k=1-d.dying/0.55f;
        glRotatef(k*k*86,0,0,1); // コテンと倒れる
      }
      if(gMHam.ok){
        glScalef(1.19f,1.19f,1.19f);
        drawModel(gMHam);
      } else {
        glScalef(0.92f,0.92f,0.92f);
        glCallList(gHamList);
      }
      glPopMatrix();
    }

    // 風船
    for(const Balloon& b : gBalloons){
      glPushMatrix();
      glTranslatef(b.x+sinf(b.phase*0.7f)*0.1f, b.y+sinf(b.phase*1.3f)*0.15f, b.z);
      int ci=b.id%5;
      const float cols[5][3]={{0.91f,0.29f,0.29f},{1,0.82f,0.4f},{0.35f,0.82f,0.65f},{0.35f,0.65f,0.91f},{0.94f,0.59f,0.71f}};
      glColor3fv(cols[ci]);
      glPushMatrix(); glScalef(1,1.15f,1); drawSphere(0.45f,10,8); glPopMatrix();
      glDisable(GL_LIGHTING);
      glColor3f(0.86f,0.86f,0.86f);
      glBegin(GL_LINES); glVertex3f(0,-0.5f,0); glVertex3f(0,-1.6f,0); glEnd();
      glEnable(GL_LIGHTING);
      glPopMatrix();
    }

    // 弾トレーサー
    glDisable(GL_LIGHTING);
    glColor3f(1,0.89f,0.63f);
    glBegin(GL_LINES);
    for(const Projectile& p : gProj){
      glVertex3f(p.px,p.py,p.pz);
      glVertex3f(p.px-p.vx*0.02f, p.py-p.vy*0.02f, p.pz-p.vz*0.02f);
    }
    glEnd();

    // パフ
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    for(const Puff& pf : gPuffs){
      glPushMatrix();
      glTranslatef(pf.x,pf.y,pf.z);
      glColor4f(pf.r,pf.g,pf.b,pf.life*2.6f);
      drawSphere(pf.size,7,5);
      glPopMatrix();
    }
    // 水面
    glColor4f(0.27f,0.53f,0.72f,0.72f);
    glBegin(GL_QUADS);
    glNormal3f(0,1,0);
    glVertex3f(-170,WATER_Y, 170); glVertex3f(170,WATER_Y, 170);
    glVertex3f( 170,WATER_Y,-170); glVertex3f(-170,WATER_Y,-170);
    glEnd();
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glEnable(GL_LIGHTING);

    // ---- HUD(2D) ----
    glDisable(GL_LIGHTING); glDisable(GL_DEPTH_TEST); glDisable(GL_FOG);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glOrtho(0,fbw,fbh,0,-1,1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    // クロスヘア
    glColor3f(1,1,1);
    glBegin(GL_LINES);
    glVertex2f(fbw/2.0f-12,fbh/2.0f); glVertex2f(fbw/2.0f+12,fbh/2.0f);
    glVertex2f(fbw/2.0f,fbh/2.0f-12); glVertex2f(fbw/2.0f,fbh/2.0f+12);
    glEnd();
    // マガジン残量バー
    {
      Weapon& w=WPN[me.sel];
      float ratio=(float)w.mag/w.magSize;
      glColor4f(0,0,0,0.4f);
      glEnable(GL_BLEND);
      glBegin(GL_QUADS);
      glVertex2f(fbw/2.0f-80,fbh-46); glVertex2f(fbw/2.0f+80,fbh-46);
      glVertex2f(fbw/2.0f+80,fbh-30); glVertex2f(fbw/2.0f-80,fbh-30);
      glEnd();
      glColor3f(1,0.85f,0.54f);
      glBegin(GL_QUADS);
      glVertex2f(fbw/2.0f-78,fbh-44); glVertex2f(fbw/2.0f-78+156*ratio,fbh-44);
      glVertex2f(fbw/2.0f-78+156*ratio,fbh-32); glVertex2f(fbw/2.0f-78,fbh-32);
      glEnd();
      glDisable(GL_BLEND);
    }
    glEnable(GL_FOG); glEnable(GL_DEPTH_TEST); glEnable(GL_LIGHTING);

    // タイトルバーにステータス表示
    titleT+=dt;
    if(titleT>0.2){
      titleT=0;
      Weapon& w=WPN[me.sel];
      char buf[256];
      snprintf(buf,sizeof(buf),
        "ハムスターFPS ネイティブ版 | %s %d/%d%s | 風船 %d | %s%s | %.0f fps",
        w.name, w.mag, w.reserve,
        me.reloading>=0?" リロード中":"",
        gScore,
        me.zoom?"ADS ":"", me.prone?"匍匐 ":"",
        fps);
      glfwSetWindowTitle(gWin,buf);
    }

    glfwSwapBuffers(gWin);
  }
  glfwDestroyWindow(gWin);
  glfwTerminate();
  return 0;
}
