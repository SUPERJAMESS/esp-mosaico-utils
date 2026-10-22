#!/usr/bin/env python3
"""Original, crisp 15x15 maker stories using the earlier simple stick silhouette.

Generate the reference rasters as deduplicated C tables. Props never add facial
features, thick outlines, glow or trails to the character.
"""
import math
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
STEP = 100
DURATION = 18000
REST = [7,4, 7,6, 7,9, 5,7,4,9, 9,7,10,9, 6,11,5,13, 8,11,9,13]
BONES = [(1,2),(1,3),(3,4),(1,5),(5,6),(2,7),(7,8),(2,9),(9,10)]
PARTS = [
    [(8,9),(11,9),(11,6)],
    [(10,9),(10,6),(10,3),(8,10),(12,10)],
    [(10,4),(12,7),(10,10),(8,7)],
]

def ease(t):
    t = max(0, min(1, t))
    return t*t*(3-2*t)

def mix(a,b,t):
    return [x+(y-x)*ease(t) for x,y in zip(a,b)]

def rnd(x):
    return math.floor(x+.5)

def inside(x,y):
    return 0 <= x < 15 and 0 <= y < 15 and min(x,14-x)+min(y,14-y) >= 2

def character_pose(kind,t):
    q=REST[:];shift=0;look=[0,0]
    if t<2400:
        think=REST[:];think[10:14]=[11,6,10,3]
        f=ease(t/400) if t<1500 else 1-ease((t-1500)/600)
        q=mix(REST,think,f)
    elif t<3400 or t>=17000:
        u=(t-2400)/1000 if t<3400 else (18000-t)/1000
        shift=-3*ease(u)
        stride=math.sin(u*math.pi*4)*math.sin(u*math.pi)
        q[14:]=[6+stride*.5,11,5+stride,13-abs(stride)*.5,
                8-stride*.5,11,9-stride,13-abs(stride)*.5]
        q[8]+=stride;q[12]-=stride
    else:
        shift=-3
        if t<7300:
            typing=REST[:];typing[6:14]=[9,8,11,10,10,7,11,9+(t//250)%2]
            gain=ease((t-3400)/500)*(1-ease((t-6200)/600))
            q=mix(REST,typing,gain)
        elif t<12400:
            count=len(PARTS[kind]);local=(t-7400)/max(1,4600/count)
            f=local-math.floor(local)
            reach=REST[:];reach[10:14]=[10,7,11,7]
            q=mix(REST,reach,math.sin(max(0,f)*math.pi)) if 7400<=t<12000 else REST[:]
        else:
            # Look first, see the creation move, then celebrate with two small pumps.
            watch=ease((t-12400)/400)*(1-ease((t-16400)/600))
            q[0]+=.75*watch
            if kind==1:q[1]-=.75*ease((t-13800)/1500)*watch
            if t<16400:look=[1,-1 if kind==1 and t>=14400 else 0]
            if 14200<=t<16200:
                u=(t-14200)/2000;lift=max(0,math.sin(u*math.pi*2))
                if u>=.5:lift=max(0,math.sin((u-.5)*math.pi*2))
                happy=q[:];happy[10:14]=[10,7,12,4]
                q=mix(q,happy,lift)
    for i in range(0,22,2):q[i]+=shift
    return q,look

def raster(kind,t):
    w,a = [0]*225,[0]*225
    def pixel(x,y,accent=False):
        x,y=rnd(x),rnd(y)
        if inside(x,y):
            i=y*15+x
            if accent and w[i]: return
            (a if accent else w)[i]=1
            (w if accent else a)[i]=0
    def line(x,y,xx,yy,accent=False):
        x,y,xx,yy=map(rnd,(x,y,xx,yy))
        dx,dy=abs(xx-x),-abs(yy-y);sx=1 if x<xx else -1;sy=1 if y<yy else -1;err=dx+dy
        while True:
            pixel(x,y,accent)
            if (x,y)==(xx,yy):break
            e=2*err
            if e>=dy:err+=dy;x+=sx
            if e<=dx:err+=dx;y+=sy
    def block(x,y):
        for dx in range(2):
            for dy in range(2):pixel(x+dx,y+dy,True)

    q,look=character_pose(kind,t)
    q=[rnd(v*4)/4 for v in q]
    # Character is always a single-cell body with the original four-cell ring.
    pts=[(rnd(q[i]),rnd(q[i+1])) for i in range(0,22,2)]
    for i,j in BONES:line(*pts[i],*pts[j])
    hx,hy=pts[0]
    line(hx,hy+1,*pts[1])
    for y,row in enumerate(['0110','1001','1001','0110']):
        for x,v in enumerate(row):
            if v=='1':pixel(hx+x-1,hy+y-2)
    if any(look):pixel(hx+(1 if look[0]>0 else 0),hy-1+(1 if look[1]>0 else 0))
    character=w[:]
    w=[0]*225

    if 500<=t<2100:
        # A small orange lightbulb, separate from the person's outline.
        for dx,dy in [(0,0),(-1,1),(1,1),(0,2),(0,3)]:pixel(12+dx,1+dy,True)
        if 800<=t<1400:pixel(10,1,True)
    elif 3400<=t<7000:
        # The lid opens as hands approach, closes after the final keystroke.
        top=9-rnd(3*ease((t-3400)/400)*(1-ease((t-6500)/500)))
        for x in range(9,14):pixel(x,top,True);pixel(x,9,True)
        for y in range(top+1,9):pixel(9,y,True);pixel(13,y,True)
        if 3800<=t<6500:
            pixel(10,7);pixel(12,8)
            if (t//300)%2:pixel(11,8)
        line(8,11,14,11,True)
    elif 7000<=t<7400:
        block(10-2*ease((t-7000)/400),9-2*ease((t-7000)/400))
    elif 7400<=t<17000:
        parts=PARTS[kind]
        built=min(len(parts),int((t-7400)*len(parts)/4600))
        dx=dy=0
        if t>=12000:
            built=len(parts)
            if kind==0:dx=rnd(ease((t-13200)/3500)*8)
            if kind==1:dy=-rnd(ease((t-13200)/3500)*15)
        if kind==2 and t>=13200:
            # The same four modules turn around a white hub.
            turn=int((t-13200)/260)%4
            blades=[[(10,4),(12,7),(10,10),(8,7)],
                    [(12,5),(12,9),(8,9),(8,5)],
                    [(10,4),(12,7),(10,10),(8,7)],
                    [(12,5),(12,9),(8,9),(8,5)]][turn]
            for x,y in blades:block(x,y)
        else:
            for x,y in parts[:built]:block(x+dx,y+dy)
            if built<len(parts):
                f=((t-7400)*len(parts)/4600)%1
                x,y=parts[built]
                block(8+(x-8)*ease(f),7+(y-7)*ease(f))
        if built==len(parts):
            if kind==0:
                pixel(10+dx,9);pixel(11+dx,8)
                pixel(9+dx,12);pixel(12+dx,12)
            elif kind==1:
                for yy in (5,8):pixel(10,yy+dy);pixel(11,yy+dy)
                pixel(10,2+dy,True);pixel(11,2+dy,True)
                if t>=13200:
                    pixel(10,12+dy);pixel(11,12+dy)
                    if (t//200)%2:pixel(10,13+dy)
            else:
                line(9,8,12,8);line(11,6,11,11);line(11,11,11,13)
    rows=[]
    for channel in (character,w,a):
        rows.extend(sum(channel[y*15+x]<<x for x in range(15)) for y in range(15))
    return rows

def main():
    frames = [[raster(k, t) for t in range(0, DURATION, STEP)] for k in range(3)]
    poses = [[[rnd(v*4) for v in character_pose(k, t)[0]] for t in range(0, DURATION, STEP)] for k in range(3)]
    def deduplicate(stories):
        unique = list(dict.fromkeys(tuple(row) for story in stories for row in story))
        indices = {row: i for i, row in enumerate(unique)}
        return unique, [[indices[tuple(row)] for row in story] for story in stories]
    def table(ctype, name, rows):
        return ('static const %s %s[%d][%d] = {\n' % (ctype, name, len(rows), len(rows[0])) +
                ',\n'.join('{'+','.join(map(str, row))+'}' for row in rows)+'\n};\n')
    unique_frames, frame_index = deduplicate(frames)
    unique_poses, pose_index = deduplicate(poses)
    header = ('/* Generated by tools/build_maker_story.py. Do not edit by hand. */\n'
              '#pragma once\n#include <stdint.h>\n'
              '#define VIBE_MAKER_DURATION 18000U\n#define VIBE_MAKER_STEP 100U\n')
    header += table('uint16_t', 's_maker_story', unique_frames)
    header += table('uint16_t', 's_maker_story_index', frame_index)
    header += table('int8_t', 's_maker_poses', unique_poses)
    header += table('uint16_t', 's_maker_pose_index', pose_index)
    (ROOT/'components/app_ui/private/vibe_maker_story.h').write_text(header)
    print(f'Generated {len(unique_frames)} unique frames and {len(unique_poses)} poses for three 18-second stories.')

if __name__ == '__main__': main()
