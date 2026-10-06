import ctypes as c
from ctypes import wintypes as w
from pathlib import Path
import os, sys, json, time, hashlib

root = Path(sys.argv[1]).resolve()
settings=json.loads((root/'config.json').read_text())
exe = Path(settings['exe']).resolve()
k = c.WinDLL('kernel32', use_last_error=True)
u = c.WinDLL('user32', use_last_error=True)
class SI(c.Structure):
    _fields_ = [('cb',w.DWORD),('reserved',w.LPWSTR),('desktop',w.LPWSTR),('title',w.LPWSTR),('x',w.DWORD),('y',w.DWORD),('xsize',w.DWORD),('ysize',w.DWORD),('xchars',w.DWORD),('ychars',w.DWORD),('fill',w.DWORD),('flags',w.DWORD),('show',w.WORD),('reserved2size',w.WORD),('reserved2',c.c_void_p),('stdin',w.HANDLE),('stdout',w.HANDLE),('stderr',w.HANDLE)]
class PI(c.Structure):
    _fields_ = [('process',w.HANDLE),('thread',w.HANDLE),('pid',w.DWORD),('tid',w.DWORD)]
class ER(c.Structure):
    _fields_ = [('code',w.DWORD),('flags',w.DWORD),('record',c.c_void_p),('address',c.c_void_p),('nparams',w.DWORD),('params',c.c_size_t*15)]
class EX(c.Structure):
    _fields_ = [('record',ER),('first',w.DWORD)]
class DS(c.Structure):
    _fields_ = [('address',c.c_void_p),('unicode',w.WORD),('length',w.WORD)]
class CP(c.Structure):
    _fields_ = [('file',w.HANDLE),('process',w.HANDLE),('thread',w.HANDLE),('base',c.c_void_p),('offset',w.DWORD),('size',w.DWORD),('tls',c.c_void_p),('start',c.c_void_p),('name',c.c_void_p),('unicode',w.WORD)]
class DL(c.Structure):
    _fields_ = [('file',w.HANDLE),('base',c.c_void_p),('offset',w.DWORD),('size',w.DWORD),('name',c.c_void_p),('unicode',w.WORD)]
class DU(c.Union):
    _fields_ = [('exception',EX),('string',DS),('created',CP),('dll',DL),('exit',w.DWORD),('raw',c.c_byte*160)]
class DE(c.Structure):
    _fields_ = [('code',w.DWORD),('pid',w.DWORD),('tid',w.DWORD),('data',DU)]
assert c.sizeof(DE)==176 and c.sizeof(SI)==104
k.CreateProcessW.argtypes=[w.LPCWSTR,w.LPWSTR,c.c_void_p,c.c_void_p,w.BOOL,w.DWORD,c.c_void_p,w.LPCWSTR,c.POINTER(SI),c.POINTER(PI)]
k.CreateProcessW.restype=w.BOOL
k.WaitForDebugEvent.argtypes=[c.POINTER(DE),w.DWORD]
k.ReadProcessMemory.argtypes=[w.HANDLE,c.c_void_p,c.c_void_p,c.c_size_t,c.POINTER(c.c_size_t)]
k.ContinueDebugEvent.argtypes=[w.DWORD,w.DWORD,w.DWORD]
k.CloseHandle.argtypes=[w.HANDLE]
k.TerminateProcess.argtypes=[w.HANDLE,w.UINT]
k.GetFinalPathNameByHandleW.argtypes=[w.HANDLE,w.LPWSTR,w.DWORD,w.DWORD]
k.OpenThread.argtypes=[w.DWORD,w.BOOL,w.DWORD];k.OpenThread.restype=w.HANDLE
k.GetThreadContext.argtypes=[w.HANDLE,c.c_void_p]
k.CreateFileW.argtypes=[w.LPCWSTR,w.DWORD,w.DWORD,c.c_void_p,w.DWORD,w.DWORD,w.HANDLE];k.CreateFileW.restype=w.HANDLE
dbg=c.WinDLL('D:/Windows Kits/10/Debuggers/x64/dbghelp.dll',use_last_error=True)
dbg.SymInitializeW.argtypes=[w.HANDLE,w.LPCWSTR,w.BOOL]
dbg.SymFunctionTableAccess64.argtypes=[w.HANDLE,c.c_uint64];dbg.SymFunctionTableAccess64.restype=c.c_void_p
dbg.SymGetModuleBase64.argtypes=[w.HANDLE,c.c_uint64];dbg.SymGetModuleBase64.restype=c.c_uint64
dbg.StackWalk64.argtypes=[w.DWORD,w.HANDLE,w.HANDLE,c.c_void_p,c.c_void_p,c.c_void_p,c.c_void_p,c.c_void_p,c.c_void_p]
dbg.SymFromAddr.argtypes=[w.HANDLE,c.c_uint64,c.POINTER(c.c_uint64),c.c_void_p]
dbg.SymGetLineFromAddr64.argtypes=[w.HANDLE,c.c_uint64,c.POINTER(w.DWORD),c.c_void_p]
dbg.MiniDumpWriteDump.argtypes=[w.HANDLE,w.DWORD,w.HANDLE,w.DWORD,c.c_void_p,c.c_void_p,c.c_void_p]
class LI(c.Structure):
    _fields_=[('size',w.DWORD),('key',c.c_void_p),('line',w.DWORD),('file',c.c_char_p),('address',c.c_uint64)]
