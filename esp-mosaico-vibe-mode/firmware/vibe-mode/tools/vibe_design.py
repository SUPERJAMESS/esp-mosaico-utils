"""Apply the EMVM-73078 reference design to the production controller's widgets.

Coordinates, typography, icons and colors follow the supplied Vibe Mode 0.3
mockups. Service bindings stay in the product; no reference sample apps ship.
"""
from pathlib import Path
import json

BG, CARD, RAISED, TILE = '#000000', '#101010', '#212021', '#292829'
TEXT, MUTED, ORANGE, RED = '#F2F2F2', '#9A9A9A', '#FF4C01', '#FF5A5F'
FONT = {'sans': 'fonts/VibeInter.otf', 'medium': 'fonts/VibeMedium.otf', 'title': 'fonts/VibeDisplay.otf'}


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

    def header(parent,title,back):
        for o in objects:
            if o.get('parent')==parent and (o.get('text')=='‹' or o.get('name','').endswith('_title') or (o['type']=='label' and o.get('y',999)<=40)):
                o['hidden']=True
        rect(parent,16,20,56,56,RAISED,28)
        icon(parent,'back-32-white',28,32)
        action = named.get(back,{}).get('events',[{}])[0].get('target_name','product_back')
        click(parent,back,2,6,84,84,action)
        text(parent,'heading_'+str(parent),title,88,30,368,37,28,'title')

    def row(parent, name, title, subtitle, glyph, y, action, subtitle_name=None, danger=False):
        rect(parent,16,y,448,80)
        rect(parent,28,y+12,56,56,RAISED,16)
        icon(parent,glyph,40,y+24)
        text(parent,'row_title_'+name,title,100,y+(12 if subtitle is not None else 24),312,30,22,'medium',RED if danger else TEXT)
        if subtitle is not None:
            text(parent,subtitle_name or 'row_sub_'+name,subtitle,100,y+44,312,24,16,color=MUTED)
        icon(parent,'chevron-20-dim',424,y+30,20)
        click(parent,'button_'+name,16,y,448,80,action)

    # Defaults are the design's grayscale palette and original font hierarchy.
    for o in objects:
        if 'text' in o:
            o['font']=FONT['sans']
            o['fg_color']=MUTED if o.get('fg_color') in ('#91919B','#999999') else TEXT
        if o.get('bg_color') in ('#181818','#242426'): o['bg_color']=CARD
    # Root/home. Old diagnostic widgets remain hidden for controller compatibility.
    for o in objects:
        if o.get('parent')==0: o['hidden']=True
    text(0,'home_brand','Vibe Mode',44,20,84,24,16,color=MUTED)
    text(0,'home_version','',132,20,60,24,16,color=MUTED,overflow='ellipsis')
    text(0,'home_page','Home',192,20,96,24,18,'medium',MUTED, 'center')
    text(0,'home_status','',324,20,112,24,16,color=MUTED,align='right')
    add('image',0,180,60,120,120,name='home_hero',image='assets/anim/home.dots.json')
    en=add('layer',0,0,0,480,480,name='home_heading_en',bind='home_heading_en_visible',bind_target='visible')
    text(en,'home_headline','Make Ideas',108,182,199,37,28,'title')
    text(en,'home_run','Run',307,182,85,37,28,'title',ORANGE)
    zh=add('layer',0,0,0,480,480,name='home_heading_zh',bind='home_heading_zh_visible',bind_target='visible',hidden=True)
    text(zh,'home_headline_zh','让 Idea',148,182,100,37,28,'title')
    text(zh,'home_run_zh','跑起来',248,182,86,37,28,'title',ORANGE)
    for name in ('home_headline_zh','home_run_zh'):
        named[name]['font']='fonts/NotoSansSC-Bold.otf'
    text(0,'home_caption','Download or build Ideas,',40,222,400,26,18,color=MUTED,align='center')
    text(0,'home_caption_second','then run them here.',40,244,400,26,18,color=MUTED,align='center')
    for i,(name,title,sub,glyph) in enumerate((('develop','Develop','Build one','code'),('manage','Manage','Download · update','grid'),('settings','Settings','Device','settings'))):
        x=16+152*i
        rect(0,x,300,144,120)
        icon(0,glyph+'-32-white',x+56,316)
        text(0,'nav_'+name+'_label',title,x+8,358,128,25,18,'medium',align='center')
        text(0,'nav_'+name+'_sub',sub,x,384,144,24,16,color=MUTED,align='center',overflow='ellipsis')
        click(0,'button_nav_'+name,x,300,144,120,'nav_'+name)
    def pager(parent, name, pages, active):
        gate=add('layer',parent,0,0,480,480,name=name,bind=name+'_visible',bind_target='visible',hidden=True)
        x=(480-(pages*8+12+(pages-1)*8))//2
        for i in range(pages):
            width=20 if i==active else 8
            rect(gate,x,440,width,8,ORANGE if i==active else TILE,4)
            x+=width+8
    # VIBE_BUNDLE_MAX=16: one Home page plus up to three six-cell Idea pages.
    # The Add Idea tile counts as a cell, including in the empty library.
    for pages in range(2,5):pager(0,'home_pager_'+str(pages),pages,0)

    # Live status icons retain the reference's compact status bar.
    hide('home_status')
    for name,glyph,x in (('wifi','wifi-20-white',348),('usb','usb-20-white',380),('battery','battery_full-20-white',412),('battery_mid','battery_mid-20-white',412),('battery_low','battery_low-20-orange',412),('battery_charging','battery_charging-20-white',412)):
        gate=add('layer',0,0,0,480,480,name='status_'+name,bind='status_'+name+'_visible',bind_target='visible')
        icon(gate,glyph,x,22,20)

    # Shared header geometry, with existing controller actions.
    for p,title,back in ((15,'Wi-Fi','button_back_wifi'),(31,'Join network','button_back_password'),
                        (39,'TCP pairing','button_back_pairing'),(49,'Download Idea','button_back_bridge'),
                        (59,'Manage','button_back_nand'),(69,'Idea details','button_back_confirm')):
        for o in objects:
            if o.get('parent')==p and o['type']=='label' and not o.get('bind'): o['hidden']=True
        header(p,title,back)
    for name,title in (('develop','Develop'),('settings','Settings'),('display','Display & haptics'),('language','Language'),
                       ('shared','Shared info'),('shared_editor','Shared info'),('about','About'),('reset','Factory reset'),
                       ('delete_confirm','Delete Idea'),('boot_tip','Update Vibe Mode')):
        header(index[name],title,'button_back_'+name)

    # Help and guide are real destinations; QR points to the supplied guide.
    help_page=add('layer',-1,0,0,480,480,name='help',bind='help_visible',bind_target='visible',hidden=True)
    header(help_page,'Help','button_back_help')
    for i,(title,sub,glyph,action) in enumerate((('Getting started','Set up and run your first Idea','flag','show_guide'),
            ('Vibe-code an Idea','Build with your Agent','code','show_guide'),
            ('Find Ideas','Download from Mosaico Ideas','globe','download'),
            ('Update Vibe Mode','Install a new system version','upgrade','show_boot_tip'))):
        row(help_page,'help_'+str(i),title,sub,glyph+'-32-cream' if glyph!='upgrade' else 'upgrade-32-orange',96+88*i,action)
    guide=add('layer',-1,0,0,480,480,name='guide',bind='guide_visible',bind_target='visible',hidden=True)
    header(guide,'View on your phone','button_back_guide')

    # Develop opens the same guide directly, with live USB status at the bottom.
    for p, prefix in ((guide, 'guide'), (index['develop'], 'develop')):
        rect(p,24,112,216,216,TEXT,16)
        add('image',p,33,121,198,198,image='assets/qr/develop@198.png')
        text(p,prefix+'_one','Step-by-step guide',260,142,204,30,18,'medium')
        text(p,prefix+'_two','Build your own Idea',260,182,204,26,16,color=MUTED)
        text(p,prefix+'_three','with your Agent.',260,208,204,26,16,color=MUTED)
        rect(p,24,352,432,56,CARD,16)
        text(p,prefix+'_url','mosaico.espressif.com/guide/vibe-coding',36,368,408,24,16,color=MUTED)
    p=index['develop']
    icon(p,'usb-20-white',32,434,20)
    text(p,'develop_usb','',64,430,392,28,16,color=MUTED)

    # One clipped viewport: drawing and hit testing share the scroll offset.
    p=index['settings']
    for o in objects:
        if o.get('parent')==p and o.get('name','').startswith('button_nav_'): o['hidden']=True
    viewport=add('container',p,0,96,480,352,name='settings_viewport',scroll=True,content_height=616)
    rows=[('wifi','Wi-Fi','','wifi'),('display','Display & haptics','Screen off after 3 min','display'),
          ('language','Language','English','globe'),('shared','Shared info','','key'),
          ('help','Help','Get started with Vibe Mode','help'),('about','About','Version · device · storage','chip'),
          ('reset','Factory reset','Clear preferences · keep NAND','replay')]
    for i,(name,title,sub,glyph) in enumerate(rows):
        group='settings_top' if i<4 else 'settings_bottom'
        row(viewport,group+'_'+name,title,sub,glyph+'-32-cream',88*i,'nav_'+name,
            group+'_'+name+'_sub',danger=name=='reset')

    # Manage: icon actions, capacity, 80px cards with 8px gaps.
    text(59,'nand_scan','',24,94,432,26,18,color=TEXT)
    hide('button_nand_scan');hide('button_manage_add')
    for x,glyph,action in ((324,'upgrade-32-white','update_all'),(408,'plus-32-white','download')):
        rect(59,x,20,56,56,RAISED,28);icon(59,glyph,x+12,32)
        click(59,'manage_'+action,x-8,12,72,72,action)
    badge=add('layer',59,0,0,480,480,name='manage_update_badge',bind='manage_update_badge_visible',bind_target='visible',hidden=True)
    rect(badge,360,16,24,24,ORANGE,12)
    text(badge,'manage_update_count','',360,17,24,23,16,color=BG,align='center')
    move('bundles',16,148,448,316,item_height=88,hidden=True)
    objects[65].update(w=448,h=88,bg_color=BG,radius=0)
    # The row background must precede text in the template's child ordering.
    rect(65,0,0,448,80,CARD,20)
    text(65,'bundles_title','',84,12,312,30,22,'medium')
    add('image',65,84,12,312,30,name='bundles_pixels',bind='bundles_pixels',bind_target='resource',dynamic_image=True)
    text(65,'bundles_detail','',84,44,312,24,16,color=MUTED,dynamic_color=True)
    move('bundles_icon',12,12,56,56)
    objects[68].update(x=408,y=25,w=24,h=30,font_size=22);front(objects[68])
    add('progress',59,24,128,432,6,name='storage_bar',bind='storage_bar',min=0,max=100,value=0,bg_color=TILE,fg_color=TEXT,radius=3)
    empty=add('layer',59,0,0,480,480,name='library_empty',bind='library_empty_visible',bind_target='visible',hidden=True)
    add('image',empty,200,190,80,80,image='assets/mosaic/hero_empty-80.png')
    text(empty,'library_empty_title','No Ideas yet',24,290,432,30,22,'medium',align='center')
    text(empty,'library_empty_note','Tap + to download your first Idea.',24,334,432,26,18,color=MUTED,align='center')
    move('undo_message',40,405,280,48,font_size=16)
    button(59,'button_undo_delete','Undo',332,405,108,48)

    # Download keypad: exact reference spacing and separate digit cells.
    for o in objects:
        if o.get('parent')==49 and (o['type']=='container' or o.get('name') in ('bridge_code','bridge_code_compact')): o['hidden']=True
    for i in range(6):
        x=82+52*i+(12 if i>=3 else 0)
        rect(49,x,92,44,56,CARD,8)
        text(49,'code_digit_'+str(i),'',x,102,44,37,28,'title',align='center')
    text(49,'bridge_detail','Enter the 6-digit ID from Mosaico Ideas',24,154,432,24,16,color=MUTED,align='center')
    for n in range(1,10):
        x,y=92+104*((n-1)%3),188+72*((n-1)//3)
        button(49,'button_code_'+str(n),str(n),x,y,88,56)
        named['button_code_'+str(n)].update(font=FONT['title'],font_size=28,bg_color=TILE,radius=16)
    button(49,'button_code_0','0',196,404,88,56);named['button_code_0'].update(font=FONT['title'],font_size=28,bg_color=TILE,radius=16)
    click(49,'button_code_delete',84,396,104,72,'code_delete');icon(49,'backspace-32-white',120,416)
    button(49,'button_download_code','Go',300,404,88,56,primary=True);named['button_download_code']['font_size']=22

    # Wi-Fi and password use the same row typography and native keyboard.
    for i in (18,): objects[i]['hidden']=True
    text(15,'wifi_connection','Not connected',24,100,286,30,22,'medium')
    text(15,'wifi_detail','Choose a network below',24,135,330,24,16,color=MUTED)
    button(15,'button_forget','Forget',348,102,108,48,danger=True);named['button_forget'].update(font_size=16,font=FONT['sans'])
    text(15,'wifi_scan','Available networks',24,178,300,24,16,color=MUTED)
    button(15,'button_scan','Rescan',352,171,104,36);named['button_scan'].update(font_size=16,font=FONT['sans'])
    move('networks',16,216,448,232,item_height=80)
    objects[25].update(w=448,h=80);objects[26].update(w=448,h=72,bg_color=CARD,radius=20)
    text(25,'networks_title','',84,10,316,30,22,'medium')
    text(25,'networks_detail','',84,40,316,24,16,color=MUTED)
    icon(25,'wifi-32-cream',24,22)
    text(15,'wifi_note','',24,451,432,24,16,color=MUTED)
    text(31,'ssid','',88,30,368,37,28,'title');hide('heading_31')
    rect(31,16,96,448,64)
    text(31,'password_display','Enter password',36,113,332,30,22,'medium')
    move('button_toggle_password',392,96,72,64,bg_color='#00000001')
    text(index['button_toggle_password'],'password_toggle','Show',0,18,72,26,16,align='center')
    text(31,'password_error','',32,176,416,24,16,color=RED)
    move('password_keyboard',0,224,480,233,key_color=TILE,function_color=TILE,delete_color=TILE,space_color=TILE,ok_color=TEXT,ok_text_color=BG)

    for kb in ('password_keyboard','shared_keyboard'):
        named[kb].update(font=FONT['medium'],font_size=22,function_font_size=18,symbols_label='123',space_label='_')

    # Idea details use the original open/delete footer and a compact metadata block.
    for o in objects:
        if o.get('parent')==69 and o['type']=='container':o['hidden']=True
    hide('heading_69');hide('button_cancel_install')
    text(69,'release','',88,30,368,37,28,'title')
    add('image',69,88,30,368,37,name='release_pixels',bind='release_pixels',bind_target='resource',dynamic_image=True,hidden=True)
    add('image',69,24,96,96,96,image='assets/idea-default.svg')
    text(69,'local_status','On this device',136,100,320,30,22,'medium')
    text(69,'release_detail','',136,140,320,26,18,color=MUTED)
    text(69,'release_path','',24,208,432,60,18,color=MUTED)
    button(69,'button_check_idea','Check updates',24,280,208,72)
    button(69,'button_preview_idea','View info',248,280,208,72)
    button(69,'button_start_idea','Delete',24,380,208,84,danger=True)
    button(69,'button_install','Open',248,380,208,84,primary=True)
    # The update check opens the real remote preview with the download action.
    hide('button_update_idea')
    header(117,'Idea details','button_back_remote')
    click(117,'button_back_remote',2,6,84,84,'cancel_idea')
    hide('idea_heading')
    move('idea_icon',24,96,96,96)
    text(117,'idea_name','Loading app information...',136,96,328,84,22,'medium')
    move('idea_title_pixels',136,96,328,84,hidden=True)
    text(117,'idea_summary','',24,178,432,24,16,color=MUTED)
    text(117,'idea_description','Fetching details from the cloud',32,202,416,176,18)
    move('idea_body_pixels',32,202,416,176,hidden=True)
    hide('idea_status')
    button(117,'button_cancel_idea','Cancel',24,380,208,84)
    button(117,'button_accept_idea','Download',248,380,208,84,primary=True)

    # Language selection matches the side-by-side reference cards.
    for name in ('language','welcome'):
        p=index[name]
        hide(name+'_note')
        y=104 if name=='language' else 240
        button(p,'button_'+name+'_en','English',24,y,208,84)
        button(p,'button_'+name+'_zh','简体中文',248,y,208,84)
        for i,language in enumerate(('en','zh')):
            selected=add('layer',p,0,0,480,480,name=f'{name}_{language}_selected',bind=f'{name}_{language}_selected_visible',bind_target='visible',hidden=True)
            rect(selected,24+i*224,y,208,84,TEXT,20)
            text(selected,f'{name}_{language}_selected_label','English' if i==0 else '简体中文',24+i*224,y+27,208,30,22,'medium',BG,'center')
        if name=='welcome':
            hide('welcome_title')
            add('image',p,200,64,80,80,image='assets/mosaic/hero_empty-80.png')
            text(p,'welcome_heading','Choose your language',24,168,432,37,28,'title',align='center')
            text(p,'welcome_chinese','选择语言',24,208,432,26,18,color=MUTED,align='center')

    # Display: reference segmented controls and toggle rows, scrolling viewport.
    p=index['display']
    for o in objects:
        if o.get('parent')==p and o.get('name') not in ('heading_'+str(p),'button_back_display') and o.get('name'):
            o['hidden']=True
    gate=add('container',p,0,92,480,356,name='display_viewport',scroll=True,content_height=412)
    text(gate,'brightness_title','BRIGHTNESS',32,0,424,28,18,'medium',color=MUTED)
    text(gate,'timeout_title','SCREEN OFF AFTER',32,116,424,28,18,'medium',color=MUTED)
    for i in range(5):
        move('button_brightness_'+str(i),24+i*88,32,80,72,parent=gate,font_size=18,font=FONT['medium'],bg_color=RAISED,radius=24)
        move('button_timeout_'+str(i),24+i*88,148,80,72,parent=gate,font_size=18,font=FONT['medium'],bg_color=RAISED,radius=24)
        for kind,y in (('brightness',32),('timeout',148)):
            selected=add('layer',gate,0,0,480,412,name=f'{kind}_selected_{i}',bind=f'{kind}_selected_{i}_visible',bind_target='visible',hidden=True)
            rect(selected,24+i*88,y,80,72,TEXT,24)
            text(selected,f'{kind}_selected_{i}_label',named[f'button_{kind}_{i}']['text'],24+i*88,y+23,80,26,18,'medium',BG,'center')
    for j,(name,title,sub,glyph) in enumerate((('rotate','Auto rotate','Follows how you hold it','rotate'),('haptics','Haptics','Ticks when you press keys','haptic'))):
        y=236+j*88
        row(gate,'display_0_'+name,title,sub,glyph+'-32-cream',y,'toggle_'+name)
        for on in (0,1):
            track=add('layer',gate,0,0,480,412,name=f'{name}_0_{on}',bind=f'{name}_0_{on}_visible',bind_target='visible',hidden=not on)
            rect(track,396,y+24,52,32,ORANGE if on else TILE,16)
            rect(track,419 if on else 399,y+27,26,26,TEXT,13)

    # Shared information keeps neutral list rows and a prominent add action.
    p=index['shared']
    for i in range(4):
        button(p,'button_shared_row_'+str(i),'',16,96+i*80,448,72)
        named['button_shared_row_'+str(i)].update(font_size=18,bg_color=CARD,text_align='left')
    button(p,'button_shared_prev','‹',16,420,64,44)
    button(p,'button_shared_add','Add entry',88,420,304,44)
    button(p,'button_shared_next','›',400,420,64,44)
    rect(p,408,20,56,56,RAISED,28);icon(p,'plus-32-white',420,32)
    click(p,'shared_header_add',400,12,72,72,'shared_add')
    empty=add('layer',p,0,0,480,410,name='shared_empty',bind='shared_empty_visible',bind_target='visible',hidden=True)
    rect(empty,0,88,480,392,BG,0)
    button(empty,'shared_empty_add','Add entry',24,380,432,84,action='shared_add')
    add('image',empty,200,128,80,80,image='assets/mosaic/hero_empty-80.png')
    text(empty,'shared_empty_title','No shared info yet',24,226,432,30,22,'medium',align='center')
    text(empty,'shared_empty_line1','Save a value once, like your city,',40,264,400,26,18,color=MUTED,align='center')
    text(empty,'shared_empty_line2','and every Idea can read it.',40,286,400,26,18,color=MUTED,align='center')

    # Shared editor follows the original input card and keyboard geometry.
    p=index['shared_editor']
    hide('heading_'+str(p))
    text(p,'shared_editor_heading','New key',88,30,368,37,28,'title')
    rect(p,16,96,448,64)
    text(p,'shared_value','',36,112,332,32,22,'medium')
    text(p,'shared_field','Key',32,174,424,40,16,color=MUTED)
    button(p,'button_shared_reveal','Show',388,108,64,40)
    named['button_shared_reveal'].update(font_size=16,font=FONT['sans'],bg_color=CARD)
    button(p,'button_shared_delete','Delete',352,28,104,40,danger=True)
    button(p,'button_shared_submit','Next',328,174,128,40,primary=True)
    hide('button_shared_submit')
    for name in ('button_shared_delete','button_shared_submit'):
        named[name].update(font_size=16,font=FONT['sans'])
    move('shared_keyboard',0,224,480,233,key_color=TILE,function_color=TILE,delete_color=TILE,space_color=TILE,ok_color=TEXT,ok_text_color=BG)

    # About: device values, no fake version or sample identity.
    p=index['about']
    for name in ('about_version','about_storage','about_mac','about_help','button_about_pairing','button_about_download'): hide(name)
    rect(p,16,96,448,176)
    for i,(title,value) in enumerate((('Version','about_version'),('Device','about_device'),('MAC','about_mac'),('Storage','about_storage'))):
        text(p,'about_key_'+str(i),title,36,105+44*i,160,26,18,color=MUTED)
        text(p,value,'ESP-Mosaico' if i==1 else '',196,105+44*i,248,26,18,align='right')
        if i:rect(p,36,96+44*i,408,1,TILE,0)
    row(p,'about_pairing','TCP pairing','Connect your development tools','usb-32-cream',284,'pairing')
    row(p,'about_download','Find Ideas','Browse Mosaico Ideas','globe-32-cream',372,'download')

    # Confirm: the original centered modal and two large footer actions.
    p=index['open_confirm']
    for o in objects:
        if o.get('parent')==p: o['hidden']=True
    rect(p,32,56,416,368,RAISED,24)
    text(p,'open_name','',56,80,368,37,28,'title')
    rect(p,56,150,368,1,TILE,0)
    text(p,'open_question','Leave Vibe Mode and open this Idea?',56,170,368,56,18)
    text(p,'open_warning','',56,234,368,70,18,color=ORANGE)
    move('button_toggle_open_confirm',56,280,368,32,font_size=16,font=FONT['sans'],bg_color='#00000001',text_align='left')
    button(p,'button_open_cancel','Cancel',48,336,184,72)
    button(p,'button_open_accept','Open',248,336,184,72,primary=True)

    # Grid uses the reference circular icons; only runtime Ideas populate it.
    p=index['grid'];hide('button_back_grid');hide('grid_title')
    text(p,'grid_brand','Vibe Mode',44,20,84,24,16,color=MUTED)
    text(p,'grid_version','',132,20,60,24,16,color=MUTED,overflow='ellipsis')
    text(p,'grid_title_top','',192,20,96,24,18,'sans',MUTED,'center')
    for i in range(6):
        x,y=34+152*(i%3),72+184*(i//3)
        move('cell_current_'+str(i),x-4,y-4,116,116,bg_color='#00000001',radius=58,border_color=ORANGE,border_width=3)
        move('button_cell_'+str(i),x-8,y-8,124,164,bg_color='#00000001',radius=54)
        move('cell_default_'+str(i),x,y,108,108)
        move('cell_icon_'+str(i),x,y,108,108)
        text(p,'cell_name_'+str(i),'',16+152*(i%3),y+114,144,40,18,'medium',align='center')
        gate=add('layer',p,0,0,480,480,name='cell_add_'+str(i),bind='cell_add_'+str(i)+'_visible',bind_target='visible',hidden=True)
        rect(gate,x,y,108,108,TILE,54)
        rect(gate,x+34,y+51,40,6,TEXT,3);rect(gate,x+51,y+34,6,40,TEXT,3)
    hide('grid_page')
    empty=add('layer',p,0,0,480,480,name='grid_empty',bind='grid_empty_visible',bind_target='visible',hidden=True)
    rect(empty,186,92,108,108,TILE,54)
    rect(empty,220,143,40,6,TEXT,3);rect(empty,237,126,6,40,TEXT,3)
    text(empty,'grid_empty_add','Add Idea',152,207,176,30,22,'medium',align='center')
    text(empty,'grid_empty_title','No Ideas yet',24,262,432,42,28,'title',align='center')
    text(empty,'grid_empty_line1','Type an ID from Mosaico Ideas,',24,310,432,26,18,color=MUTED,align='center')
    text(empty,'grid_empty_line2','or ask your agent to build one.',24,332,432,26,18,color=MUTED,align='center')
    for pages in range(2,5):
        for active in range(1,pages):pager(p,f'grid_pager_{pages}_{active}',pages,active)
    for name,glyph,x in (('wifi','wifi-20-white',348),('usb','usb-20-white',380),('battery','battery_full-20-white',412),('battery_mid','battery_mid-20-white',412),('battery_low','battery_low-20-orange',412),('battery_charging','battery_charging-20-white',412)):
        gate=add('layer',p,0,0,480,480,name='grid_status_'+name,bind='grid_status_'+name+'_visible',bind_target='visible')
        icon(gate,glyph,x,22,20)

    # Progress/results retain real transfer diagnostics in the reference hierarchy.
    for p in (80,91):
        for o in objects:
            if o.get('parent')==p and o['type']=='label' and not o.get('bind'):o['hidden']=True
    text(80,'update_title','',24,56,432,42,28,'title',align='center')
    text(80,'update_detail','',24,112,432,54,18,color=MUTED,align='center')
    text(80,'update_percent','',24,184,432,90,64,'title',align='center')
    move('progress',24,292,432,8,bg_color=TILE,fg_color=ORANGE)
    text(80,'update_bytes','',24,314,432,26,18,align='center')
    text(80,'update_rate','',24,342,432,24,16,color=MUTED,align='center')
    text(80,'update_component','',24,368,432,24,16,color=MUTED,align='center')
    hide('update_owner');hide('update_verified')
    button(80,'button_cancel_download','Cancel',24,408,432,56)
    add('image',91,200,80,80,80,image='assets/mosaic/hero_empty-80.png')
    text(91,'result_title','',24,178,432,42,28,'title',align='center')
    text(91,'result_detail','',24,234,432,72,18,color=MUTED,align='center')
    button(91,'button_result_library','Done',24,380,208,84)
    button(91,'button_result_open','Open',248,380,208,84,primary=True)
    hide('button_home')

    # Reset/delete preserve the accepted NAND policy and three-second hold.
    for name in ('reset','delete_confirm'):
        p=index[name]
        note='reset_note' if name=='reset' else 'delete_note'
        move(note,24,112,432,132,font_size=18,font=FONT['sans'],fg_color=MUTED)
        if name=='reset':
            add('progress',p,24,348,432,96,name='reset_hold_progress',bind='reset_hold_progress',
                bind_target='value',bg_color=RAISED,fg_color='#612629',radius=20)
        button(p,'button_reset_hold' if name=='reset' else 'button_delete_hold','Hold to reset' if name=='reset' else 'Hold to delete',24,348,432,96,danger=True)
        if name=='reset':
            named['button_reset_hold']['bg_color']='#00000001'

    # Unify legacy detail labels with the reviewed title/body styles.
    for o in objects:
        if o.get('font')==FONT['sans'] and o.get('font_size') in (22,24):
            o.update(font=FONT['medium'],font_size=22)

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
    for name in ('update_title','result_title'): values.setdefault(name,set()).update(titles)
    details.update(('Reading and verifying files', 'Checking installed application',
                    'Preparing application', 'Writing application'))
    for name in ('update_detail','result_detail'): values.setdefault(name,set()).update(details)
    for i in range(6):values.setdefault('cell_name_'+str(i),set()).add('Add Idea')
    values.setdefault('bundles_detail',set()).update(('Current','Update available','Updating · %u%%','Waiting'))
    values.setdefault('shared_field',set()).update(('Key','Value','Saving settings...','Please use a-z, 0-9 and _'))
    values.setdefault('shared_submit_label',set()).update(('Next','Save'))
    values.setdefault('shared_reveal_label',set()).update(('Show','Hide'))
    for i in range(4):values.setdefault('shared_row_'+str(i)+'_label',set()).add('Undo · %u s')
    return {name: ''.join(sorted(''.join(strings)+''.join(translations.get(s,'') for s in strings))) for name,strings in values.items()}
