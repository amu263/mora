"""Exercise the real Windows console input path in a hidden test console."""
import ctypes as c
from ctypes import wintypes as w
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
os.chdir(ROOT)
k = c.WinDLL('kernel32', use_last_error=True)
u = c.WinDLL('user32', use_last_error=True)
class Coord(c.Structure): _fields_ = [('X',w.SHORT),('Y',w.SHORT)]
class Rect(c.Structure): _fields_ = [('Left',w.SHORT),('Top',w.SHORT),('Right',w.SHORT),('Bottom',w.SHORT)]
class Char(c.Union): _fields_ = [('UnicodeChar',w.WCHAR),('AsciiChar',c.c_char)]
class Key(c.Structure): _fields_ = [('down',w.BOOL),('repeat',w.WORD),('vk',w.WORD),('scan',w.WORD),('char',Char),('control',w.DWORD)]
class Mouse(c.Structure): _fields_ = [('position',Coord),('buttons',w.DWORD),('control',w.DWORD),('flags',w.DWORD)]
class Event(c.Union): _fields_ = [('key',Key),('mouse',Mouse),('raw',c.c_byte*16)]
class Input(c.Structure): _fields_ = [('type',w.WORD),('event',Event)]
k.CreateFileW.argtypes=[w.LPCWSTR,w.DWORD,w.DWORD,c.c_void_p,w.DWORD,w.DWORD,w.HANDLE]
k.CreateFileW.restype=w.HANDLE
k.GetConsoleWindow.restype=w.HWND
k.WriteConsoleInputW.argtypes=[w.HANDLE,c.POINTER(Input),w.DWORD,c.POINTER(w.DWORD)]
k.SetConsoleWindowInfo.argtypes=[w.HANDLE,w.BOOL,c.POINTER(Rect)]
k.SetConsoleScreenBufferSize.argtypes=[w.HANDLE,Coord]
k.GetConsoleMode.argtypes=[w.HANDLE,c.POINTER(w.DWORD)]
k.SetConsoleMode.argtypes=[w.HANDLE,w.DWORD]
k.CloseHandle.argtypes=[w.HANDLE]
k.ReadConsoleOutputCharacterW.argtypes=[w.HANDLE,w.LPWSTR,w.DWORD,Coord,c.POINTER(w.DWORD)]

allocated=bool(k.AllocConsole())
window=k.GetConsoleWindow()
if allocated and window: u.ShowWindow(window,0)
conin=k.CreateFileW('CONIN$',0xC0000000,3,None,3,0,None)
conout=k.CreateFileW('CONOUT$',0xC0000000,3,None,3,0,None)
if conin==w.HANDLE(-1).value or conout==w.HANDLE(-1).value: raise OSError(c.get_last_error(),'Test console unavailable')
k.SetConsoleScreenBufferSize(conout,Coord(116,100))
rect=Rect(0,0,115,35)
k.SetConsoleWindowInfo(conout,True,c.byref(rect))
snapshot=ROOT/'build/ui-smoke.txt'
snapshot.unlink(missing_ok=True)

def send(vk=0,char='\0'):
    events=(Input*2)()
    for index,down in enumerate((True,False)):
        events[index].type=1
        events[index].event.key=Key(down,1,vk,0,Char(UnicodeChar=char),0)
    written=w.DWORD()
    if not k.WriteConsoleInputW(conin,events,2,c.byref(written)): raise OSError(c.get_last_error())

def click(x,y):
    events=(Input*2)()
    for index,button in enumerate((1,0)):
        events[index].type=2
        events[index].event.mouse=Mouse(Coord(x,y),button,0,0)
    written=w.DWORD()
    if not k.WriteConsoleInputW(conin,events,2,c.byref(written)): raise OSError(c.get_last_error())

def type_text(text):
    for char in text:send(0,char)

def command(text):
    type_text(text);send(13)

checks=[]
def expect(text,label):
    deadline=time.monotonic()+4
    while time.monotonic()<deadline:
        try: frame=snapshot.read_text(encoding='utf-8')
        except (OSError,UnicodeDecodeError): frame=''
        if text in frame: checks.append(label);return
        time.sleep(.1)
    raise AssertionError(f'{label}: expected {text!r}\n{frame}')

