"""Extend the owned game's GoGui forms; never write into the extraction.

The output is a read-only archive overlay consumed by the retail resource loader.
MGF directory format: LarvalExtract/MGF-Explorer (see inspect_mgf.py).
"""
from pathlib import Path
import copy, hashlib, struct, zlib
from inspect_mgf import Mgf

PROJECT=Path(__file__).resolve().parents[1]
NAME=0x1487300E; ID=0xFADBB5D7; PARENT=0xC644C4C4; CHILD=0x3331CA50; NEXT=0x3A281A30
ACTION=0xDE48BE56; TEXT=0xBAF9634C; FONT=0xBC5F04F8; FOCUS=0xFDE73B4E
X=0xFBB2E896; Y=0x8CB5D800; W=0x40DCC1F2; H=0xDFE1626F
TEXT_CLASS=0x94A35D35
WIDESCREEN_MODE=0x87824F17
PARENT_STATE=0x007D2089
HORIZONTAL_JUSTIFICATION=0xE7F7101D
VERTICAL_JUSTIFICATION=0xF8F5965B

def cache_version(data):
    return ('PC2-'+hashlib.sha256(data).hexdigest()[:16]+'\r\n').encode('ascii')

def parse(data):
    magic,count,_,_=struct.unpack_from('<4I',data)
    assert magic==0xF8002E00
    objects=[]; pos=16
    for _ in range(count):
        magic,n,kind=struct.unpack_from('<3I',data,pos); pos+=12
        assert magic==0xF8012E01
        props={}
        for _ in range(n):
            magic,key,size,typ=struct.unpack_from('<4I',data,pos); pos+=16
            assert magic==0xF8022E02
            props[key]=(typ,data[pos:pos+size]); pos+=size
        objects.append([kind,props])
    assert pos==len(data)
    return objects

def get(o,k,default=0):
    return struct.unpack('<I',o[1][k][1])[0] if k in o[1] else default

def put(o,k,v):
    typ=o[1].get(k,(6,b''))[0]
    o[1][k]=(typ,struct.pack('<I',v&0xffffffff))

def string(o,k,v): o[1][k]=(9,v.encode('ascii'))

def encode(objects):
    out=bytearray(struct.pack('<4I',0xF8002E00,len(objects),0,0))
    for kind,props in objects:
        out+=struct.pack('<3I',0xF8012E01,len(props),kind)
        for key,(typ,data) in sorted(props.items()):
            out+=struct.pack('<4I',0xF8022E02,key,len(data),typ)+data
    return out

def rectangle(o,x,y,w,h):
    for key,value in [(X,x),(Y,y),(W,w),(H,h),(0x193444DF,x),(0x24546D6F,y),(0x3D2175C9,w),(0xA36D69E2,h)]:
        o[1][key]=(3,struct.pack('<i',value))

def settings_form(m,page):
    original=parse(m.file('interface/menus/volume/volume.ggf'))
    options=parse(m.file('interface/menus/options/options.ggf'))
    root=copy.deepcopy(original[0]); panel=copy.deepcopy(original[1])
    put(root,ID,1); put(root,CHILD,2); put(panel,ID,2);put(panel,PARENT,1);put(panel,CHILD,3)
    rectangle(panel,32,24,576,432)
    # This retail texture has wide transparent margins. Crop its source UVs
    # to the authored blue panel so the destination rectangle is the box.
    panel[1][0x148F1E8A]=(15,struct.pack('<4i',106,20,300,217))
    template=next(o for o in original if get(o,TEXT)==188)
    # Rows are left-aligned. The retail volume label was right-aligned to its slider.
    template=copy.deepcopy(template);put(template,0xE7F7101D,0);put(template,0x7D2089,0)
    title=copy.deepcopy(template)
    put(title,ID,3);put(title,PARENT,2);put(title,CHILD,0);put(title,NEXT,10);put(title,ACTION,9000)
    put(title,TEXT,0x70000000+page*100);rectangle(title,70,54,500,30)
    string(title,FONT,'\\Interface\\Fonts\\English\\ImpactReg18.mgFont')
    result=[root,panel,title]
    # Keep the complete display page inside the original blue panel.
    rows=12 if page==0 else 10
    step=24 if page==0 else 28
    for row in range(rows):
        item=copy.deepcopy(template); n=10+row
        put(item,ID,n);put(item,PARENT,2);put(item,CHILD,0);put(item,NEXT,n+1)
        put(item,ACTION,9100+row);put(item,TEXT,0x70000001+page*100+row)
        put(item,FOCUS,0);rectangle(item,76,99+row*step,490,step)
        # Use the original normal/highlight colors; selection is owned by the page.
        result.append(item)
    footer=copy.deepcopy(template)
    put(footer,ID,10+rows);put(footer,PARENT,2);put(footer,CHILD,0);put(footer,NEXT,23 if page==0 else 0);put(footer,ACTION,9200)
    put(footer,TEXT,0x70000011+page*100);rectangle(footer,76,416 if page==0 else 406,490,28)
    result.append(footer)
    if page==0:
        hint=copy.deepcopy(template)
        put(hint,ID,23);put(hint,PARENT,2);put(hint,CHILD,0);put(hint,NEXT,24);put(hint,FOCUS,0)
        put(hint,ACTION,9300);put(hint,TEXT,0x70000012);rectangle(hint,76,391,490,22)
        result.append(hint)
        for original_id,new_id,action in [(4,24,9301),(5,25,9302)]:
            slider=copy.deepcopy(next(o for o in original if get(o,ID)==original_id))
            put(slider,ID,new_id);put(slider,PARENT,2);put(slider,CHILD,0);put(slider,NEXT,25 if new_id==24 else 0)
            put(slider,ACTION,action);put(slider,FOCUS,0);put(slider,PARENT_STATE,0);put(slider,WIDESCREEN_MODE,2)
            put(slider,0xA33FB056,1);rectangle(slider,325,99+7*step+5,230,16)
            if original_id==5:put(slider,0x8DBE6966,101) # NumValues, inclusive 0..100
            result.append(slider)
    return result