class EI(c.Structure):
    _fields_=[('tid',w.DWORD),('pointers',c.c_void_p),('client',w.BOOL)]
class EP(c.Structure):
    _fields_=[('record',c.c_void_p),('context',c.c_void_p)]
modules=[]
def module_record(file,base):
    buf=c.create_unicode_buffer(32768)
    if file and k.GetFinalPathNameByHandleW(file,buf,len(buf),0):modules.append({'base':base,'path':buf.value})
def diagnose(ev):
    th=k.OpenThread(0x48,False,ev.tid)
    storage=c.create_string_buffer(1248);ptr=(c.addressof(storage)+15)&~15
    c.c_uint32.from_address(ptr+48).value=0x10001f
    if not th or not k.GetThreadContext(th,ptr):
        return {'contextFailure':c.get_last_error()}
    regs={name:c.c_uint64.from_address(ptr+offset).value for name,offset in [('rax',120),('rcx',128),('rdx',136),('rbx',144),('rsp',152),('rbp',160),('rip',248)]}
    record=ev.data.exception.record
    ep=EP(c.addressof(record),ptr);ei=EI(ev.tid,c.addressof(ep),False)
    file=k.CreateFileW(str(root/'crash.dmp'),0x40000000,0,None,2,0,None)
    ok=bool(dbg.MiniDumpWriteDump(pi.process,pi.pid,file,0x1000,c.byref(ei),None,None));k.CloseHandle(file)
    dbg.SymSetOptions(0x10|0x4|0x2)
    initialized=bool(dbg.SymInitializeW(pi.process,'E:/AIProMax/github/cocos4-vulkan-test-project/build/windows/proj/Debug',True))
    sf=c.create_string_buffer(1024)
    for offset,value in [(0,regs['rip']),(32,regs['rbp']),(48,regs['rsp'])]:
        c.c_uint64.from_buffer(sf,offset).value=value;c.c_uint32.from_buffer(sf,offset+12).value=3
    frames=[]
    for n in range(40):
        if not dbg.StackWalk64(0x8664,pi.process,th,sf,ptr,None,c.cast(dbg.SymFunctionTableAccess64,c.c_void_p),c.cast(dbg.SymGetModuleBase64,c.c_void_p),None):break
        pc=c.c_uint64.from_buffer(sf,0).value
        if not pc:break
        sym=c.create_string_buffer(4096);c.c_uint32.from_buffer(sym,0).value=88;c.c_uint32.from_buffer(sym,80).value=2048
        disp=c.c_uint64();name='';li=LI();li.size=c.sizeof(li);ld=w.DWORD()
        if dbg.SymFromAddr(pi.process,pc,c.byref(disp),sym):name=c.string_at(c.addressof(sym)+84).decode(errors='replace')
        line={}
        if dbg.SymGetLineFromAddr64(pi.process,pc,c.byref(ld),c.byref(li)):line={'file':li.file.decode(errors='replace'),'line':li.line}
        module=next((m for m in sorted(modules,key=lambda m:m['base'],reverse=True) if m['base']<=pc),None)
        frames.append({'pc':hex(pc),'symbol':name,'displacement':disp.value,'source':line,'module':module})
    k.CloseHandle(th)
    return {'registers':regs,'accessType':record.params[0],'accessAddress':hex(record.params[1]),'dumpWritten':ok,'symInitialized':initialized,'frames':frames,'modules':modules}
