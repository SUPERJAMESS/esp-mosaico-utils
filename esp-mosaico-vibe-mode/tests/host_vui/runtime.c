// SPDX-License-Identifier: Apache-2.0
#include "vui.h"
#include "vui_ids.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
extern int vibe_pc_init(vui_handle_t ui);
extern void vibe_pc_deinit(vui_handle_t ui);
static unsigned releases, ticks;
static void released(void *data) { releases++; free(data); }
static void timer(vui_handle_t ui, void *ctx) { ticks++; assert(vui_timer_delete(ui,*(void **)ctx)==0); }
int main(void)
{
    vui_handle_t ui=NULL;
    assert(vui_create(&ui)==0);
    void *handle=vui_timer_create(ui,10,timer,&handle);
    assert(handle);
    vui_tick(ui,10);vui_tick(ui,100);assert(ticks==1);
    unsigned id=VUI_VIBE_BIND_IDEA_TITLE_PIXELS;
    void *data=malloc(328*84*2);
    assert(vui_canvas_push(ui,id,data,1,released,data)==VUI_ERR_INVALID_ARG);
    assert(releases==0);
    assert(vui_canvas_push(ui,id,data,328*2,released,data)==0);
    void *next=malloc(328*84*2);
    assert(vui_canvas_push(ui,id,next,328*2,released,next)==0 && releases==1);
    assert(vui_canvas_stop(ui,id)==0 && releases==2);
    assert(vui_canvas_stop(ui,id)==0 && releases==2);
    assert(vibe_pc_init(ui)==0);
    uint16_t *frame=malloc(480*480*2);
    unsigned random=73078;
    for(unsigned i=0;i<6000;i++) {
        random=random*1664525U+1013904223U;
        if(i%3==0)vui_pointer(ui,(random>>8)%480,(random>>18)%480,(i%9)!=0);
        vui_tick(ui,20);
        vui_render(ui,frame);
    }
    vibe_pc_deinit(ui);
    data=malloc(328*84*2);
    assert(vui_canvas_push(ui,id,data,328*2,released,data)==0);
    vui_destroy(ui);assert(releases==3);free(frame);
    puts("Vui canvas ownership, timer retirement and input stress passed");
}
