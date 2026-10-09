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
buf=ctypes.create_string_buffer(1300); off=(16-ctypes.addressof(buf)%16)%16
ctx=(ctypes.c_char*1232).from_buffer(buf,off)
hs={t:k.OpenThread(0x1A,False,t) for t in tids}
top=collections.Counter(); tot=0; INC=collections.Counter()
sb=ctypes.create_string_buffer(8192)
end=time.time()+secs
while time.time()<end:
    for t,h in hs.items():
        k.SuspendThread(h)
        ctypes.memset(ctx,0,1232); ctypes.c_uint32.from_buffer(ctx,0x30).value=0x100001
        ok=k.GetThreadContext(h,ctx)
        if ok:
            ip=ctypes.c_uint64.from_buffer(ctx,0xF8).value; sp=ctypes.c_uint64.from_buffer(ctx,0x98).value
            got=ctypes.c_size_t(); k.ReadProcessMemory(hp,sp,sb,8192,ctypes.byref(got))
            vals=(ctypes.c_uint64*(got.value//8)).from_buffer_copy(sb.raw[:got.value//8*8])
        k.ResumeThread(h)
        if ok:
            incl=set()
            for v in list(vals)+[ip]:
                if 0x0<v:
                    mm=mod(v)
                    if mm=='rexgpu-odisea.dll': incl.add(sym(v))
            for f in incl: INC[f]+=1
            m=mod(ip); chain=[sym(ip)]
            if m!='rexgpu-odisea.dll':
                c=0
                for v in vals:
                    mm=mod(v)
                    if mm=='rexgpu-odisea.dll':
                        chain.append(sym(v)); c+=1
                        if c>=1: break
            import re; top[re.sub(r'\+[0-9a-f]+','',' <- '.join(chain))]+=1; tot+=1
    time.sleep(0.002)
print('samples',tot)
for s,c in top.most_common(40): print('%5.1f%%  %s'%(100*c/tot,s))

print('--- INCLUSIVO (funciones presentes en la pila)')
for f,c in INC.most_common(45): print('%5.1f%%  %s'%(100*c/tot,f[:150]))