u.GetWindowThreadProcessId.argtypes=[w.HWND,c.POINTER(w.DWORD)]
u.PostMessageW.argtypes=[w.HWND,w.UINT,w.WPARAM,w.LPARAM]
CB=c.WINFUNCTYPE(w.BOOL,w.HWND,w.LPARAM)
def close_windows():
    @CB
    def fn(hwnd,param):
        pid=w.DWORD();u.GetWindowThreadProcessId(hwnd,c.byref(pid))
        if pid.value==pi.pid: u.PostMessageW(hwnd,0x10,0,0)
        return True
    u.EnumWindows(fn,0)
env_before=dict(os.environ)
os.environ['VK_LAYER_PATH']='D:/Software/VulkanSDK/1.4.341.1/Bin'
(root/'empty-implicit').mkdir()
os.environ['VK_IMPLICIT_LAYER_PATH']=str(root/'empty-implicit')
for name in list(os.environ):
    if name.startswith('CC_VULKAN_REAL_PROJECT_PERF_') or name == 'CC_VULKAN_REAL_PROJECT_VISIBILITY':
        os.environ.pop(name,None)
for name in ['CC_VULKAN_FORCE_API','CC_VULKAN_COMPLETION','CC_VULKAN_SUBMIT','CC_VULKAN_BARRIER','CC_VULKAN_RENDERING','CC_VULKAN_DISABLE_FEATURES','CC_VULKAN_PIPELINE_CACHE','CC_VULKAN_REAL_PROJECT_CACHE_PATH','CC_VULKAN_REAL_PROJECT_PREWARM','CC_VULKAN_REAL_PROJECT_INDIRECT','CC_VULKAN_REAL_PROJECT_INDIRECT_CAPTURE','CC_VULKAN_REAL_PROJECT_RESOURCE_ROOT','CC_VULKAN_REAL_PROJECT_SUBPASS','CC_VULKAN_REAL_PROJECT_SUBPASS_CAPTURE','CC_VULKAN_REAL_PROJECT_RESOLVE','CC_VULKAN_REAL_PROJECT_RESOLVE_CAPTURE','CC_VULKAN_REAL_PROJECT_SCOPES','CC_VULKAN_REAL_PROJECT_FEATURES','CC_VULKAN_REAL_PROJECT_FEATURES_CAPTURE','CC_VULKAN_REAL_PROJECT_WSI','CC_VULKAN_REAL_PROJECT_PERFORMANCE','CC_VULKAN_REAL_PROJECT_PERF_CACHE','CC_VULKAN_REAL_PROJECT_PERF_FRAMES','CC_VULKAN_REAL_PROJECT_PERF_WARMUP']:
    os.environ.pop(name,None)
os.environ['CC_VULKAN_BARRIER']='legacy'
settings=json.loads((root/'config.json').read_text())
os.environ.update(settings['env'])
si=SI();si.cb=c.sizeof(si);si.flags=0;si.show=0
pi=PI()
cmd=c.create_unicode_buffer('"'+str(exe)+'"')
launch_qpc=c.c_int64();qpc_frequency=c.c_int64();k.QueryPerformanceFrequency(c.byref(qpc_frequency));k.QueryPerformanceCounter(c.byref(launch_qpc))
if not k.CreateProcessW(str(exe),cmd,None,None,False,2,None,str(root),c.byref(si),c.byref(pi)):
    raise c.WinError(c.get_last_error())
