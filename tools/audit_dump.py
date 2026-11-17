"""Read-only inspection of an extracted Engage 2.0.0 dump. No game files are modified."""
import argparse
import ctypes
import ctypes.util
import hashlib
import json
from pathlib import Path
import struct

BUILDS={
    'main':'8c08b9719e085f91847b5e0f935d9488',
    'sdk':'56ec24747ec9e7f19b3efb062483a19d1da091a8',
    'subsdk0':'bffed55ea46c1b48ddeabcacdfbf81bf418125ac',
}
FE3H_LAYOUT={'main':0x7000000000,'sdk':0x7004000000,'subsdk0':0x7005000000}
ENGAGE_LAYOUT={'main':0x7000000000,'sdk':0x7008000000,'subsdk0':0x7010000000}
def sha(data):return hashlib.sha256(data).hexdigest()
def nso(path):
    raw=path.read_bytes()
    if raw[:4]!=b'NSO0' or len(raw)<256:raise ValueError(f'Not an NSO: {path.name}')
    flags=struct.unpack_from('<I',raw,12)[0]
    segments=[];image=bytearray()
    for i,name in enumerate(('text','rodata','data')):
        offset,address,size=struct.unpack_from('<III',raw,16+16*i)
        packed=struct.unpack_from('<I',raw,96+4*i)[0] if flags & (1<<i) else size
        if address+size>512*1024*1024 or offset+packed>len(raw):raise ValueError('Invalid segment bounds')
        payload=raw[offset:offset+packed]
        if flags & (1<<i):
            lib=ctypes.CDLL(ctypes.util.find_library('lz4') or 'liblz4.so.1')
            lib.LZ4_decompress_safe.argtypes=[ctypes.c_char_p,ctypes.c_void_p,ctypes.c_int,ctypes.c_int]
            lib.LZ4_decompress_safe.restype=ctypes.c_int
            output=ctypes.create_string_buffer(size)
            if lib.LZ4_decompress_safe(payload,output,len(payload),size)!=size:raise ValueError('LZ4 segment size mismatch')
            payload=output.raw
        actual=sha(payload)
        if flags & (1<<(3+i)) and actual!=raw[160+32*i:192+32*i].hex():raise ValueError(f'{path.name}: {name} integrity mismatch')
        if len(image)<address+size:image.extend(b'\0'*(address+size-len(image)))
        image[address:address+size]=payload
        segments.append({'name':name,'address':hex(address),'size':size,'sha256':actual})
    mod=struct.unpack_from('<I',image,4)[0]
    if image[mod:mod+4]!=b'MOD0':raise ValueError('Missing MOD0')
    dynamic=mod+struct.unpack_from('<i',image,mod+4)[0]
    tags={}
    for off in range(dynamic,len(image)-15,16):
        tag,val=struct.unpack_from('<QQ',image,off)
        if tag==0:break
        tags[tag]=val
    def string(at):
        end=image.find(b'\0',at)
        return image[at:end].decode('utf-8',errors='replace')
    imports=[];exports=[]
    if all(t in tags for t in (4,5,6)):
        count=struct.unpack_from('<I',image,tags[4]+4)[0]
        if count>1000000 or tags[6]+24*count>len(image):raise ValueError('Invalid symbol table')
        for i in range(count):
            name,info,other,section,value,size=struct.unpack_from('<IBBHQQ',image,tags[6]+24*i)
            symbol=string(tags[5]+name)
            if symbol:(exports if section else imports).append(symbol)
    text_lo=int(segments[0]['address'],16);text_size=segments[0]['size']
    svc={}
    for (word,) in struct.iter_unpack('<I',image[text_lo:text_lo+text_size]):
        if word & 0xffe0001f == 0xd4000001:
            key=f'0x{(word>>5)&0xffff:02x}';svc[key]=svc.get(key,0)+1
    build=raw[64:96].rstrip(b'\0').hex()
    bss=struct.unpack_from('<I',raw,0x3c)[0]
    return {'size':len(raw),'sha256':sha(raw),'build_id':build,'expected_build':BUILDS.get(path.name),
            'build_matches':path.name not in BUILDS or build==BUILDS[path.name],
            'segments':segments,'mapped_size':(len(image)+bss+0x3fff)&~0x3fff,
            'dynamic_tags':{hex(tag):hex(value) for tag,value in tags.items()},
            'imports':imports,'exports':exports,'svc_instructions':svc}

def layout_report(modules,layout):
    regions=[(base,base+modules[name]['mapped_size'],name) for name,base in layout.items()]
    regions.sort()
    collisions=[f'{a[2]} overlaps {b[2]}' for i,a in enumerate(regions) for b in regions[i+1:] if a[1]>b[0]]
    return {'regions':{name:{'base':hex(base),'end_exclusive':hex(end)} for base,end,name in regions},
            'collisions':collisions}

def audit(root):
    modules={name:nso(root/'exefs'/name) for name in ('main','subsdk0','sdk','rtld')}
    all_exports={s for m in modules.values() for s in m['exports']}
    for m in modules.values():m['imports_not_exported_by_modules']=sorted(set(m['imports'])-all_exports)
    bundle=(root/'romfs/Data/data.unity3d').open('rb').read(128)
    metadata=(root/'romfs/Data/Managed/Metadata/global-metadata.dat').read_bytes()
    magic,version=struct.unpack_from('<II',metadata)
    if magic!=0xfab11baf:raise ValueError('Invalid IL2CPP metadata signature')
    dlc=[]
    if (root/'DLC').is_dir():
        for folder in sorted((root/'DLC').iterdir()):
            if folder.is_dir():dlc.append({'id':folder.name,'files':[{'path':p.relative_to(folder).as_posix(),'size':p.stat().st_size} for p in sorted(folder.rglob('*')) if p.is_file()]})
    return {'game':'Fire Emblem Engage','target_version':'2.0.0','dump':str(root),
            'all_module_builds_match':all(m['build_matches'] for m in modules.values()),
            'unity_version_matches':b'2020.3.18f1\0' in bundle,'modules':modules,
            'metadata':{'version':version,'size':len(metadata),'sha256':sha(metadata)},'dlc':dlc,
            'fe3h_layout':layout_report(modules,FE3H_LAYOUT),'candidate_layout':layout_report(modules,ENGAGE_LAYOUT),
            'note':'Static inventory only. Does not establish bootability, asset completeness or performance.'}
if __name__=='__main__':
    ap=argparse.ArgumentParser();ap.add_argument('dump',type=Path);args=ap.parse_args()
    result=audit(args.dump)
    print(json.dumps(result,indent=2,ensure_ascii=False))
    if not result['all_module_builds_match'] or not result['unity_version_matches'] or result['candidate_layout']['collisions']:raise SystemExit(2)
