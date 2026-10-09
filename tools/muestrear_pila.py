import ctypes, ctypes.wintypes as w, sys, time, collections, os
k=ctypes.WinDLL('kernel32',use_last_error=True); ps=ctypes.WinDLL('psapi'); dh=ctypes.WinDLL('dbghelp')
V=ctypes.c_void_p
k.OpenThread.restype=V; k.OpenProcess.restype=V
k.SuspendThread.argtypes=[V]; k.ResumeThread.argtypes=[V]; k.GetThreadContext.argtypes=[V,V]
k.ReadProcessMemory.argtypes=[V,V,V,ctypes.c_size_t,V]
ps.GetModuleFileNameExW.argtypes=[V,V,V,w.DWORD]; ps.GetModuleInformation.argtypes=[V,V,V,w.DWORD]; ps.EnumProcessModules.argtypes=[V,V,w.DWORD,V]
dh.SymInitialize.argtypes=[V,V,w.BOOL]; dh.SymLoadModuleExW.argtypes=[V,V,w.LPCWSTR,w.LPCWSTR,ctypes.c_uint64,w.DWORD,V,w.DWORD]; dh.SymLoadModuleExW.restype=ctypes.c_uint64
dh.SymFromAddr.argtypes=[V,ctypes.c_uint64,V,V]
pid=int(sys.argv[1]); secs=float(sys.argv[3])
class TE(ctypes.Structure):
    _fields_=[('sz',w.DWORD),('u',w.DWORD),('tid',w.DWORD),('owner',w.DWORD),('b',w.LONG),('f',w.LONG),('g',w.DWORD)]
k.CreateToolhelp32Snapshot.restype=V
snap=k.CreateToolhelp32Snapshot(4,0); te=TE(); te.sz=ctypes.sizeof(te); tids=[]
k.GetThreadDescription.argtypes=[V,V]
ok=k.Thread32First(V(snap),ctypes.byref(te))
while ok:
    if te.owner==pid:
        h=k.OpenThread(0x800,False,te.tid)
        if h:
            d=ctypes.c_void_p(); k.GetThreadDescription(h,ctypes.byref(d))
            if d.value and ctypes.wstring_at(d.value).startswith(sys.argv[2]): tids.append(te.tid)
    ok=k.Thread32Next(V(snap),ctypes.byref(te))
print('hilos',tids)
import re,bisect
msyms=[]
for l in open('mapped.map',errors='ignore'):
    m=re.match(r'\s+0001:[0-9a-f]+\s+(\S+)\s+([0-9a-f]{16})\s',l)
    if m: msyms.append((int(m.group(2),16)-0x180000000,m.group(1)))
msyms.sort(); maddr=[x[0] for x in msyms]
def msym(rva):
    i=bisect.bisect_right(maddr,rva)-1
    return msyms[i][1] if i>=0 else '?'
