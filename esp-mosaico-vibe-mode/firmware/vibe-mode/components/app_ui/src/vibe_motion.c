// SPDX-License-Identifier: Apache-2.0
/* Adapted from the f0b53c0 UI handoff motion.js / vibe_motion.c. */
#include "vibe_motion.h"
#include "vibe_maker_story.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265358979323846f
typedef struct { int n; float *w, *a; } frame_t;
static float clamp(float v) { return fmaxf(0.0f, fminf(1.0f, v)); }
static float ease(float v) { v = clamp(v); return v * v * (3.0f - 2.0f * v); }
bool vibe_motion_inside(int n, int x, int y)
{
    return x >= 0 && y >= 0 && x < n && y < n &&
           (x < n - 1 - x ? x : n - 1 - x) + (y < n - 1 - y ? y : n - 1 - y) >= 2;
}
static void clear(frame_t f)
{
    memset(f.w, 0, (size_t)f.n * f.n * sizeof(float));
    memset(f.a, 0, (size_t)f.n * f.n * sizeof(float));
}
static void put(frame_t f, int x, int y, float value, bool accent, bool add)
{
    if (!vibe_motion_inside(f.n, x, y)) return;
    float *v = (accent ? f.a : f.w) + y * f.n + x;
    *v = clamp(add ? *v + value : fmaxf(*v, value));
}
/* Keep C float and JavaScript rounding ties identical at integer LED positions. */
static int snap(float v) { return (int)floorf(v + .5001f); }
static void wipe(frame_t a, frame_t b, float p)
{
    const int columns = (int)floorf(clamp(p) * a.n);
    for (int i = 0; i < a.n * a.n; ++i) if (i % a.n < columns) {
        a.w[i] = b.w[i];
        a.a[i] = b.a[i];
    }
}
static void assembly(frame_t f, float p)
{
    static const int m[4][4] = {{2,2,4,4},{10,2,8,4},{2,10,4,8},{10,10,8,8}};
    clear(f);
    for (int i = 0; i < 4; ++i) {
        const float q = ease((p - i * .08f) / .76f);
        const int x = snap(m[i][0] + (m[i][2] - m[i][0]) * q);
        const int y = snap(m[i][1] + (m[i][3] - m[i][1]) * q);
        for (int dy = 0; dy < 3; ++dy) for (int dx = 0; dx < 3; ++dx)
            if (dx != 1 || dy != 1) put(f, x + dx, y + dy, 1, i == 3, false);
    }
    static const int center[5][2] = {{7,6},{6,7},{7,7},{8,7},{7,8}};
    if (p >= .86f) for (int i = 0; i < 5; ++i) put(f, center[i][0], center[i][1], 1, true, false);
}
static void word(frame_t f, float p)
{
    /* M O S A I C O, five columns with one blank column between letters. */
    static const uint8_t letters[7][7] = {
        {17,27,21,21,17,17,17}, {14,17,17,17,17,17,14},
        {15,16,16,14,1,1,30}, {14,17,17,31,17,17,17},
        {31,4,4,4,4,4,31}, {15,16,16,16,16,16,15}, {14,17,17,17,17,17,14}
    };
    clear(f);
    const int offset = snap(15 - 56 * p);
    for (int k = 0; k < 7; ++k) for (int y = 0; y < 7; ++y) for (int x = 0; x < 5; ++x)
        if (letters[k][y] & (1 << (4 - x))) put(f, offset + k * 6 + x, 4 + y, 1, false, false);
    put(f, 7, 12, 1, true, false);
}
static void run(frame_t f, float p)
{
    clear(f);
    for (int y = 3; y <= 11; ++y) put(f, 3, y, 1, false, false);
    static const int outline[9][2] = {{4,3},{4,11},{5,4},{5,10},{6,5},{6,9},{7,6},{7,8},{8,7}};
    for (int i = 0; i < 9; ++i) put(f, outline[i][0], outline[i][1], 1, false, false);
    put(f, 5, 7, 1, true, false);
    put(f, snap(9 + clamp(p) * 4), 7, 1, true, false);
}
void vibe_motion_home(float *white, float *accent, uint32_t ms, bool reduced)
{
    frame_t f = {15, white, accent};
    float tw[225], ta[225];
    frame_t tmp = {15, tw, ta};
    const uint32_t t = ms % VIBE_MOTION_CYCLE_MS;
    if (reduced) { assembly(f, 1); return; }
    if (t < 1200) assembly(f, 0);
    else if (t < 2100) assembly(f, (t - 1200) / 900.0f);
    else if (t < 2700) assembly(f, 1);
    else if (t < 7500) {
        const float p = (t - 2700) / 4800.0f;
        word(f, p);
    } else if (t < 7900) {
        word(f, 1); run(tmp, 0); wipe(f, tmp, ease((t - 7500) / 400.0f));
    } else if (t < 9200) run(f, (t - 7900) / 1300.0f);
    else if (t < 9800) {
        const float p = (t - 9200) / 600.0f;
        if (p < .45f) { run(f, 1); assembly(tmp, 1); wipe(f, tmp, ease(p / .45f)); }
        else assembly(f, 1 - ease((p - .45f) / .55f));
    } else assembly(f, 0);
}
void vibe_motion_ripple(float *white, float *accent, const vibe_ripple_t *ripples,
                        unsigned count, const vibe_ripple_t *press)
{
    frame_t f = {15, white, accent};
    if (count > VIBE_RIPPLE_LIMIT) count = VIBE_RIPPLE_LIMIT;
    for (int y = 0; y < 15; ++y) for (int x = 0; x < 15; ++x) {
        const int i = y * 15 + x;
        if (white[i] + accent[i] > 0) continue;
        float light = 0;
        for (unsigned k = 0; k < count; ++k) {
            const float p = ripples[k].age_ms / 650.0f;
            if (p >= 1) continue;
            const float d = hypotf(x - ripples[k].x, y - ripples[k].y);
            if (fabsf(d - p * 15) <= .55f) light = 1;
        }
        if (press && hypotf(x - press->x, y - press->y) <= 1) light = 1;
        put(f, x, y, light, true, false);
    }
}
/* Simple authored poses: interpolation only connects reactions to the current story. */
static const float s_maker_rest[22] = {7,4,7,6,7,9,5,7,4,9,9,7,10,9,6,11,5,13,8,11,9,13};
typedef struct { float q[22]; int lx, ly; } maker_pose_t;
static float soft(float t) { t=fmaxf(0,fminf(1,t)); return t*t*(3-2*t); }
static uint32_t remaining(uint32_t v,uint32_t dt) { return v>dt?v-dt:0; }
static uint32_t capped(uint32_t v,uint32_t dt,uint32_t max) { return v+dt<max?v+dt:max; }
static void maker_pose(uint32_t ms,float *q)
{
    const unsigned kind=(ms/18000)%3,t=ms%18000,i=t/100,j=i<179?i+1:179;
    const float u=(t%100)/100.f;
    for(int k=0;k<22;k++) q[k]=(s_maker_poses[s_maker_pose_index[kind][i]][k]+(s_maker_poses[s_maker_pose_index[kind][j]][k]-s_maker_poses[s_maker_pose_index[kind][i]][k])*u)/4.f;
}
static maker_pose_t companion_pose(const vibe_companion_t *s,bool reduced)
{
    maker_pose_t p={0};float base[22];
    if(reduced)memcpy(base,s_maker_rest,sizeof(base));else maker_pose(s->idle,base);
    const bool live=s->down||s->look||s->pose;
    if(!s->response_valid||(!live&&s->return_age>=300)){memcpy(p.q,base,sizeof(base));return p;}
    const float shift=base[4]-7;
    for(int k=0;k<22;k++)p.q[k]=s_maker_rest[k]+(k%2?0:shift);
    if(s->intent==1){
        const float age=reduced?650:s->response_age,amount=soft((age-80)/300)*(1-soft((age-1100)/350));
        const float wave[4]={10+shift,6,11+shift+.8f*sinf(fmaxf(0,age-380)/650*3.14159265358979323846f*2),3};
        for(int k=0;k<4;k++)p.q[10+k]+=(wave[k]-p.q[10+k])*amount;
    }else{
        p.lx=s->intent==2?-1:s->intent==3?1:0;p.ly=s->intent==4?1:s->intent==5?-1:0;
        p.q[0]+=p.lx;p.q[1]+=p.ly;p.q[2]+=.25f*p.lx;
    }
    if(!reduced){const float u=soft(s->response_age/240.f);for(int k=0;k<22;k++)p.q[k]=s->response_from[k]+(p.q[k]-s->response_from[k])*u;}
    if(s->response_age<140&&!reduced)p.lx=p.ly=0;
    if(!live){
        const float u=soft(s->return_age/300.f);for(int k=0;k<22;k++)p.q[k]+=(base[k]-p.q[k])*u;
        if(u>.5f){const uint32_t t=s->idle%18000;p.lx=t>=12800&&t<16400?1:0;p.ly=p.lx&&(s->idle/18000)%3==1&&t>=14400?-1:0;}
    }
    return p;
}
static bool character_hit(const vibe_companion_t *s,float x,float y)
{
    if(x<1.5f||x>12.5f||y>11.5f)return false;
    const maker_pose_t p=companion_pose(s,false);const float *q=p.q;
    if(x>=q[0]-1.8f&&x<=q[0]+2.8f&&y>=q[1]-2.6f&&y<=q[1]+1.6f)return true;
    static const unsigned bones[][2]={{1,2},{1,3},{3,4},{1,5},{5,6}};
    for(unsigned i=0;i<5;i++){
        const unsigned a=bones[i][0]*2,b=bones[i][1]*2;
        const float ax=q[a],ay=q[a+1],dx=q[b]-ax,dy=q[b+1]-ay,len=dx*dx+dy*dy;
        const float u=fmaxf(0,fminf(1,((x-ax)*dx+(y-ay)*dy)/(len?len:1))),ex=x-ax-u*dx,ey=y-ay-u*dy;
        if(ex*ex+ey*ey<=1.21f)return true;
    }
    return false;
}
static int response_intent(const vibe_companion_t *s,float x,float y,bool hit)
{
    if (hit) return 1;
    if (y > 11.5f) return 4;
    if (x < 1.5f) return 2;
    if (x > 12.5f) return 3;
    float q[22];maker_pose(s->idle,q);const float dx=x-q[0]-.5f,dy=y-q[1];
    return fabsf(dy)>fabsf(dx)+1?(dy>0?4:5):(dx<0?2:3);
}
static void capture_response(vibe_companion_t *s,int intent)
{
    const maker_pose_t p=companion_pose(s,false);memcpy(s->response_from,p.q,sizeof(p.q));
    s->response_valid=true;s->response_age=s->return_age=0;s->intent=intent;
}
void vibe_companion_init(vibe_companion_t *s)
{
    memset(s,0,sizeof(*s));s->x=s->y=s->x0=s->y0=s->tap_x=s->tap_y=7;s->tap_gap=1000;s->return_age=300;
}
void vibe_companion_tick(vibe_companion_t *s,uint32_t dt)
{
    if (dt > 100) dt = 100;
    const uint32_t previous = s->intro;
    s->intro=capped(s->intro,dt,VIBE_MOTION_CYCLE_MS);
    if(!s->down&&!s->look&&!s->pose&&s->return_age>=300)s->idle=(s->idle+dt-(s->intro-previous))%54000;
    s->look=remaining(s->look,dt);s->pose=remaining(s->pose,dt);
    s->pose_age=capped(s->pose_age,dt,10000);s->gaze_age=capped(s->gaze_age,dt,10000);s->response_age=capped(s->response_age,dt,10000);
    s->return_age=s->down||s->look||s->pose?0:capped(s->return_age,dt,300);
    s->quiet=s->down?0:capped(s->quiet,dt,60000);s->tap_gap=capped(s->tap_gap,dt,1000);
    if(s->down)s->held=capped(s->held,dt,10000);
}
void vibe_companion_input(vibe_companion_t *s,float x,float y,bool pressed,bool cancelled)
{
    if(cancelled){s->down=s->moved=false;s->pose=s->look=0;s->tap_gap=1000;s->quiet=0;return;}
    x=fmaxf(0,fminf(14,x));y=fmaxf(0,fminf(14,y));
    if(pressed&&!s->down){
        s->intro=VIBE_MOTION_CYCLE_MS;s->hit=character_hit(s,x,y);capture_response(s,response_intent(s,x,y,s->hit));
        s->down=true;s->moved=false;s->held=0;s->x0=x;s->y0=y;s->mood=s->hit?VIBE_FACE_WINK:VIBE_FACE_NEUTRAL;s->pose=s->pose_age=s->gaze_age=0;
    }
    if(!s->down)return;
    s->x=x;s->y=y;s->quiet=0;s->gx=x<5?-1:x>9?1:0;s->gy=y<5?-1:y>9?1:0;s->look=s->hit?1600:1100;
    const float dx=x-s->x0,dy=y-s->y0;if(dx*dx+dy*dy>1.44f)s->moved=true;
    if(s->moved){const int intent=response_intent(s,x,y,false);if(intent!=s->intent)capture_response(s,intent);}
    if (pressed) return;
    s->down=false;
    if(s->moved){
        s->mood=VIBE_FACE_NEUTRAL;s->pose=0;s->tap_gap=1000;
        if(dx>2){capture_response(s,3);const uint32_t base=s->idle/18000*18000;uint32_t t=s->idle%18000;if(t<7400)t=7400;s->idle=base+(t+800<12400?t+800:12400);}
    }else if(s->held>=600&&!s->hit){capture_response(s,3);s->mood=VIBE_FACE_HAPPY;s->tap_gap=1000;s->idle=s->idle/18000*18000+12400;}
    else{
        const float tx=x-s->tap_x,ty=y-s->tap_y;const bool twice=s->tap_gap<=320&&tx*tx+ty*ty<=16;
        if(twice){capture_response(s,1);s->mood=VIBE_FACE_SMILE;s->look=1600;s->tap_gap=1000;s->joy=(s->joy+1)%3;s->idle=((s->idle/18000+1)%3)*18000;}
        else{s->mood=s->hit?VIBE_FACE_WINK:VIBE_FACE_NEUTRAL;s->tap_gap=0;s->tap_x=x;s->tap_y=y;}
    }
}
static void maker_pixel(frame_t f,int x,int y)
{ if(vibe_motion_inside(15,x,y))f.w[y*15+x]=1; }
static void maker_line(frame_t f,int x,int y,int tx,int ty)
{
    const int dx=abs(tx-x),dy=-abs(ty-y),sx=x<tx?1:-1,sy=y<ty?1:-1;int err=dx+dy;
    for(;;){maker_pixel(f,x,y);if(x==tx&&y==ty)break;const int e=2*err;if(e>=dy){err+=dy;x+=sx;}if(e<=dx){err+=dx;y+=sy;}}
}
static void maker_person(frame_t f,maker_pose_t p)
{
    int q[22];for(int k=0;k<22;k++)q[k]=(int)floorf(p.q[k]+.5001f);
    static const unsigned bones[][2]={{1,2},{1,3},{3,4},{1,5},{5,6},{2,7},{7,8},{2,9},{9,10}};
    for(unsigned i=0;i<9;i++){const int a=bones[i][0]*2,b=bones[i][1]*2;maker_line(f,q[a],q[a+1],q[b],q[b+1]);}
    const int hx=q[0],hy=q[1];maker_line(f,hx,hy+1,q[2],q[3]);
    static const char ring[]="0110100110010110";
    for(int i=0;i<16;i++)if(ring[i]=='1')maker_pixel(f,hx+i%4-1,hy+i/4-2);
    if(p.lx||p.ly)maker_pixel(f,hx+(p.lx>0?1:0),hy-1+(p.ly>0?1:0));
}
void vibe_companion_frame(const vibe_companion_t *s,float *white,float *accent,bool reduced)
{
    if(s->intro<VIBE_MOTION_CYCLE_MS&&!reduced){vibe_motion_home(white,accent,s->intro,false);return;}
    const frame_t f={15,white,accent};clear(f);
    if(reduced){maker_person(f,companion_pose(s,true));return;}
    const bool interactive=s->down||s->look||s->pose||s->return_age<300;
    if(interactive)maker_person(f,companion_pose(s,false));
    const uint16_t *row=s_maker_story[s_maker_story_index[(s->idle/18000)%3][(s->idle%18000)/100]];
    for(int y=0;y<15;y++)for(int x=0;x<15;x++){
        const int i=y*15+x;if(!interactive)white[i]=(row[y]>>x)&1;
        if ((row[15+y] >> x) & 1) white[i] = 1;
        accent[i]=white[i]?0:(row[30+y]>>x)&1;
    }
}

