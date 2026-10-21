"""Adapt the f0b53c0 UI handoff to the retained firmware's live widgets.

Coordinates, typography, icons and colors follow the supplied Vibe Mode 0.3
mockups. Service bindings stay in the product; no reference sample apps ship.
"""
from pathlib import Path
import json

BG, CARD, RAISED, TILE = '#090A0B', '#191B1D', '#272A2D', '#272A2D'
TEXT, MUTED, ORANGE, RED = '#F3F4F2', '#B2B7BA', '#FF6439', '#FF8A85'
LINE, ICON_BG = '#3D4145', '#202326'
FONT = {'sans': 'fonts/VibeInter.otf', 'medium': 'fonts/VibeMedium.otf',
        'title': 'fonts/VibeDisplay.otf', 'hero': 'fonts/ArchivoExtended-Black.ttf',
        'doto': 'fonts/Doto-Black.ttf'}


def apply(scene):
    objects = scene['objects']
    named = {o['name']: o for o in objects if o.get('name')}
    index = {o['name']: i for i, o in enumerate(objects) if o.get('name')}
    order = [len(objects)]

    def front(obj):
        order[0] += 1
        obj['_paint_order'] = order[0]

    def add(kind, parent, x, y, w, h, **props):
        obj = dict(type=kind, parent=parent, x=x, y=y, w=w, h=h, **props)
        front(obj)
        objects.append(obj)
        if obj.get('name'):
            named[obj['name']] = obj
            index[obj['name']] = len(objects)-1
        return len(objects)-1

    def move(name, x, y, w, h, **props):
        named[name].update(dict(x=x, y=y, w=w, h=h, hidden=False) | props)
        front(named[name])
        return index[name]

    def hide(name):
        named[name]['hidden'] = True

    def rect(parent, x, y, w, h, color=CARD, radius=20, **props):
        return add('container', parent, x, y, w, h, bg_color=color, radius=radius, **props)

    def text(parent, name, value, x, y, w, h=26, size=18, role='sans', color=TEXT, align='left', **props):
        args = dict(text=value, font_size=size, font=FONT[role], fg_color=color, text_align=align, **props)
        if name in named:
            return move(name, x, y, w, h, parent=parent, **args)
        return add('label', parent, x, y, w, h, name=name, bind=name, **args)

    def icon(parent, name, x, y, size=32):
        return add('image', parent, x, y, size, size, image='assets/icons/'+name+'.png')

    def click(parent, name, x, y, w, h, action=None, arg=None):
        event = dict(event='click', action='call', target_name=action or name)
        if arg is not None: event['arg'] = arg
        if name in named:
            obj = named[name]
            obj.update(type='button', parent=parent, x=x,y=y,w=w,h=h, hidden=False,
                       text='', bg_color='#00000001', events=[event])
            obj.pop('bind', None)
            front(obj)
            return index[name]
        return add('button',parent,x,y,w,h,name=name,bg_color='#00000001',events=[event])

    def button(parent,name,title,x,y,w,h=84,primary=False,danger=False,action=None):
        # Keep the established label binding on the visible button.
        bind = named.get(name,{}).get('bind')
        args=dict(parent=parent,text=title,font=FONT['medium'],font_size=22,
                  fg_color=RED if danger else BG if primary else TEXT,
                  bg_color=TEXT if primary else RAISED,radius=20)
        if name in named: i=move(name,x,y,w,h,**args)
        else:
            i=add('button',parent,x,y,w,h,name=name,**{k:v for k,v in args.items() if k!='parent'},
                  events=[dict(event='click',action='call',target_name=action or name)])
        if bind: objects[i]['bind']=bind
        return i

    def header(parent,title,back,size=30,role='title',title_name=None,title_visible=True):
        for o in objects:
            if o.get('parent')==parent and (o.get('text')=='‹' or o.get('name','').endswith('_title') or (o['type']=='label' and o.get('y',999)<=40)):
                o['hidden']=True
        rect(parent,16,20,56,56,RAISED,28)
        icon(parent,'back-32-white',28,32)
        action = named.get(back,{}).get('events',[{}])[0].get('target_name','product_back')
        click(parent,back,2,6,84,84,action)
        if title_visible:
            text(parent,title_name or 'heading_'+str(parent),title,88,29,368,40,size,role,overflow='ellipsis')

    def row(parent, name, title, subtitle, glyph, y, action, subtitle_name=None, danger=False):
        rect(parent,16,y,448,80)
        rect(parent,30,y+14,52,52,ICON_BG,16)
        icon(parent,glyph.replace('-32-', '-38-'),37,y+21,38)
        text(parent,'row_title_'+name,title,100,y+(8 if subtitle is not None else 22),312,34,24,'medium',RED if danger else TEXT,overflow='ellipsis')
        if subtitle is not None:
            text(parent,subtitle_name or 'row_sub_'+name,subtitle,100,y+42,312,28,20,color=MUTED,overflow='ellipsis')
        icon(parent,'chevron-20-dim',424,y+30,20)
        click(parent,'button_'+name,16,y,448,80,action)

    # Defaults are the design's grayscale palette and original font hierarchy.
    for o in objects:
        if 'text' in o:
            o['font']=FONT['sans']
            o['fg_color']=MUTED if o.get('fg_color') in ('#91919B','#999999') else TEXT
        if o.get('bg_color') in ('#181818','#242426'): o['bg_color']=CARD
    for o in objects:
        if o.get('parent') == -1 and o.get('type') == 'layer': o['bg_color']=BG
    # Root/home. Old diagnostic widgets remain hidden for controller compatibility.
    for o in objects:
        if o.get('parent')==0: o['hidden']=True
    text(0,'home_brand','Vibe Mode',30,20,88,24,16,color=MUTED)
    text(0,'home_version','',122,20,116,24,16,color=MUTED,overflow='ellipsis')
    text(0,'home_nav_badge','',184,20,148,24,16,'sans',MUTED,'center',overflow='ellipsis')
    text(0,'home_status','',324,20,112,24,16,color=MUTED,align='right')
    rect(0,8,56,464,188,TEXT,24)
    text(0,'home_headline','make ideas',24,72,244,44,35,'title',BG)
    text(0,'home_run','run',24,88,244,134,116,'hero',ORANGE)
    # Keep locale gates for live language updates. The headline stays English.
    en=add('layer',0,0,0,480,480,name='home_heading_en',bind='home_heading_en_visible',bind_target='visible')
    zh=add('layer',0,0,0,480,480,name='home_heading_zh',bind='home_heading_zh_visible',bind_target='visible',hidden=True)
    text(0,'home_caption','Snap ideas together.',24,205,244,31,22,'sans',BG)
    wifi_pill=add('layer',0,0,0,480,480,name='home_wifi_pill',bind='home_wifi_pill_visible',bind_target='visible',hidden=True)
    rect(wifi_pill,20,198,248,38,'#E03E3A',12)
    text(wifi_pill,'home_wifi_btn_label','Wi-Fi not connected ›',20,203,248,28,18,'medium','#FFFFFF','center')
    click(0,'home_wifi',20,198,248,38,'home_wifi')
    add('matrix',0,280,64,172,172,name='home_hero')
    rect(0,8,256,464,84,CARD,18,border_color=LINE,border_width=1)
    text(0,'home_ideas_title','My Ideas',24,261,298,40,28,'medium')
    text(0,'home_ideas_sub','Open your next possibility',24,300,298,26,18,color=MUTED)
    text(0,'home_ideas_count','',334,269,84,56,44,'title',align='right')
    icon(0,'arrow-28-orange',430,284,28)
    click(0,'home_ideas',8,256,464,84,'home_ideas')
    for i,(name,title,sub,glyph) in enumerate((('develop','Develop','Build one','code'),('manage','Manage','Download · update','grid'),('settings','Settings','Device','settings'))):
        x=8+159*i
        rect(0,x,352,146,92,CARD,18,border_color=LINE,border_width=1)
        icon(0,glyph+'-36-white',x+55,364,36)
        text(0,'nav_'+name+'_label',title,x+4,403,138,32,22,'medium',align='center')
        click(0,'button_nav_'+name,x,352,146,92,'nav_'+name)
    def pager(parent, name, pages, active):
        gate=add('layer',parent,0,0,480,480,name=name,bind=name+'_visible',bind_target='visible',hidden=True)
        x=(480-(pages*8+12+(pages-1)*8))//2
        for i in range(pages):
            width=20 if i==active else 8
            rect(gate,x,452,width,7,ORANGE if i==active else LINE,4)
            x+=width+8
    # VIBE_BUNDLE_MAX=16: one Home page plus up to three six-cell Idea pages.
    # The Add Idea tile counts as a cell, including in the empty library.
    for pages in range(2,5):pager(0,'home_pager_'+str(pages),pages,0)

    # Live status icons retain the reference's compact status bar.
    hide('home_status')
    STATUS_ICONS = (
        ('wifi','wifi-22-white',360,False),
        ('wifi_off','wifi_off-22-grey',360,True),
        ('usb','usb-22-white',394,False),
        ('usb_agent','usb-22-orange',394,True),
        ('usb_off','usb_off-22-grey',394,True),
        ('battery','battery_full-22-white',428,False),
        ('battery_full_green','battery_full-22-green',428,True),
        ('battery_mid','battery_mid-22-white',428,False),
        ('battery_low','battery_low-22-yellow',428,False),
        ('battery_charging','battery_charging-22-white',428,False),
        ('battery_charging_green','battery_charging-22-green',428,True),
        ('battery_charging_yellow','battery_charging-22-yellow',428,True),
        ('battery_unknown','battery_unknown-22-grey',428,True),
    )
    for name,glyph,x,hidden_default in STATUS_ICONS:
        gate=add('layer',0,0,0,480,480,name='status_'+name,bind='status_'+name+'_visible',bind_target='visible',hidden=hidden_default)
        icon(gate,glyph,x,20,22)

    # Shared header geometry, with existing controller actions.
    for p,title,back in ((15,'Wi-Fi','button_back_wifi'),(31,'Join Network','button_back_password'),
                        (39,'TCP pairing','button_back_pairing'),(49,'Download Idea','button_back_bridge'),
                        (59,'Manage','button_back_nand'),(69,'Idea Details','button_back_confirm')):
        for o in objects:
            if o.get('parent')==p and o['type']=='label' and not o.get('bind'): o['hidden']=True
        header(p,title,back)
    for name,title in (('develop','Develop'),('settings','Settings'),('display','Display & Haptics'),('language','Language'),
                       ('shared','Shared Info'),('shared_editor','Shared Info'),('about','About'),('reset','Factory Reset'),
                       ('delete_confirm','Delete Idea'),('boot_tip','Update Vibe Mode')):
        header(index[name],title,'button_back_'+name,
               size=28 if name=='delete_confirm' else 30,
               title_name='delete_confirm_title' if name=='delete_confirm' else None,
               title_visible=name!='shared_editor')

    # Help and guide are real destinations; QR points to the supplied guide.
    help_page=add('layer',-1,0,0,480,480,name='help',bind='help_visible',bind_target='visible',hidden=True)
    header(help_page,'Help','button_back_help',title_name='help_heading')
    help_root=add('layer',help_page,0,0,480,480,name='help_root_layer',bind='help_root_layer_visible',bind_target='visible')
    for i,(title,sub,glyph,action) in enumerate((
            ('Getting Started','Set up and run your first Idea','flag','help_topic_0'),
            ('Develop Ideas','Build one with your AI agent','code','help_topic_1'),
            ('Idea IDs & Sharing','Find, download and share','hash','help_topic_2'),
            ('Troubleshooting','Wi-Fi, storage, stuck Ideas','support','help_topic_3'))):
        row(help_root,'help_'+str(i),title,sub,glyph+'-32-cream',96+88*i,action)
    help_steps=add('layer',help_page,0,0,480,480,name='help_steps_layer',bind='help_steps_layer_visible',bind_target='visible',hidden=True)
    for i in range(1,4):
        if i>1:
            rect(help_steps,64,96+(i-1)*88,392,1,LINE,0)
        sy=108+(i-1)*88
        text(help_steps,f'help_step_num_{i}',str(i),24,sy,28,32,24,'medium',ORANGE,'center')
        text(help_steps,f'help_step_body_{i}','Plug in USB and ask your agent to build an Idea.',64,sy-4,392,64,20,'sans',TEXT)
    btn_hqr=button(help_steps,'button_help_qr','',24,380,432,84,action='help_open_qr')
    named['button_help_qr'].update(bg_color=RAISED,radius=20,text='')
    for key, glyph in (('qr','qr-20-white'), ('wifi','wifi-32-cream'), ('storage','storage-32-cream')):
        gate=add('layer',btn_hqr,0,0,432,84,name='help_action_'+key,bind='help_action_'+key+'_visible',bind_target='visible',hidden=key!='qr')
        icon(gate,glyph,66,32,20)
    text(btn_hqr,'help_qr_caption','Full guide on your phone',96,26,316,32,22,'medium',TEXT,'left')
    help_fix=add('layer',help_page,0,0,480,480,name='help_fix_layer',bind='help_fix_layer_visible',bind_target='visible',hidden=True)
    for i,(title,sub,glyph,action) in enumerate((
            ('Wi-Fi Connection','Check networks and passwords','wifi','fix_wifi'),
            ('Storage Is Full','Manage installed Ideas','storage','fix_storage'),
            ('Return to Vibe Mode','View the AI-key startup steps','replay','fix_return'))):
        row(help_fix,'fix_'+str(i),title,sub,glyph+'-32-cream',96+88*i,action)
    btn_fqr=button(help_fix,'button_fix_qr','',24,380,432,84,action='fix_open_qr')
    named['button_fix_qr'].update(bg_color=RAISED,radius=20,text='')
    icon(btn_fqr,'qr-20-white',66,32,20)
    text(btn_fqr,'fix_qr_caption','Full guide on your phone',96,26,316,32,22,'medium',TEXT,'left')

    guide=add('layer',-1,0,0,480,480,name='guide',bind='guide_visible',bind_target='visible',hidden=True)
    header(guide,'View on your phone','button_back_guide',title_name='guide_heading')

    for p, prefix in ((guide, 'guide'),):
        text(p,prefix+'_one','Scan for the coding guide.',24,102,432,34,22,'sans',color=TEXT,align='center')
        text(p,prefix+'_two','',260,182,204,26,16,color=MUTED,hidden=True)
        text(p,prefix+'_three','',260,208,204,26,16,color=MUTED,hidden=True)
        rect(p,138,148,203,203,TEXT,18)
        for qname,qfile,qhid in (('develop','develop.png',False),('start','help-start.png',True),('ids','help-ids.png',True),('fix','help-fix.png',True)):
            qgate=add('layer',p,0,0,480,480,name=f'guide_qr_{qname}',bind=f'guide_qr_{qname}_visible',bind_target='visible',hidden=qhid)
            add('image',qgate,147,157,185,185,image='assets/qr/'+qfile)
        rect(p,24,374,432,54,RAISED,27,border_color=LINE,border_width=1)
        icon(p,'globe-20-grey',46,391,20)
        text(p,prefix+'_url','mosaico.espressif.com/guide/vibe-coding',76,387,364,28,18,color=TEXT,overflow='ellipsis')
    p_boot=index['boot_tip']
    text(p_boot,'boot_tip_top','Power off; hold Boot while turning on.',24,92,432,30,20,'sans',TEXT)
    rect(p_boot,20,136,203,203,TEXT,18)
    add('image',p_boot,29,145,185,185,image='assets/qr/vibe-update.png')
    move('boot_note',236,148,224,68,font_size=20,font=FONT['sans'],fg_color=TEXT,
         text='Scan for the steps\nand what\'s new.')
    rect(p_boot,232,228,232,68,RAISED,16,border_color=LINE,border_width=1)
    icon(p_boot,'globe-20-grey',244,252,20)
    text(p_boot,'boot_tip_url','mosaico.espressif.com/\nvibe/update',270,238,186,48,16,color=TEXT)
    text(p_boot,'boot_tip_how','Release Boot after entering ROM.',24,352,432,30,20,'sans',MUTED,'center')
    rect(p_boot,24,398,168,36,'#33231E',10,border_color=ORANGE,border_width=1)
    text(p_boot,'boot_key_1_num','1',32,404,22,24,18,'medium',ORANGE,'center')
    text(p_boot,'boot_key_1_text','Hold Boot',56,402,130,28,18,'medium',TEXT,'center')
    icon(p_boot,'arrow_down-24-orange',96,444,24)
    rect(p_boot,288,398,168,36,RAISED,10,border_color=LINE,border_width=1)
    text(p_boot,'boot_key_2_num','2',296,404,22,24,18,'medium',MUTED,'center')
    text(p_boot,'boot_key_2_text','Turn on',320,402,130,28,18,'medium',TEXT,'center')
    icon(p_boot,'arrow_down-24-white',360,444,24)

    p=index['develop']
    rect(p,24,96,432,114,CARD,20,border_color=LINE,border_width=1)
    text(p,'develop_eyebrow','USB / AGENT',44,104,278,26,18,color=MUTED)
    text(p,'develop_usb','',44,130,278,32,24,'medium')
    for dname,dcol,dhid in (('ready','#8FD69C',True),('wait','#EFC46C',True),('off','#737878',False)):
        dgate=add('layer',p,0,0,480,480,name='develop_dot_'+dname,bind='develop_dot_'+dname+'_visible',bind_target='visible',hidden=dhid)
        rect(dgate,44,173,10,10,dcol,5)
    text(p,'develop_agent','',62,164,260,28,20,color=MUTED)
    add('matrix',p,335,98,110,110,name='develop_signal')
    click(p,'develop_connection_help',24,96,432,114,'show_guide')
    def compact_row(parent, name, title, subtitle, glyph, y, action, subtitle_name=None):
        rect(parent,16,y,448,72)
        rect(parent,30,y+10,52,52,ICON_BG,16)
        icon(parent,glyph.replace('-32-', '-38-'),37,y+17,38)
        text(parent,'row_title_'+name,title,100,y+6,312,32,24,'medium',TEXT,overflow='ellipsis')
        text(parent,subtitle_name or 'row_sub_'+name,subtitle,100,y+38,312,26,18,color=MUTED,overflow='ellipsis')
        icon(parent,'chevron-20-dim',424,y+26,20)
        click(parent,'button_'+name,16,y,448,72,action)
    compact_row(p,'develop_run','Run Current Build','','play-32-cream',218,'run_development','develop_run_detail')
    compact_row(p,'develop_save','Save Current Build','','storage-32-cream',296,'save_development','develop_save_detail')
    compact_row(p,'develop_guide','Vibe-code an Idea','Scan for the coding guide','code-32-cream',374,'show_guide')
    text(p,'develop_feedback','',24,450,432,26,18,color=MUTED,align='center')

    # One clipped viewport: drawing and hit testing share the scroll offset.
    p=index['settings']
    for o in objects:
        if o.get('parent')==p and o.get('name','').startswith('button_nav_'): o['hidden']=True
    viewport=add('container',p,0,96,480,368,name='settings_viewport',scroll=True,content_height=648)
    rows=[('wifi','Wi-Fi','','wifi'),('display','Display & Haptics','Screen off after 3 min','display'),
          ('language','Language','English','globe'),('shared','Shared Info','','key'),
          ('help','Help','Get started with Vibe Mode','help'),('about','About','Version · device · storage','chip'),
          ('reset','Factory Reset','Clear preferences · keep NAND','replay')]
    for i,(name,title,sub,glyph) in enumerate(rows):
        group='settings_top' if i<4 else 'settings_bottom'
        row(viewport,group+'_'+name,title,sub,glyph+'-32-cream',88*i+(16 if i>=4 else 0)+(16 if i>=6 else 0),'nav_'+name,
            group+'_'+name+'_sub',danger=False)

    # Manage: icon actions, capacity, 80px cards with 8px gaps.
    text(59,'nand_scan','',24,94,432,28,20,color=MUTED,overflow='ellipsis')
    hide('button_nand_scan');hide('button_manage_add')
    for x,glyph,action in ((324,'upgrade-32-white','update_all'),(408,'plus-32-white','download')):
        target=59
        if action=='update_all':
            target=add('layer',59,0,0,480,480,name='manage_update_action',bind='manage_update_action_visible',bind_target='visible')
        rect(target,x,20,56,56,RAISED,28);icon(target,glyph,x+12,32)
        click(target,'manage_'+action,x-8,12,72,72,action)
    badge=add('layer',59,0,0,480,480,name='manage_update_badge',bind='manage_update_badge_visible',bind_target='visible',hidden=True)
    rect(badge,360,16,24,24,ORANGE,12)
    text(badge,'manage_update_count','',360,17,24,23,16,color=BG,align='center')
    move('bundles',16,148,448,316,item_height=88,hidden=True)
    objects[65].update(w=448,h=88,bg_color=BG,radius=0)
    # The row background must precede text in the template's child ordering.
    rect(65,0,0,448,80,CARD,20)
    text(65,'bundles_title','',84,8,312,34,24,'medium',overflow='marquee')
    add('image',65,84,12,312,30,name='bundles_pixels',bind='bundles_pixels',bind_target='resource',dynamic_image=True,overflow='marquee')
    text(65,'bundles_detail','',84,42,312,28,20,color=MUTED,dynamic_color=True,overflow='ellipsis')
    move('bundles_icon',12,12,56,56,radius=12)
    objects[68].update(x=408,y=25,w=24,h=30,font_size=22);front(objects[68])
    add('progress',59,24,128,432,6,name='storage_bar',bind='storage_bar',min=0,max=100,value=0,bg_color=TILE,fg_color=TEXT,radius=3)
    empty=add('layer',59,0,0,480,480,name='library_empty',bind='library_empty_visible',bind_target='visible',hidden=True)
    add('image',empty,200,190,80,80,image='assets/mosaic/hero_empty-80.png')
    text(empty,'library_empty_title','No Ideas yet',24,290,432,30,22,'medium',align='center')
    text(empty,'library_empty_note','Tap + to download your first Idea.',24,334,432,54,18,color=MUTED,align='center')
    retry=add('layer',empty,0,0,480,480,name='retry_storage_gate',bind='button_retry_storage_visible',bind_target='visible',hidden=True)
    button(retry,'button_retry_storage','Retry',140,404,200,56)
    named['button_retry_storage'].update(font=FONT['sans'],font_size=20)
    named['button_retry_storage']['events']=[{'event':'click','action':'call','target_name':'retry_storage'}]
    move('undo_message',40,405,280,48,font_size=16)
    button(59,'button_undo_delete','Undo',332,405,108,48)

    # Download keypad: exact reference spacing and separate digit cells.
    for o in objects:
        if o.get('parent')==49 and (o['type']=='container' or o.get('name') in ('bridge_code','bridge_code_compact')): o['hidden']=True
    for i in range(6):
        x=82+52*i+(12 if i>=3 else 0)
        rect(49,x,92,44,56,CARD,8)
        fgate=add('layer',49,0,0,480,480,name='code_focus_'+str(i),bind='code_focus_'+str(i)+'_visible',bind_target='visible',hidden=i!=0)
        rect(fgate,x,92,44,56,'#00000001',8,border_color=ORANGE,border_width=2)
        text(49,'code_digit_'+str(i),'',x,102,44,37,28,'title',align='center')
    text(49,'bridge_detail','Enter the 6-digit ID from Mosaico Ideas',24,151,432,30,20,color=MUTED,align='center',overflow='ellipsis',dynamic_color=True)
    rect(49,408,20,56,56,RAISED,28);icon(49,'help-32-white',420,32)
    click(49,'download_help',394,6,84,84,'download_help')
    for n in range(1,10):
        x,y=92+104*((n-1)%3),188+72*((n-1)//3)
        button(49,'button_code_'+str(n),str(n),x,y,88,56)
        named['button_code_'+str(n)].update(font=FONT['title'],font_size=28,bg_color=TILE,radius=16)
    button(49,'button_code_0','0',196,404,88,56);named['button_code_0'].update(font=FONT['title'],font_size=28,bg_color=TILE,radius=16)
    click(49,'button_code_delete',92,404,88,56,'code_delete');named['button_code_delete'].update(bg_color=ORANGE,radius=16);icon(index['button_code_delete'],'backspace-32-white',28,12,32)
    button(49,'button_download_code','Go',300,404,88,56);named['button_download_code'].update(font_size=22,bg_color=TILE,fg_color=TEXT,radius=16)
    go_ready=add('layer',index['button_download_code'],0,0,88,56,name='download_go_ready',bind='download_go_ready_visible',bind_target='visible',hidden=True)
    rect(go_ready,0,0,88,56,ORANGE,16)
    text(go_ready,'download_go_ready_label','Go',0,12,88,32,24,'medium',BG,'center')

    # Wi-Fi and password use the same row typography and native keyboard.
    for i in (18,): objects[i]['hidden']=True
    rect(15,16,96,448,80,CARD,20)
    rect(15,30,110,52,52,ICON_BG,16);icon(15,'wifi-38-cream',37,117,38)
    text(15,'wifi_connection','Not connected',100,104,248,34,24,'medium',overflow='ellipsis')
    text(15,'wifi_detail','Choose a network below',100,139,340,28,20,color=MUTED,overflow='ellipsis')
    button(15,'button_forget','Forget',350,102,100,32,danger=True);named['button_forget'].update(font_size=18,font=FONT['sans'])
    text(15,'wifi_scan','Available networks',32,188,416,30,20,color=MUTED,overflow='ellipsis')
    rect(15,408,20,56,56,RAISED,28);icon(15,'refresh-32-white',420,32)
    click(15,'button_scan',394,6,84,84,'scan')
    move('networks',16,224,448,216,item_height=88)
    objects[25].update(w=448,h=88);objects[26].update(w=448,h=80,bg_color=CARD,radius=20)
    rect(25,14,14,52,52,ICON_BG,16)
    text(25,'networks_title','',84,8,312,34,24,'medium',overflow='ellipsis')
    text(25,'networks_detail','',84,42,312,28,20,color=MUTED,overflow='ellipsis')
    icon(25,'wifi-38-cream',21,21,38)
    text(15,'wifi_note','',24,444,432,28,18,color=MUTED,overflow='ellipsis')
    text(31,'ssid','',88,29,368,40,30,'title',overflow='ellipsis');hide('heading_31')
    rect(31,16,96,448,64)
    text(31,'password_display','Enter password',36,113,332,30,22,'medium')
    move('button_toggle_password',392,96,72,64,bg_color='#00000001')
    text(index['button_toggle_password'],'password_toggle','Show',0,18,72,26,16,align='center',hidden=True)
    pw_eye_off=add('layer',index['button_toggle_password'],0,0,72,64,name='password_eye_off',bind='password_eye_off_visible',bind_target='visible')
    icon(pw_eye_off,'eye-32-white',20,16,32)
    pw_eye_on=add('layer',index['button_toggle_password'],0,0,72,64,name='password_eye_on',bind='password_eye_on_visible',bind_target='visible',hidden=True)
    icon(pw_eye_on,'eye_off-32-white',20,16,32)
    text(31,'password_error','',32,168,416,53,22,color=RED)
    move('password_keyboard',0,224,480,233,key_color=TILE,function_color=TILE,delete_color=TILE,space_color=TILE,ok_color=TEXT,ok_text_color=BG)

    for kb in ('password_keyboard','shared_keyboard'):
        named[kb].update(font=FONT['medium'],font_size=22,function_font_size=18,symbols_label='123',space_label='_')

    # Idea details use the original open/delete footer and a compact metadata block.
    for o in objects:
        if o.get('parent')==69 and o['type']=='container' and o.get('y')!=20:o['hidden']=True
    hide('heading_69');hide('button_cancel_install')
    text(69,'release','',88,29,368,40,30,'title',overflow='marquee')
    add('image',69,88,29,368,40,name='release_pixels',bind='release_pixels',bind_target='resource',dynamic_image=True,hidden=True,overflow='marquee')
    add('image',69,24,92,80,80,name='local_icon',bind='local_icon',image='assets/idea-default.svg',dynamic_image=True,radius=18)
    text(69,'local_status','On this device',124,93,268,32,22,'medium')
    text(69,'release_detail','',124,129,268,40,20,color=MUTED)
    rect(69,24,174,432,1,LINE,0);rect(69,24,192,4,152,ORANGE,0)
    local_description=add('container',69,24,188,432,156,name='local_description_viewport',scroll=True,content_height=176)
    text(local_description,'release_path','',8,0,416,176,22,color=MUTED)
    add('image',local_description,8,0,416,176,name='local_body_pixels',bind='local_body_pixels',dynamic_image=True,hidden=True)
    button(69,'button_check_idea','',400,100,56,56)
    named['button_check_idea'].update(radius=28)
    icon(index['button_check_idea'],'refresh-32-white',12,12,32)
    hide('button_preview_idea')
    button(69,'button_update_idea','Update',24,360,136,96)
    button(69,'button_install','Run',172,360,136,96,primary=True)
    named['button_install']['bg_color']=ORANGE
    button(69,'button_start_idea','Delete',320,360,136,96,danger=True)
    for name,glyph,x in (('button_update_idea','refresh-32-white',24),('button_install','play-32-black',172),('button_start_idea','trash-32-red',320)):
        # Icon and label live under the actual button, including its visibility gate.
        named[name]['text']=''
        icon(index[name],glyph,52,14)
        text(index[name],name+'_caption',{'button_update_idea':'Update','button_install':'Run','button_start_idea':'Delete'}[name],4,53,128,34,24,'medium',RED if name=='button_start_idea' else BG if name=='button_install' else TEXT,'center')
    update_footer=add('layer',69,0,0,480,480,name='local_update_footer',bind='local_update_footer_visible',bind_target='visible',hidden=True)
    for name in ('button_update_idea','button_install','button_start_idea'):
        named[name]['parent']=update_footer
    normal_footer=add('layer',69,0,0,480,480,name='local_normal_footer',bind='local_normal_footer_visible',bind_target='visible')
    for name,title,glyph,x,action in (('button_local_open','Run','play-32-black',24,'install'),('button_local_delete','Delete','trash-32-red',248,'delete_idea')):
        parent=button(normal_footer,name,'',x,360,208,96,primary=action=='install',danger=action=='delete_idea',action=action)
        if action=='install': named[name]['bg_color']=ORANGE
        icon(parent,glyph,88,14)
        text(parent,name+'_caption',title,4,53,200,34,24,'medium',BG if action=='install' else RED,'center')
    # The update check opens the real remote preview with the download action.
    # The controller exposes Update only when the selected release has an update.
    header(117,'Idea Details','button_back_remote')
    click(117,'button_back_remote',2,6,84,84,'cancel_idea')
    hide('idea_heading');hide('heading_117')
    # Reflow published title glyphs into a native single-line cache. The header
    # clips and scrolls it without stretching. The body keeps display/1 geometry.
    text(117,'idea_name','Loading app information...',88,29,368,40,24,'medium',overflow='marquee')
    move('idea_title_pixels',88,29,368,40,hidden=True,overflow='marquee')
    move('idea_icon',24,92,80,80,radius=18)
    text(117,'idea_status','',124,92,332,30,20,overflow='ellipsis')
    text(117,'idea_loading_status','Please wait',32,92,416,30,20,color=MUTED)
    text(117,'idea_version','',124,126,332,24,20,color=MUTED,overflow='ellipsis')
    text(117,'idea_summary','',124,154,332,24,20,color=MUTED,overflow='ellipsis')
    rect(117,24,186,432,1,LINE,0)
    viewport=add('container',117,24,200,432,168,name='idea_description_viewport',scroll=True,content_height=176)
    text(viewport,'idea_description','Loading...',8,0,416,176,22)
    move('idea_body_pixels',8,0,416,176,parent=viewport,hidden=True)
    button(117,'button_cancel_idea','Cancel',24,380,208,84)
    button(117,'button_accept_idea','Download',248,380,208,84,primary=True)

    # Language selection matches the side-by-side reference cards.
    for name in ('language','welcome'):
        p=index[name]
        hide(name+'_note')
        y=108 if name=='language' else 260
        button(p,'button_'+name+'_en','English',24,y,432 if name=='language' else 210,88)
        button(p,'button_'+name+'_zh','简体中文',24 if name=='language' else 246,y+100 if name=='language' else y,432 if name=='language' else 210,88)
        for i,language in enumerate(('en','zh')):
            selected=add('layer',p,0,0,480,480,name=f'{name}_{language}_selected',bind=f'{name}_{language}_selected_visible',bind_target='visible',hidden=True)
            sx,sy,sw=(24,y+i*100,432) if name=='language' else (24+i*222,y,210)
            rect(selected,sx,sy,sw,88,TEXT,20)
            text(selected,f'{name}_{language}_selected_label','English' if i==0 else '简体中文',sx,sy+25,sw,38,24,'medium',BG,'center')
        if name=='welcome':
            hide('welcome_title')
            add('image',p,180,40,120,120,image='assets/mosaic/hero_vibe-120.png')
            text(p,'welcome_heading','Choose Your Language',24,172,432,40,28,'title',align='center')
            text(p,'welcome_chinese','选择语言',24,210,432,40,28,color=MUTED,align='center')
            text(p,'welcome_later','Change later in Settings.',24,368,432,30,22,color=MUTED,align='center')
            text(p,'welcome_later_zh','之后可在「设置」里更改。',24,402,432,30,22,color=MUTED,align='center')
        else:
            rect(p,28,320,424,1,LINE,0);rect(p,28,348,4,60,ORANGE,0)
            text(p,'language_explanation','The interface changes now.\nIdea names stay as published.',48,342,404,68,22,color=MUTED)

    # Display: reference segmented controls and toggle rows, scrolling viewport.
    p=index['display']
    for o in objects:
        if o.get('parent')==p and o.get('name') not in ('heading_'+str(p),'button_back_display') and o.get('name'):
            o['hidden']=True
    gate=add('container',p,0,92,480,372,name='display_viewport',scroll=True,content_height=412)
    text(gate,'brightness_title','BRIGHTNESS',32,0,424,28,20,'medium',color=MUTED)
    text(gate,'timeout_title','SCREEN OFF AFTER',32,116,424,28,20,'medium',color=MUTED)
    for i in range(5):
        move('button_brightness_'+str(i),24+i*88,32,80,72,parent=gate,font_size=22,font=FONT['medium'],bg_color=RAISED,radius=24)
        move('button_timeout_'+str(i),24+i*88,148,80,72,parent=gate,font_size=20,font=FONT['medium'],bg_color=RAISED,radius=24)
        for kind,y in (('brightness',32),('timeout',148)):
            selected=add('layer',gate,0,0,480,412,name=f'{kind}_selected_{i}',bind=f'{kind}_selected_{i}_visible',bind_target='visible',hidden=True)
            rect(selected,24+i*88,y,80,72,TEXT,24)
            text(selected,f'{kind}_selected_{i}_label',named[f'button_{kind}_{i}']['text'],24+i*88,y+20,80,32,20 if kind=='timeout' else 22,'medium',BG,'center')
    for j,(name,title,sub,glyph) in enumerate((('rotate','Auto-Rotate','Follows how you hold it','rotate'),('haptics','Haptic Feedback','Touch feedback','haptic'))):
        y=236+j*88
        row(gate,'display_0_'+name,title,sub,glyph+'-32-cream',y,'toggle_'+name,'display_'+name+'_sub')
        named['row_title_display_0_'+name]['w']=284
        named['display_'+name+'_sub']['w']=284
        for on in (0,1):
            track=add('layer',gate,0,0,480,412,name=f'{name}_0_{on}',bind=f'{name}_0_{on}_visible',bind_target='visible',hidden=not on)
            rect(track,396,y+24,52,32,ORANGE if on else TILE,16)
            rect(track,419 if on else 399,y+27,26,26,TEXT,13)

    # Shared information keeps neutral list rows and a prominent add action.
    p=index['shared']
    hide('button_shared_prev')
    hide('button_shared_add')
    hide('button_shared_next')
    sh_vp=add('container',p,0,96,480,368,name='shared_viewport',bind='shared_viewport_height',scroll=True,content_height=368)
    for i in range(23):
        btn_r=button(sh_vp,'button_shared_row_'+str(i),'',16,i*80,448,72,action='shared_row')
        objects[btn_r]['events'][0]['arg']=i
        objects[btn_r].update(font_size=18,bg_color=CARD,text_align='left')
        text(btn_r,'shared_key_'+str(i),'',84,6,312,30,24,'medium',overflow='ellipsis')
        text(btn_r,'shared_entry_value_'+str(i),'',84,38,312,26,20,color=MUTED,overflow='ellipsis')
        rect(btn_r,14,10,52,52,ICON_BG,16)
        icon(btn_r,'key-38-cream',21,17,38)
        lk=add('layer',btn_r,0,0,448,72,name='shared_lock_'+str(i),bind='shared_lock_'+str(i)+'_visible',bind_target='visible',hidden=True)
        rect(lk,14,10,52,52,ICON_BG,16)
        icon(lk,'lock-38-cream',21,17,38)
        icon(btn_r,'chevron-20-dim',408,26,20)
    rect(p,408,20,56,56,RAISED,28);icon(p,'plus-32-white',420,32)
    click(p,'shared_header_add',400,12,72,72,'shared_add')
    rect(p,324,20,56,56,RAISED,28);icon(p,'help-32-white',336,32)
    click(p,'shared_help_button',310,6,84,84,'shared_help')
    named['heading_'+str(p)]['w']=224
    empty=add('layer',p,0,0,480,480,name='shared_empty',bind='shared_empty_visible',bind_target='visible',hidden=True)
    rect(empty,0,88,480,392,BG,0)
    button(empty,'shared_empty_add','Add entry',88,420,304,44,action='shared_add')
    add('image',empty,200,128,80,80,image='assets/mosaic/hero_empty-80.png')
    text(empty,'shared_empty_title','No shared info yet',24,226,432,30,22,'medium',align='center')
    text(empty,'shared_empty_line1','Save a value once, like your city,',40,264,400,26,18,color=MUTED,align='center')
    text(empty,'shared_empty_line2','and every Idea can read it.',40,286,400,26,18,color=MUTED,align='center')

    # Shared editor follows the original input card and keyboard geometry.
    p=index['shared_editor']
    text(p,'shared_editor_heading','New Key',88,29,252,40,30,'title')
    rect(p,16,96,448,64)
    text(p,'shared_value','',36,112,332,32,22,'medium')
    text(p,'shared_field','Key',32,168,416,53,22,color=MUTED)
    button(p,'button_shared_reveal','',388,96,68,64)
    named['button_shared_reveal'].update(font_size=16,font=FONT['sans'],bg_color='#00000001',text='')
    sh_eye_off=add('layer',index['button_shared_reveal'],0,0,68,64,name='shared_eye_off',bind='shared_eye_off_visible',bind_target='visible')
    icon(sh_eye_off,'eye-32-white',18,16,32)
    sh_eye_on=add('layer',index['button_shared_reveal'],0,0,68,64,name='shared_eye_on',bind='shared_eye_on_visible',bind_target='visible',hidden=True)
    icon(sh_eye_on,'eye_off-32-white',18,16,32)
    button(p,'button_shared_delete','Delete',352,28,104,40,danger=True)
    button(p,'button_shared_submit','Next',328,174,128,40,primary=True)
    hide('button_shared_submit')
    for name in ('button_shared_delete','button_shared_submit'):
        named[name].update(font_size=16,font=FONT['sans'])
    move('shared_keyboard',0,224,480,233,key_color=TILE,function_color=TILE,delete_color=TILE,space_color=TILE,ok_color=TEXT,ok_text_color=BG)

    # About: device values, no fake version or sample identity.
    p=index['about']
    for name in ('about_version','about_storage','about_mac','about_help','button_about_pairing','button_about_download'): hide(name)
    viewport=add('container',p,0,96,480,368,name='about_viewport',scroll=True,content_height=544)
    rect(viewport,16,0,448,176)
    for i,(title,value) in enumerate((('Version','about_version'),('Device','about_device'),('MAC','about_mac'),('Storage','about_storage'))):
        text(viewport,'about_key_'+str(i),title,36,6+44*i,136,30,20,color=MUTED)
        text(viewport,value,'ESP-Mosaico' if i==1 else '',176,6+44*i,268,30,20,align='right')
        if i:rect(viewport,36,44*i,408,1,LINE,0)
    row(viewport,'about_check','Update Vibe Mode','Check updates','upgrade-32-orange',188,'firmware_check','about_check')
    row(viewport,'about_licenses','Open-Source Licenses','Components used in Vibe Mode','hash-32-cream',276,'licenses')
    named['row_title_about_licenses']['font_size']=22
    row(viewport,'about_download','Find Ideas','Browse Mosaico Ideas','globe-32-cream',364,'download')

    def info_page(name,title):
        page=add('layer',-1,0,0,480,480,name=name,bind=name+'_visible',bind_target='visible',hidden=True,bg_color=BG)
        header(page,title,'button_back_'+name)
        return page
    licenses=info_page('licenses','Licenses & Notices')
    body=add('container',licenses,0,96,480,368,name='licenses_viewport',scroll=True,content_height=820)
    text(body,'licenses_heading_8','Software Components',24,8,432,34,24,'medium')
    for idx,(comp,lic) in enumerate((('ESP-IDF · ESP-Iris','Apache License 2.0'),('ESP-GSP · VUI','Espressif Modified MIT License'),('LZMA SDK','Public domain'))):
        cy=48+idx*92
        rect(body,16,cy,448,84,CARD,16)
        text(body,f'licenses_comp_{idx}',comp,32,cy+10,416,30,22,'medium')
        text(body,f'licenses_lic_{idx}',lic,32,cy+44,416,28,20,color=MUTED)
    text(body,'licenses_heading_248','Fonts',24,336,432,34,24,'medium')
    for idx,(comp,lic) in enumerate((('Inter · Archivo · Doto','SIL Open Font License 1.1'),('Noto Sans SC','SIL Open Font License 1.1'),('DejaVu','Separate font license notices')),3):
        cy=376+(idx-3)*92
        rect(body,16,cy,448,84,CARD,16)
        text(body,f'licenses_comp_{idx}',comp,32,cy+10,416,30,22,'medium')
        text(body,f'licenses_lic_{idx}',lic,32,cy+44,416,28,20,color=MUTED)
    rect(body,24,660,432,1,LINE,0)
    text(body,'licenses_body_8','Other components retain their own licenses and copyright notices.',20,676,440,60,20,color=MUTED)
    text(body,'licenses_body_248','Full terms and copyright notices are in each project\'s license files.',20,740,440,60,20,color=MUTED)
    text(body,'licenses_heading_488','',24,800,424,10,16,hidden=True)
    text(body,'licenses_body_488','',24,800,424,10,16,hidden=True)
    shared_help=info_page('shared_help','Shared Info')
    text(shared_help,'shared_help_intro','Save a value once.\nEvery Idea can read it.',24,96,432,68,22,'medium')
    text(shared_help,'shared_help_example_title','FOR EXAMPLE',24,174,432,26,18,'medium',color=MUTED)
    rect(shared_help,16,204,448,112,CARD,18,border_color=LINE,border_width=1)
    text(shared_help,'shared_help_ex0_k','city',36,218,160,30,22,'medium')
    text(shared_help,'shared_help_ex0_v','read by Weather',196,220,248,26,18,color=MUTED,align='right')
    rect(shared_help,36,260,408,1,LINE,0)
    text(shared_help,'shared_help_ex1_k','api_key',36,272,160,30,22,'medium')
    text(shared_help,'shared_help_ex1_v','read by Skills',196,274,248,26,18,color=MUTED,align='right')
    text(shared_help,'shared_help_rules','Keys: a-z, 0-9 and _ · up to 15 characters\nValues: up to 256 bytes',24,332,432,120,20,color=MUTED)
    detail=info_page('shared_detail','Shared Info')
    row(detail,'shared_detail_key','Key','','key-32-cream',96,'shared_help','shared_detail_key')
    row(detail,'shared_detail_value','Value','','id-32-cream',184,'shared_edit','shared_detail_value')
    eye_gate=add('layer',detail,0,0,480,480,name='shared_detail_eye_gate',bind='button_shared_detail_reveal_visible',bind_target='visible',hidden=True)
    rect(eye_gate,380,198,68,52,RAISED,14)
    icon(eye_gate,'eye-32-white',398,208,32)
    click(eye_gate,'button_shared_detail_reveal',372,190,84,68,'shared_detail_reveal')
    rect(detail,24,276,432,88,CARD,14,border_color=LINE,border_width=1)
    text(detail,'shared_detail_note','Tap Value to edit this entry.',36,282,408,76,20,color=MUTED)
    button(detail,'button_shared_detail_delete','Delete entry',24,380,432,84,danger=True,action='shared_delete')
    sys_ui=add('layer',detail,0,0,480,480,name='shared_detail_sys_ui',bind='shared_detail_sys_visible',bind_target='visible',hidden=True)
    rect(sys_ui,30,110,52,52,ICON_BG,16)
    icon(sys_ui,'lock-32-cream',40,120,32)
    rect(sys_ui,416,120,32,32,CARD,0)
    rect(sys_ui,24,380,432,84,ORANGE,18)
    text(sys_ui,'shared_detail_open_settings_label','Open Settings',40,404,400,36,24,'medium',BG,'center')

    # Confirm: the original centered modal and two large footer actions.
    p=index['open_confirm']
    for o in objects:
        if o.get('parent')==p: o['hidden']=True
    rect(p,330,6,24,24,ORANGE,6)
    text(p,'open_key_ai_num','2',330,5,24,24,18,'medium',BG,'center')
    text(p,'open_key_ai_arrow','↑',360,4,24,28,24,'medium',ORANGE,'center')
    rect(p,330,450,24,24,TEXT,6)
    text(p,'open_key_pwr_num','1',330,449,24,24,18,'medium',BG,'center')
    text(p,'open_key_pwr_arrow','↓',360,448,24,28,24,'medium',TEXT,'center')
    rect(p,24,34,432,410,CARD,24,border_color=LINE,border_width=1)
    text(p,'open_question','Running Leaves Vibe Mode',48,48,384,34,24,'medium')
    text(p,'open_name','',48,84,384,28,20,overflow='marquee')
    add('image',p,48,84,384,28,name='open_name_pixels',bind='open_name_pixels',bind_target='resource',dynamic_image=True,hidden=True,overflow='marquee')
    rect(p,48,118,384,1,LINE,0)
    text(p,'open_how','To come back to Vibe Mode:',48,126,384,28,20,color=MUTED)
    text(p,'open_steps','',48,158,384,28,18,hidden=True)
    for i,step_txt in enumerate(('Power off the device','Hold AI and turn it on','Release AI in Vibe Mode.'),1):
        sy=158+(i-1)*32
        rect(p,48,sy+2,24,24,'#33231E',6)
        text(p,f'open_step_num_{i}',str(i),48,sy+1,24,24,18,'medium',ORANGE,'center')
        text(p,f'open_step_text_{i}',step_txt,84,sy,348,28,20,'sans',TEXT)
    text(p,'open_warning','',48,260,384,26,18,color=ORANGE)
    move('button_toggle_open_confirm',48,294,384,34,font_size=18,font=FONT['sans'],bg_color='#00000001',text='',text_align='left')
    rect(index['button_toggle_open_confirm'],0,5,24,24,CARD,5,border_color=MUTED,border_width=2)
    chk_gate=add('layer',index['button_toggle_open_confirm'],0,0,384,34,name='open_confirm_checked',bind='open_confirm_checked_visible',bind_target='visible',hidden=True)
    rect(chk_gate,0,5,24,24,TEXT,5)
    icon(chk_gate,'check-20-black',2,7,20)
    text(index['button_toggle_open_confirm'],'open_skip_caption',"Don't show this again",36,3,348,28,20,'sans',TEXT)
    button(p,'button_open_cancel','Cancel',48,346,186,84)
    named['button_open_cancel'].update(bg_color=RAISED,radius=16,border_color=LINE,border_width=1)
    button(p,'button_open_accept','',246,346,186,84,primary=True)
    named['button_open_accept'].update(bg_color=ORANGE,fg_color=BG,radius=16,text='')
    icon(index['button_open_accept'],'play-32-black',44,26,32)
    text(index['button_open_accept'],'open_accept_caption','Run',82,25,84,34,24,'medium',BG,'left')
    click(p,'open_outside_top',0,0,480,34,'product_back')
    click(p,'open_outside_bottom',0,444,480,36,'product_back')

    # Grid uses the reference rounded-rectangle icons; only runtime Ideas populate it.
    p=index['grid'];hide('button_back_grid');hide('grid_title')
    text(p,'grid_brand','Vibe Mode',30,20,88,24,16,color=MUTED)
    text(p,'grid_version','',122,20,116,24,16,color=MUTED,overflow='ellipsis')
    text(p,'grid_title_top','',184,20,148,24,16,'sans',MUTED,'center',overflow='ellipsis')
    for i in range(6):
        x,y=38+148*(i%3),77+184*(i//3)
        move('cell_current_'+str(i),x-4,y-4,116,116,bg_color='#00000001',radius=26,border_color=ORANGE,border_width=3)
        move('button_cell_'+str(i),x-8,y-8,124,164,bg_color='#00000001',radius=26,press_ring=True)
        move('cell_default_'+str(i),x,y,108,108,radius=22)
        move('cell_icon_'+str(i),x,y,108,108,radius=22)
        text(p,'cell_name_'+str(i),'',27+148*(i%3),y+112,130,50,20,'medium',align='center')
        add('image',p,27+148*(i%3),y+125,130,24,name='cell_title_'+str(i),bind='cell_title_'+str(i),bind_target='resource',dynamic_image=True,hidden=True,overflow='marquee',text_align='center')
        badge=add('layer',p,0,0,480,480,name='cell_update_'+str(i),bind='cell_update_'+str(i)+'_visible',bind_target='visible',hidden=True)
        rect(badge,x+86,y-4,26,26,'#7FD1D7',8)
        icon(badge,'refresh-20-black',x+89,y-1,20)
        gate=add('layer',p,0,0,480,480,name='cell_add_'+str(i),bind='cell_add_'+str(i)+'_visible',bind_target='visible',hidden=True)
        rect(gate,x,y,108,108,TILE,22,border_color=LINE,border_width=1)
        rect(gate,x+34,y+51,40,6,TEXT,3);rect(gate,x+51,y+34,6,40,TEXT,3)
    hide('grid_page')
    empty=add('layer',p,0,0,480,480,name='grid_empty',bind='grid_empty_visible',bind_target='visible',hidden=True)
    rect(empty,186,92,108,108,TILE,22,border_color=LINE,border_width=1)
    rect(empty,220,143,40,6,TEXT,3);rect(empty,237,126,6,40,TEXT,3)
    text(empty,'grid_empty_add','Add Idea',152,207,176,30,22,'medium',align='center')
    text(empty,'grid_empty_title','No Ideas yet',24,262,432,42,28,'title',align='center')
    text(empty,'grid_empty_line1','Type an ID from Mosaico Ideas,',24,310,432,26,18,color=MUTED,align='center')
    text(empty,'grid_empty_line2','or ask your agent to build one.',24,332,432,26,18,color=MUTED,align='center')
    for pages in range(2,5):
        for active in range(1,pages):pager(p,f'grid_pager_{pages}_{active}',pages,active)
    for name,glyph,x,hidden_default in STATUS_ICONS:
        gate=add('layer',p,0,0,480,480,name='grid_status_'+name,bind='grid_status_'+name+'_visible',bind_target='visible',hidden=hidden_default)
        icon(gate,glyph,x,20,22)

    # Progress/results retain real transfer diagnostics in the reference hierarchy.
    for p in (80,91):
        for o in objects:
            if o.get('parent')==p and o['type']=='label' and not o.get('bind'):o['hidden']=True
    for uname,uglyph,uhid in (('app_default','code-38-cream',False),('chip','chip-38-cream',True),('storage','storage-38-cream',True)):
        ulayer=add('layer',80,0,0,480,480,name='update_icon_'+uname,bind='update_icon_'+uname+'_visible',bind_target='visible',hidden=uhid)
        rect(ulayer,32,52,96,96,ICON_BG,24,border_color=LINE,border_width=1)
        icon(ulayer,uglyph,61,81,38)
        rect(ulayer,102,122,10,10,ORANGE,5)
    add('image',80,32,52,96,96,name='update_icon_app',bind='update_icon_app',image='assets/idea-default.svg',dynamic_image=True,hidden=True,radius=20)
    text(80,'update_title','',148,50,308,56,24,'medium')
    text(80,'update_detail','',148,108,308,32,20,color=MUTED,overflow='ellipsis')
    text(80,'update_component','',24,172,432,24,16,color=ORANGE,align='center')
    text(80,'update_percent','',24,196,432,110,88,'doto',align='center')
    wait_dots=add('layer',80,0,0,480,480,name='update_wait_dots',bind='update_wait_dots_visible',bind_target='visible',hidden=True)
    for idx,dx in enumerate((202,234,266)):
        rect(wait_dots,dx,242,12,12,ORANGE if idx<2 else LINE,6)
    move('progress',40,316,400,12,bg_color=TILE,fg_color=ORANGE)
    text(80,'update_bytes','',40,340,240,28,20,color=MUTED,align='left',overflow='ellipsis')
    text(80,'update_rate','',284,340,156,28,18,color=MUTED,align='right',overflow='ellipsis')
    hide('update_owner');hide('update_verified')
    pwr_gate=add('layer',80,0,0,480,480,name='update_power_note_gate',bind='update_power_note_visible',bind_target='visible',hidden=True)
    text(pwr_gate,'update_power_note','Keep power connected until restart.',24,414,432,30,20,color=MUTED,align='center')
    button(80,'button_cancel_download','Cancel',24,400,432,64)
    add('image',91,32,76,120,120,name='result_hero',bind='result_hero_visible',bind_target='visible',image='assets/mosaic/hero_saved-120.png')
    add('image',91,32,76,120,120,name='result_failure',bind='result_failure_visible',bind_target='visible',hidden=True,image='assets/mosaic/hero_failed-120.png')
    text(91,'result_title','',176,92,280,90,28,'title')
    rect(91,32,228,416,1,LINE,0)
    bar_ok=add('layer',91,0,0,480,480,name='result_bar_ok',bind='result_bar_ok_visible',bind_target='visible')
    rect(bar_ok,32,264,4,68,'#8FD69C',2)
    bar_fail=add('layer',91,0,0,480,480,name='result_bar_fail',bind='result_bar_fail_visible',bind_target='visible',hidden=True)
    rect(bar_fail,32,264,4,68,RED,2)
    text(91,'result_detail','',52,256,396,96,22,color=MUTED)
    button(91,'button_result_library','Done',24,380,208,84)
    named['button_result_library']['events']=[dict(event='click',action='call',target_name='result_done')]
    button(91,'button_result_open','',248,380,208,84,primary=True)
    named['button_result_open'].update(bg_color=ORANGE,fg_color=BG,text='')
    icon(index['button_result_open'],'play-32-black',52,26,32)
    text(index['button_result_open'],'result_open_caption','Run',92,25,88,34,24,'medium',BG,'left')
    button(91,'button_result_done','Done',24,380,432,84,primary=True,action='result_done')
    hide('button_home')

    p=add('layer',-1,0,0,480,480,name='update_confirm',bind='update_confirm_visible',
          bind_target='visible',hidden=True,bg_color=BG)
    header(p,'Update All','button_back_update_confirm',24,'medium')
    text(p,'update_confirm_question','Update all saved Ideas?',24,132,432,64,24,'medium')
    text(p,'update_confirm_note','Download available updates to My Ideas.',24,216,432,96,22,color=MUTED)
    button(p,'button_update_all_cancel','Cancel',24,380,208,84,action='product_back')
    button(p,'button_update_all_accept','Update All',248,380,208,84,primary=True,action='update_all_accept')

    button(59,'button_format_nand','Format NAND',24,380,432,84,danger=True,action='format_nand')
    p=add('layer',-1,0,0,480,480,name='format_nand_confirm',bind='format_nand_confirm_visible',
          bind_target='visible',hidden=True,bg_color=BG)
    header(p,'NAND unavailable','button_back_format_nand',24,'medium')
    text(p,'format_nand_question','Initialize NAND Flash?',24,112,432,64,24,'medium')
    text(p,'format_nand_note','NAND initialization or reading failed. Formatting erases all files and saved Ideas on NAND.',24,188,432,144,22,color=MUTED)
    button(p,'button_format_nand_cancel','Cancel',24,380,208,84,action='product_back')
    button(p,'button_format_nand_accept','Format',248,380,208,84,danger=True,action='format_nand_accept')

    # Reset/delete preserve the accepted NAND policy and three-second hold.
    for name in ('reset','delete_confirm'):
        p=index[name]
        note='reset_note' if name=='reset' else 'delete_note'
        move(note,24,112,432,156,font_size=22,font=FONT['sans'],fg_color=MUTED)
        if name=='reset':
            hide(note)
            text(p,'reset_body','Clears settings, then restarts:',24,94,432,26,20,'sans',TEXT)
            rect(p,16,124,448,172,CARD,18,border_color=LINE,border_width=1)
            for r_idx,(r_key,r_val) in enumerate((('Ideas','Kept on this device'),('Wi-Fi','Not connected'),('Shared Info','0 entries'),('Settings','Display & haptics, language'))):
                ry=124+r_idx*43
                if r_idx: rect(p,36,ry,408,1,LINE,0)
                kw = 112 if r_idx == 3 else 144
                vx, vw = (152, 292) if r_idx == 3 else (184, 260)
                text(p,f'reset_key_{r_idx}',r_key,36,ry+8,kw,28,20,color=MUTED)
                text(p,f'reset_val_{r_idx}',r_val,vx,ry+8,vw,28,20,color=TEXT,align='right',overflow='ellipsis')
        else:
            hide('delete_progress')
        progress_name='reset_hold_progress' if name=='reset' else 'delete_hold_progress'
        add('progress',p,24,348,432,96,name=progress_name,bind=progress_name,
            bind_target='value',bg_color=RAISED,fg_color=ORANGE,radius=20)
        button(p,'button_reset_hold' if name=='reset' else 'button_delete_hold','Hold to reset' if name=='reset' else 'Hold to delete',24,348,432,96,danger=True)
        named['button_reset_hold' if name=='reset' else 'button_delete_hold'].update(bg_color='#00000001',fg_color=TEXT)

    reset_status=add('container',index['reset'],0,80,480,400,name='reset_status_panel',
        bind='reset_status_panel',bind_target='visible',hidden=True,bg_color=BG)
    text(reset_status,'reset_status','Restoring factory settings...',24,112,432,112,24,
        color=TEXT,align='center')

    # Display only: keep the product's existing timeout, brightness and wake policy.
    p=index['sleep']
    for obj in objects:
        if obj.get('parent')==p: obj['hidden']=True
    add('matrix',p,154,122,172,172,name='sleep_matrix')
    text(p,'sleep_message','Tap or press AI to wake',24,324,432,34,24,align='center')

    for o in objects:
        if o.get('parent') == -1 and o.get('type') == 'layer': o['bg_color']=BG

    wait_modal=add('layer',-1,0,0,480,480,name='wait_modal',bind='wait_modal_visible',bind_target='visible',hidden=True)
    click(wait_modal,'wait_modal_backdrop',0,0,480,480,'wait_modal_close')
    rect(wait_modal,24,116,432,248,CARD,24,border_color=LINE,border_width=1)
    text(wait_modal,'wait_modal_title','Please wait',48,148,384,34,24,'medium',TEXT,'center')
    text(wait_modal,'wait_modal_body','Checking apps and storage...',48,196,384,32,20,'sans',MUTED,'center')
    button(wait_modal,'button_wait_modal_cancel','Cancel',48,256,384,84,action='wait_modal_close')
    named['button_wait_modal_cancel'].update(bg_color=RAISED,radius=16,border_color=LINE,border_width=1)

    order[0]=999998
    ai_fb=add('layer',-1,0,0,480,56,name='ai_feedback',bind='ai_feedback_visible',bind_target='visible',hidden=True)
    rect(ai_fb,340,4,64,8,ORANGE,4)
    rect(ai_fb,328,20,88,32,RAISED,16,border_color=LINE,border_width=1)
    text(ai_fb,'ai_feedback_chip','Back',328,25,88,22,16,'medium',TEXT,'center')

    # Unify legacy detail labels with the reviewed title/body styles.
    for o in objects:
        if o.get('type')=='button' and o.get('font_size')==22 and not o.get('name','').startswith(('button_brightness_','button_timeout_')):
            o['font_size']=24

    # The source retains invisible compatibility bindings; don't spend glyph
    # atlases on old labels that can never be shown.
    for o in objects:
        if o.get('hidden') and 'text' in o and not o.get('bind'):
            o['text']=''
    # Reused bound labels must draw after their new card backgrounds. Remap
    # parent indices once, after authoring; runtime binding names stay stable.
    ordered = sorted(enumerate(objects), key=lambda pair: pair[1].get('_paint_order', pair[0]))
    remap = {old: new for new, (old, _) in enumerate(ordered)}
    for _, obj in ordered:
        parent = obj.get('parent', -1)
        if parent >= 0: obj['parent'] = remap[parent]
        obj.pop('_paint_order', None)
    objects[:] = [obj for _, obj in ordered]
    return scene


def runtime_charsets(root, translations):
    """Project known UI strings onto their binding's atlas, not every font size."""
    import re
    values = {}
    sources = '\n'.join(p.read_text() for p in (root/'components/app_ui/src').glob('vibe_*.c'))
    for match in re.finditer(r'(?:TEXT|FORMAT)\(s,\s*(\w+)\s*,(.*?);', sources, re.S):
        target = values.setdefault(match[1].lower(), set())
        for quoted in re.findall(r'"(?:[^"\\]|\\.)*"', match[2]):
            try: target.add(json.loads(quoted))
            except ValueError: pass
    # Job titles and details are supplied by services rather than the UI macros.
    titles = set()
    details = set()
    service_sources = '\n'.join(p.read_text() for p in (root/'components/app_ideas/src').glob('*.c'))
    for match in re.finditer(r'ideas_message\(\w+,\s*("(?:[^"\\]|\\.)*")\s*,\s*("(?:[^"\\]|\\.)*")', service_sources):
        titles.add(json.loads(match[1])); details.add(json.loads(match[2]))
    for key in translations:
        if any(word in key.lower() for word in ('updating','update complete','update failed','failed','verified','cancelled','battery')) and len(key)<40:
            titles.add(key)
        if any(word in key.lower() for word in ('verif','connect','transferred','saving','restarting','committing','installing','unchanged','retained','saved in','selecting','charge above')):
            details.add(key)
    # Follow backend title/detail producers, including stage variables. Keyword
    # guesses alone missed Checking image / Saving Idea and their Chinese text.
    backend = service_sources + (root/'components/app_ui/src/ui_services.c').read_text()
    def literals(statement):
        return [json.loads(value) for value in re.findall(r'"(?:[^"\\]|\\.)*"', statement)
                if value != '"%s"']
    for field, target in (('title', titles), ('detail', details)):
        patterns = (rf'COPY\(update_{field},[^;]+;',
                    rf'snprintf\([^;]*?(?:->|\.){field}\s*,[^;]+;',
                    rf'\b{field}\s*=\s*[^;]+;')
        for pattern in patterns:
            for statement in re.findall(pattern, backend, re.S):
                target.update(literals(statement))
    for name in ('update_title','result_title'): values.setdefault(name,set()).update(titles)
    details.update(('Reading and verifying files', 'Checking installed application',
                    'Preparing application', 'Writing application'))
    for name in ('update_detail','result_detail'): values.setdefault(name,set()).update(details)
    # The verb is formatted through a variable, outside TEXT/FORMAT literals.
    values.setdefault('update_rate', set()).update(('Download', 'Read', 'Receive', 'Write'))
    values.setdefault('firmware_progress_title', set()).update(('Updating firmware', 'Firmware update failed'))
    values.setdefault('firmware_phase', set()).update(('Downloading firmware', 'Verifying firmware', 'Writing firmware', 'Restarting Vibe Mode'))
    for i in range(6):values.setdefault('cell_name_'+str(i),set()).add('Add Idea')
    values.setdefault('bundles_detail',set()).update(('Current','Update available','Updating · %u%%','Waiting'))
    values.setdefault('networks_detail',set()).update(('Open network','Secured'))
    # Direct text projection in project_shared is outside TEXT/FORMAT macros.
    values.setdefault('shared_value',set()).add('Enter text')
    values.setdefault('shared_field',set()).update(('Saving settings...','Please use a-z, 0-9 and _','Key already exists'))
    values.setdefault('shared_submit_label',set()).update(('Next','Save'))
    values.setdefault('shared_reveal_label',set()).update(('Show','Hide'))
    values.setdefault('button_shared_detail_delete',set()).update(('Delete entry','Open Settings'))
    values.setdefault('help_heading',set()).update(('Help','Getting Started','Develop Ideas','Idea IDs & Sharing','Troubleshooting','Return to Vibe Mode','Wi-Fi Connection','Storage Is Full'))
    for step_idx, step_strs in (
        (1, ('Plug in USB and ask your agent to build an Idea.','Open the Vibe-coding guide and\nfollow it in your AI agent.','Every Idea has a 6-digit ID shown\non Mosaico Ideas.','Power off the device.')),
        (2, ('Or download one: open Manage, tap +, type its ID.','Plug in USB. The agent installs the\nIdea over the cable.','Open Manage, tap +, type the ID\nand press Go.','Hold AI and turn the device on.')),
        (3, ('Power off. Hold AI and power on.\nRelease AI in Vibe Mode.','Check Develop for USB and agent status.','Share your own Idea to get an ID\nfor friends.','Release AI when Vibe Mode appears.')),
    ):
        values.setdefault(f'help_step_body_{step_idx}',set()).update(step_strs)
    for step_idx, step_strs in enumerate([('Open Wi-Fi Settings and check\nthat Wi-Fi is on.', 'Open Manage Storage to check\nyour Ideas and storage usage.'), ('Select your network and enter\nthe correct password.', 'Delete an Idea you no longer need\nto free space.'), ('If connection fails, check the\npassword and scan again.', 'The current Idea cannot be deleted.\nRun another Idea first.')], 1):
        values.setdefault(f'help_step_body_{step_idx}',set()).update(step_strs)
    values.setdefault('guide_heading',set()).update(('View on your phone','Vibe Coding Guide','Getting Started','Idea IDs & Sharing','Troubleshooting'))
    values.setdefault('guide_one',set()).update(('Scan for the coding guide.','Build your first Idea with AI.','Set up and run your first Idea.','Find Idea IDs and share your own.','Help with Wi-Fi, storage and Ideas.'))
    values.setdefault('guide_url',set()).update(('mosaico.espressif.com/guide/vibe-coding','mosaico.espressif.com/vibe/start','mosaico.espressif.com/vibe/ids','mosaico.espressif.com/vibe/fix'))
    for i in range(4):values.setdefault('shared_entry_value_'+str(i),set()).add('Undo · %u s')
    values.setdefault('ai_feedback_chip',set()).update(('Sleep','Home','Back','Close','Wake'))
    values.setdefault('heading_49',set()).add('前往')
    return {name: ''.join(sorted(''.join(strings)+''.join(translations.get(s,'') for s in strings))) for name,strings in values.items()}