def patch_legends(objects,template):
    changed=False
    by_id={get(o,ID):o for o in objects}
    buttons={'button a':0,'button b':1,'button x':2,'button y':3,'button white':4,'button black':5}
    for i,o in enumerate(objects):
        name=o[1].get(NAME,(9,b''))[1].decode('ascii',errors='ignore').lower()
        if o[0]!=0xBE564DF5 or name not in buttons:continue
        t=copy.deepcopy(template)
        for key in (ID,PARENT,CHILD,NEXT,ACTION,FOCUS):put(t,key,get(o,key))
        put(t,FOCUS,0);string(t,NAME,'PC input legend')
        put(t,PARENT_STATE,0);put(t,HORIZONTAL_JUSTIFICATION,2)
        put(t,WIDESCREEN_MODE,2)
        string(t,FONT,'\\Interface\\Fonts\\English\\ArialUnicodeMSBold14.mgFont')
        put(t,TEXT,0x70010000+buttons[name]);put(t,0xA33FB056,get(o,0xA33FB056))
        caption=by_id.get(get(o,CHILD))
        if caption and caption[0]==TEXT_CLASS:
            # One coordinate policy and baseline for the complete pair.
            # Retail forms have independent widescreen rectangles; an icon
            # clone without those properties otherwise moves separately from
            # its caption at ultrawide aspect ratios.
            x,y,w,h=(get(caption,k) for k in (X,Y,W,H))
            rectangle(caption,x,y,w,h);put(caption,WIDESCREEN_MODE,2)
            put(t,VERTICAL_JUSTIFICATION,get(caption,VERTICAL_JUSTIFICATION))
            rectangle(t,x-64,y,56,h)
        else:
            x=struct.unpack('<i',o[1][X][1])[0]
            rectangle(t,x-12,get(o,Y),56,get(o,H))
        # Text must stay visible independently of the cloned label's focus state.
        put(t,0x1255CE12,1)
        objects[i]=t;changed=True
    return changed

def build():
    m=Mgf(PROJECT/'game_files/common.mgf'); changes={}
    legend_template=next(o for o in parse(m.file('interface/menus/volume/volume.ggf')) if get(o,TEXT)==28)
    pages={'interface/menus/calibration/calibration.ggf':0,
           'interface/menus/volume/volume.ggf':1,
           'interface/menus/pause/p_volume.ggf':1,
           'interface/menus/pause/p_controller.ggf':2,
           'interface/menus/controlleroptions/controlleroptions_newui.ggf':2,
           'interface/menus/controlleroptions/controlleroptions.ggf':2}
    for name in m.files:
        if not name.lower().endswith('.ggf'):continue
        objects=parse(m.file(name))
        if name.lower() in pages:
            objects=settings_form(m,pages[name.lower()]);changes[name]=encode(objects)
        elif patch_legends(objects,legend_template):changes[name]=encode(objects)
    assert all(any(n.lower()==p for n in changes) for p in pages)
    data=bytearray(m.read(0,m.size)); table=struct.unpack_from('<I',data,20)[0]
    for name,payload in changes.items():
        data+=b'\0'*((-len(data))%16); offset=len(data);data+=payload
        record=list(m.records[m.files[name]])
        # MGF stores the un-finalized CRC accumulator, not the usual ZIP CRC.
        record[1]=zlib.crc32(payload)^0xffffffff;record[3]=record[4]=len(payload);record[6]=offset
        struct.pack_into('<8I',data,table+m.files[name]*32,*record)
    out=PROJECT/'pc_assets/common.mgf';out.parent.mkdir(exist_ok=True);out.write_bytes(data)
    verify=Mgf(out)
    for name,payload in changes.items():
        assert verify.file(name)==payload
        assert verify.records[verify.files[name]][1]==zlib.crc32(payload)^0xffffffff
    # The retail loader caches whole archives. A content-specific namespace
    # prevents old menus from surviving an application/asset update.
    (out.parent/'cache.version').write_bytes(cache_version(data))
    print(f'Built {len(changes)} native forms in {out} ({len(data):,} bytes)')

if __name__=='__main__':
    import argparse
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--project',type=Path,default=PROJECT,
        help='Game root containing game_files; writes only its pc_assets directory.')
    PROJECT=parser.parse_args().project.resolve()
    build()