os.environ.clear();os.environ.update(env_before)
(root/'pid.txt').write_text(str(pi.pid))
start=time.monotonic(); errors=0;closing=None;initial_break=False;exitcode=None; exceptions=[]; count=0
print('launched pid='+str(pi.pid),flush=True)
with (root/'debug-output.log').open('w',encoding='utf-8',buffering=1) as log, (root/'debug-events.jsonl').open('w',encoding='utf-8',buffering=1) as events:
    while True:
        elapsed=time.monotonic()-start
        if closing is None and (elapsed>settings.get('closeAfterSeconds',300) or (root/'close-request').exists() or errors>=10):
            close_windows();closing=elapsed;events.write(json.dumps({'action':'WM_CLOSE','elapsed':elapsed,'errors':errors})+'\n')
        if closing is not None and elapsed-closing>90:
            k.TerminateProcess(pi.process,0xEE01)
        ev=DE()
        if not k.WaitForDebugEvent(c.byref(ev),100):
            if c.get_last_error()==121:continue
            raise c.WinError(c.get_last_error())
        cont=0x10002
        if ev.code==8:
            d=ev.data.string;buf=c.create_string_buffer(d.length*(2 if d.unicode else 1));nr=c.c_size_t()
            if k.ReadProcessMemory(pi.process,d.address,buf,len(buf),c.byref(nr)):
                msg=buf.raw[:nr.value].decode('utf-16-le' if d.unicode else 'utf-8',errors='replace').rstrip('\0')
                log.write(msg);count+=1
                if '[ERROR]' in msg or 'SYNC-HAZARD-' in msg:errors+=1
        elif ev.code==1:
            r=ev.data.exception
            row={'exception':hex(r.record.code),'firstChance':bool(r.first),'address':hex(r.record.address or 0),'elapsed':elapsed};exceptions.append(row);events.write(json.dumps(row)+'\n')
            if r.record.code==0xc0000005 and not (root/'crash-analysis.json').exists():
                details=diagnose(ev);(root/'crash-analysis.json').write_text(json.dumps(details,indent=2),encoding='utf-8')
            if r.record.code==0x80000003 and not initial_break:initial_break=True
            else:cont=0x80010001
        elif ev.code==3:
            module_record(ev.data.created.file,ev.data.created.base)
            if ev.data.created.file:k.CloseHandle(ev.data.created.file)
        elif ev.code==6:
            module_record(ev.data.dll.file,ev.data.dll.base)
            if ev.data.dll.file:k.CloseHandle(ev.data.dll.file)
        elif ev.code==5:
            exitcode=ev.data.exit
        k.ContinueDebugEvent(ev.pid,ev.tid,cont)
        if ev.code==5:break
k.CloseHandle(pi.thread);k.CloseHandle(pi.process)
summary={'executor':'Lead','exe':str(exe),'pid':pi.pid,'exitCode':exitcode,'seconds':time.monotonic()-start,'errorLogMessages':errors,'debugMessages':count,'exceptions':exceptions,'modules':modules,'capture':'Windows debug events / OutputDebugString; no source changes','exeSHA256':hashlib.sha256(exe.read_bytes()).hexdigest()}
(root/'runtime-summary.json').write_text(json.dumps(summary,indent=2),encoding='utf-8')
print(json.dumps({k:summary[k] for k in ['executor','pid','exitCode','seconds','errorLogMessages','exeSHA256']}),flush=True)

(root/'launch-clock.json').write_text(json.dumps({'LaunchQpc':launch_qpc.value,'QpcFrequency':qpc_frequency.value,'Clock':'Windows QueryPerformanceCounter before CreateProcessW'},indent=2))