void vibe_motion_usb(float *white, float *accent, vibe_usb_motion_t state, uint32_t ms, bool reduced)
{
    frame_t f = {11, white, accent}; clear(f);
    const bool ready = state == VIBE_USB_READY, waiting = state == VIBE_USB_WAITING;
    for (int y = 3; y <= 7; ++y) { put(f, 1, y, .74f, false, false); put(f, 9, y, ready ? .74f : .42f, false, false); }
    for (int y = 3; y <= 7; y += 4) { put(f, 2, y, .7f, false, false); put(f, 8, y, ready ? .7f : .38f, false, false); }
    if (!ready && !waiting) { put(f, 3, 5, .5f, false, false); put(f, 7, 5, .5f, false, false); return; }
    for (int x = 3; x <= 7; ++x) put(f, x, 5, ready ? .55f : .17f, false, false);
    if (waiting) {
        const float p = reduced ? .5f : (ms % 3000) / 3000.0f;
        for (int x = 3; x <= 7; ++x) put(f, x, 5, .55f * clamp(1 - fabsf(x - (1 + p * 9)) / 1.3f), true, false);
    } else {
        const float breath = reduced ? .7f : .68f + .08f * sinf((ms % 3800) / 3800.0f * PI * 2);
        for (int x = 3; x <= 7; ++x) put(f, x, 5, breath, true, false);
        put(f, 2, 5, .8f, true, false); put(f, 8, 5, .8f, true, false);
    }
}
void vibe_motion_status(float *white, float *accent, vibe_status_motion_t kind, uint32_t ms, bool reduced)
{
    frame_t f = {13, white, accent}; clear(f);
    if (kind == VIBE_STATUS_SUCCESS) {
        static const int path[8][2] = {{3,6},{4,7},{5,8},{6,7},{7,6},{8,5},{9,4},{10,3}};
        const float p = reduced ? 1 : clamp(ms / 700.0f);
        for (int i = 0; i < 8; ++i) {
            const float v = ease(p * 8 - i);
            put(f, path[i][0], path[i][1], .94f * v, true, false);
            put(f, path[i][0], path[i][1] + 1, .84f * v, true, false);
        }
    } else if (kind == VIBE_STATUS_FAILED) {
        const float gain = reduced ? 1 : ease(ms / 300.0f);
        for (int i = -3; i <= 3; ++i) { put(f, 6 + i, 6 + i, .9f * gain, true, false); put(f, 6 - i, 6 + i, .9f * gain, true, false); }
    } else if (kind == VIBE_STATUS_BATTERY) {
        for (int x = 2; x <= 9; ++x) { put(f, x, 3, .8f, false, false); put(f, x, 9, .8f, false, false); }
        for (int y = 4; y <= 8; ++y) { put(f, 1, y, .8f, false, false); put(f, 10, y, .8f, false, false); }
        for (int y = 5; y <= 7; ++y) { put(f, 11, y, .7f, false, false); put(f, 3, y, .85f, true, false); }
    } else if (kind == VIBE_STATUS_STORAGE) {
        for (int y = 2; y <= 10; ++y) { put(f, 2, y, .8f, false, false); put(f, 10, y, .8f, false, false); }
        for (int x = 2; x <= 10; ++x) put(f, x, 11, .8f, false, false);
        for (int y = 3; y <= 9; y += 3) for (int x = 4; x <= 8; ++x) put(f, x, y, .85f, y == 3, false);
    } else {
        static const int arcs[21][2] = {{1,4},{2,3},{3,2},{4,2},{5,1},{6,1},{7,1},{8,2},{9,2},{10,3},{11,4},
            {3,5},{4,4},{5,4},{6,4},{7,4},{8,4},{9,5},{5,7},{6,6},{7,7}};
        for (int i = 0; i < 21; ++i) put(f, arcs[i][0], arcs[i][1], .55f, false, false);
        put(f, 6, 9, .85f, true, false); put(f, 6, 10, .85f, true, false);
        for (int i = 0; i < 9; ++i) put(f, 2 + i, 2 + i, .95f, true, false);
    }
}