startup=subprocess.STARTUPINFO()
startup.dwFlags=subprocess.STARTF_USESHOWWINDOW
startup.wShowWindow=0
try:
    with open('CONIN$','rb',buffering=0) as input_file,open('CONOUT$','wb',buffering=0) as output_file:
        child=subprocess.Popen([str(ROOT/'build/mora.exe'),'--demo','--seconds','30','--snapshot',str(snapshot)],stdin=input_file,stdout=output_file,stderr=output_file,startupinfo=startup)
        try:
            expect('阿木','reference-based Amu home')
            type_text('/vol')
            expect('volume','slash suggestions')
            send(9)
            expect('/volume ','Tab completion')
            type_text('70');send(37);send(8);type_text('8');send(13)
            expect('80% VOL','edit command and set idle volume')
            command('/search 周杰伦')
            expect('离线界面演示','Unicode search command')
            send(40);send(13)
            expect('▸    七里香','select and activate song')
            send(ord('L'),'l')
            expect('账号 · 网易云音乐','keyboard login dialog')
            send(27);time.sleep(.15)
            click(60,25)
            expect('音质 · 下一首歌生效','mouse quality dialog')
            send(38);send(13)
            expect('音质将在下一首歌生效','quality selection')
            send(ord('M'),'m')
            expect('音频动效已关闭','motion toggle')
            click(108,25)
            expect('85% VOL','mouse volume')
            send(0,'?')
            expect('MORA · 操作指南','help dialog')
            send(27);time.sleep(.15)
            command('/home')
            expect('阿木','home command')
            command('/agent')
            expect('MCP stdio','MCP integration panel')
            send(27);time.sleep(.15)
            command('/ai')
            expect('音乐 Agent · 模型设置','truthful model adapter state')
            send(ord('K'),'k')
            expect('输入模型密钥','model key editor')
            type_text('mora-smoke-secret')
            expect('•••••••••••••••••','masked model key')
            frame=snapshot.read_text(encoding='utf-8')
            assert 'mora-smoke-secret' not in frame, 'model key leaked into terminal frame'
            checks.append('model key absent from frame')
            send(27);time.sleep(.15)
            send(27);time.sleep(.15)
            command('/guide')
            expect('新手引导 · 1 / 3','open onboarding guide')
            send(13)
            expect('新手引导 · 2 / 3','advance onboarding guide')
            send(13)
            expect('新手引导 · 3 / 3','optional agent setup')
            send(27);time.sleep(.15)
            expect('随时用 /guide 重看引导','skip guide without losing access')
            command('/guide')
            expect('新手引导 · 1 / 3','reopen onboarding guide')
            assert k.SetConsoleWindowInfo(conout,True,c.byref(Rect(0,0,77,23)))
            expect('S 或 Esc 跳过','guide fits minimum terminal')
            assert k.SetConsoleWindowInfo(conout,True,c.byref(Rect(0,0,115,35)))
            send(ord('S'),'s')
            command('推荐一首轻音乐')
            expect('演示模式 · 模型请求请退出后运行 mora','demo chat boundary')
            command('/quit')
            child.wait(timeout=5)
            assert child.returncode==0
        finally:
            if child.poll() is None: child.terminate();child.wait(timeout=5)
        with tempfile.TemporaryDirectory(prefix='mora-saved-session-') as isolated:
            environment=dict(os.environ,MORA_DATA_DIR=isolated)
            fixture="""Add-Type -AssemblyName System.Security
$json='{"device":"fixture-device","cookies":{"MUSIC_U":"fixture-cookie-only"},"uid":42,"nickname":"fixture-user"}'
$bytes=[Text.Encoding]::UTF8.GetBytes($json)
$protected=[Security.Cryptography.ProtectedData]::Protect($bytes,$null,[Security.Cryptography.DataProtectionScope]::CurrentUser)
[IO.File]::WriteAllBytes((Join-Path $env:MORA_DATA_DIR 'session.bin'),$protected)
"""
            subprocess.run(['powershell','-NoProfile','-Command',fixture],env=environment,
                           check=True,capture_output=True,text=True)
            saved_child=subprocess.Popen([str(ROOT/'build/mora.exe')],stdin=input_file,
                                         stdout=output_file,stderr=output_file,
                                         env=environment,startupinfo=startup)
            try:
                deadline=time.monotonic()+6
                while time.monotonic()<deadline:
                    lines=[]
                    for row in range(36):
                        buffer=c.create_unicode_buffer(116)
                        read=w.DWORD()
                        assert k.ReadConsoleOutputCharacterW(conout,buffer,116,Coord(0,row),c.byref(read))
                        lines.append(buffer[:read.value])
                    if '这台电脑保存过网易云音乐登录' in ''.join(lines):
                        checks.append('saved account requires explicit choice')
                        break
                    time.sleep(.1)
                else: raise AssertionError('saved account confirmation was not shown')
                send(ord('D'),'d')
                deadline=time.monotonic()+6
                while time.monotonic()<deadline and (Path(isolated)/'session.bin').exists():
                    time.sleep(.1)
                assert not (Path(isolated)/'session.bin').exists(), 'local account file not cleared'
                checks.append('forget saved account removes local session')
                send(27);time.sleep(.15)
                command('/quit')
                saved_child.wait(timeout=5)
                assert saved_child.returncode==0
            finally:
                if saved_child.poll() is None: saved_child.terminate();saved_child.wait(timeout=5)
    report={'passed':True,'checks':checks,'count':len(checks),'realWindowsConsoleInput':True}
    (ROOT/'build/ui-smoke.json').write_text(json.dumps(report,indent=2,ensure_ascii=False)+'\n',encoding='utf-8')
    print(json.dumps(report,ensure_ascii=True))
finally:
    k.CloseHandle(conin);k.CloseHandle(conout)
    if allocated:k.FreeConsole()