hp=k.OpenProcess(0x1F0FFF,False,pid)
dh.SymSetOptions(0x2|0x4)  # undecorate, defer
dh.SymInitialize(hp,None,False)
mods=(V*1024)(); n=w.DWORD(); ps.EnumProcessModules(hp,mods,ctypes.sizeof(mods),ctypes.byref(n))
ml=[]
for i in range(n.value//8):
    nm=ctypes.create_unicode_buffer(260); ps.GetModuleFileNameExW(hp,mods[i],nm,260)
    mi=(ctypes.c_uint64*3)(); ps.GetModuleInformation(hp,mods[i],mi,24)
    b=mods[i]; e=b+mi[1]; name=os.path.basename(nm.value)
    ml.append((b,e,name)); dh.SymLoadModuleExW(hp,None,nm.value,None,b,mi[1],None,0)
SZ=ctypes.sizeof(ctypes.c_byte)
class SYM(ctypes.Structure):
    _fields_=[('SizeOfStruct',w.ULONG),('TypeIndex',w.ULONG),('R',ctypes.c_uint64*2),('Index',w.ULONG),('Size',w.ULONG),('ModBase',ctypes.c_uint64),('Flags',w.ULONG),('Value',ctypes.c_uint64),('Address',ctypes.c_uint64),('Register',w.ULONG),('Scope',w.ULONG),('Tag',w.ULONG),('NameLen',w.ULONG),('MaxNameLen',w.ULONG),('Name',ctypes.c_char*256)]
def sym(ip):
    for b,e,nm in ml:
        if b<=ip<e:
            if nm=='rexgpu-odisea.dll': return 'ODISEA:'+msym(ip-b)
            s=SYM(); s.SizeOfStruct=88; s.MaxNameLen=255; d=ctypes.c_uint64()
            if dh.SymFromAddr(hp,ip,ctypes.byref(d),ctypes.byref(s)): return '%s!%s+%x'%(nm,s.Name.decode(),d.value)
            return '%s+%x'%(nm,ip-b)
    return 'unk'
def mod(ip):
    for b,e,nm in ml:
        if b<=ip<e: return nm
    return None

dh.StackWalk64.argtypes=[w.DWORD,V,V,V,V,V,V,V,V]; dh.StackWalk64.restype=w.BOOL
FTA=ctypes.cast(dh.SymFunctionTableAccess64,V).value; GMB=ctypes.cast(dh.SymGetModuleBase64,V).value
buf=ctypes.create_string_buffer(1300); off=(16-ctypes.addressof(buf)%16)%16
ctx=(ctypes.c_char*1232).from_buffer(buf,off)
hs={t:k.OpenThread(0x1A,False,t) for t in tids}
FOCUS=os.environ.get('FOCUS'); CALLEES=os.environ.get('CALLEES'); CAL=collections.Counter(); OFFS=collections.Counter(); INC=collections.Counter(); CH=collections.Counter(); SELF=collections.Counter(); tot=0
sf=ctypes.create_string_buffer(512)
def fname(ip):
    m=mod(ip)
    if m=='rexgpu-odisea.dll':
        for b,e,nm in ml:
            if nm==m: return msym(ip-b)
    return sym(ip)
end=time.time()+secs
while time.time()<end:
    for t,h in hs.items():
        k.SuspendThread(h)
        ctypes.memset(ctx,0,1232); ctypes.c_uint32.from_buffer(ctx,0x30).value=0x100003
        frames=[]
        if k.GetThreadContext(h,ctx):
            ctypes.memset(sf,0,512)
            for off_,reg in((0,0xF8),(32,0xA0),(48,0x98)):
                ctypes.c_uint64.from_buffer(sf,off_).value=ctypes.c_uint64.from_buffer(ctx,reg).value
                ctypes.c_uint32.from_buffer(sf,off_+12).value=3
            for _ in range(48):
                if not dh.StackWalk64(0x8664,hp,h,sf,ctx,None,FTA,GMB,None): break
                pc=ctypes.c_uint64.from_buffer(sf,0).value
                if not pc: break
                frames.append(pc)
        k.ResumeThread(h)
        if frames:
            tot+=1
            names=[fname(f) for f in frames]
            SELF[names[0]]+=1
            if CALLEES:
                for i,n in enumerate(names):
                    if CALLEES in n:
                        CAL[names[i-1][:110] if i>0 else '(propio)']+=1; break
            if FOCUS and FOCUS in names[0]:
                for b,e,nm in ml:
                    if nm=='rexgpu-odisea.dll' and b<=frames[0]<e:
                        r=frames[0]-b; i=bisect.bisect_right(maddr,r)-1; OFFS[r-maddr[i]]+=1
            for key in sys.argv[5:]:
                for i,n in enumerate(names):
                    if key in n:
                        CH[key+' <- '+' <- '.join(x[:60] for x in names[i+1:i+4])]+=1; break
            for n in set(names): INC[n]+=1
    time.sleep(0.003)
print('samples',tot)
print('--- INCLUSIVO')
for f,c in INC.most_common(int(sys.argv[4]) if len(sys.argv)>4 else 60): print('%5.1f%%  %s'%(100*c/tot,f[:160]))

print('--- CADENAS')
for f,c in CH.most_common(25): print('%5.1f%%  %s'%(100*c/tot,f))

if FOCUS:
    print('--- OFFSETS en',FOCUS)
    for o,c in sorted(OFFS.items(), key=lambda x:-x[1])[:25]: print('  +%x  %d'%(o,c))

if CALLEES:
    print('--- LLAMADAS DESDE',CALLEES)
    for f,c in CAL.most_common(25): print('%5.1f%%  %s'%(100*c/tot,f))
