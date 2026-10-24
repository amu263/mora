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
class ScreenBufferInfo(c.Structure): _fields_ = [('size',Coord),('cursor',Coord),('attributes',w.WORD),('window',Rect),('maximum',Coord)]
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
k.GetConsoleScreenBufferInfo.argtypes=[w.HANDLE,c.POINTER(ScreenBufferInfo)]
k.GetConsoleScreenBufferInfo.restype=w.BOOL
k.FlushConsoleInputBuffer.argtypes=[w.HANDLE]
k.FlushConsoleInputBuffer.restype=w.BOOL
k.FreeConsole.argtypes=[]
k.FreeConsole.restype=w.BOOL
k.AllocConsole.argtypes=[]
k.AllocConsole.restype=w.BOOL

# Detach from the console this script was started in and allocate a private one. A shared host
# console (Windows Terminal / a shell that queries the terminal for its capabilities) injects
# DECRPM/DA replies into the input buffer, which a TUI reads as keystrokes and types into its
# composer. A private console keeps the real-input injection path honest.
k.FreeConsole()
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

def flush():
    """Drop console-generated replies (DA1/DECRPM and friends) before injecting our own keys."""
    k.FlushConsoleInputBuffer(conin)

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
    # Deliberately no flush here: some cases build one logical edit out of type_text + raw
    # send(), and flushing between them would drop keys the player has not polled yet.
    for char in text:send(0,char)

def command(text):
    for char in text:send(0,char)
    send(13)

def console_rows(width=116,height=36):
    """Read back the visible console cells, the same way a listener would see them."""
    rows=[]
    for row in range(height):
        buffer=c.create_unicode_buffer(width)
        read=w.DWORD()
        assert k.ReadConsoleOutputCharacterW(conout,buffer,width,Coord(0,row),c.byref(read))
        rows.append(buffer[:read.value])
    return rows

def window_size():
    """Actual console window size and origin; a silently failed resize would invalidate a test."""
    info=ScreenBufferInfo()
    k.GetConsoleScreenBufferInfo(conout,c.byref(info))
    return (info.window.Right-info.window.Left+1, info.window.Bottom-info.window.Top+1,
            info.window.Left, info.window.Top)

def resize(width,height,attempts=40):
    target=(width,height)
    for _ in range(attempts):
        k.SetConsoleWindowInfo(conout,True,c.byref(Rect(0,0,width-1,height-1)))
        if window_size()[:2]==target: return
        time.sleep(.1)
    raise AssertionError(f'console did not resize to {width}x{height}: {window_size()}')

checks=[]
flush()
def expect(text,label,width=116,height=36,path=None):
    """Wait for a rendered frame (snapshot file) containing `text`."""
    target=path or snapshot
    deadline=time.monotonic()+4
    while time.monotonic()<deadline:
        try: frame=target.read_text(encoding='utf-8')
        except (OSError,UnicodeDecodeError): frame=''
        if text in frame: checks.append(label);return
        time.sleep(.1)
    raise AssertionError(f'{label}: expected {text!r}\n{frame}')

def expect_console(text,label,width=116,height=36):
    """Wait for a rendered frame read straight from the shared console buffer."""
    deadline=time.monotonic()+8
    while time.monotonic()<deadline:
        if text in ''.join(console_rows(width,height)): checks.append(label);return
        time.sleep(.1)
    raise AssertionError(f'{label}: expected {text!r}\n'+'\n'.join(console_rows(width,height)))

startup=subprocess.STARTUPINFO()
startup.dwFlags=subprocess.STARTF_USESHOWWINDOW
startup.wShowWindow=0
try:
    with open('CONIN$','rb',buffering=0) as input_file,open('CONOUT$','wb',buffering=0) as output_file:
        child=subprocess.Popen([str(ROOT/'build/mora.exe'),'--demo','--seconds','90','--snapshot',str(snapshot)],stdin=input_file,stdout=output_file,stderr=output_file,startupinfo=startup)
        flush()
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

            # --- Home: now-playing card with cover and a browse entry -------------------
            expect('♪ 七里香','home now-playing title')
            expect('周杰伦 · 七里香','home now-playing artist and album')
            expect('04:29','home now-playing duration')
            expect('极高 320K','home now-playing quality label')
            expect('顺序','home shows the queue mode')
            expect('浏览','home browse entry row')
            expect('D 详情','home detail entry')

            # --- the advertised D shortcut works from the default focus ------------------
            # Character-only key events (vk 0), the same transport the round-9 audit uses.
            send(0,'D')
            expect('D 关闭','home D key opens the detail page from the default focus')
            send(27);time.sleep(.2)
            expect('♪ 七里香','Esc leaves the detail page back to the homepage')
            # A non-empty composer keeps D as an ordinary character instead of a shortcut.
            send(0,'/')
            expect('❯ /','composer accepts a plain slash')
            send(0,'D')
            time.sleep(.4)
            frame=snapshot.read_text(encoding='utf-8')
            assert 'D 关闭' not in frame, \
                'D opened the detail page although the composer had text in it\n' + \
                '\n'.join([line for line in frame.split('\n') if '❯' in line or '详情' in line])
            checks.append('D stays a character while the composer has text')
            send(27);time.sleep(.2)
            expect('❯','composer cleared again')

            send(ord('1'),'1')
            expect('离线界面演示','home digit opens the browse page')
            expect('♪ 七里香','route page now-playing title')
            expect('极高 320K','route page now-playing quality')
            command('/home')
            expect('阿木','back to home')
            click(12,22)
            expect('离线界面演示','mouse opens browse page from the home entry')

            # --- Queue page: entries, current row, mode ---------------------------------
            send(53)
            expect('队列 8 首','queue page summary with entries')
            expect('顺序','queue page shows the mode')
            expect('七里香','queue page lists the current track')
            rows=console_rows()
            assert any('▸' in line and '七里香' in line for line in rows), \
                f'queue page does not mark the current row\n'+'\n'.join(rows)
            checks.append('queue page marks the current row')

            # --- Detail page: cover, metadata, truthful lyrics state, Esc returns --------
            send(0,'D')
            expect('D 关闭','queue page D key opens the detail page (character-only event)')
            expect('时长 04:29','detail page metadata includes duration')
            expect('歌词 · ','detail page has a lyrics section')
            frame=snapshot.read_text(encoding='utf-8')
            assert any(marker in frame for marker in ('歌词尚未加载','歌词暂不可用','纯音乐','当前行高亮')), \
                'detail page did not report a truthful lyrics state'
            checks.append('detail page reports its lyrics state truthfully')
            send(27);time.sleep(.2)
            expect('队列 8 首','Esc returns to the page the detail view came from')
            send(ord('D'),'d')
            expect('D 关闭','queue page D key also works for vk-coded events')
            send(27);time.sleep(.2)
            expect('队列 8 首','Esc returns again from the detail page')

            # --- Queue modes: /mode, /repeat compatibility, control button --------------
            command('/mode shuffle')
            expect('随机','/mode shuffle switches to shuffle')
            command('/mode')
            expect('顺序','/mode without arguments cycles the mode')
            command('/mode repeat_one')
            expect('单曲循环','/mode accepts repeat_one')
            command('/mode repeat_all')
            expect('列表循环','/mode accepts the canonical repeat_all name')
            command('/repeat on')
            expect('列表循环','/repeat on maps to repeat_all')
            command('/repeat off')
            expect('顺序','/repeat off maps to sequential')
            command('/mode sequential')
            expect('顺序','/mode back to sequential')

            # --- Persona-specific waiting lines -----------------------------------------
            command('/persona')
            expect('等待语：','persona picker previews the waiting line')
            expect('正在翻唱片架','amu waiting line')
            send(40)
            expect('别急，我这就去扒拉两首','fox waiting line')
            send(27);time.sleep(.15)

            # --- Voluntary reaction settings --------------------------------------------
            command('/settings')
            expect('自发评价：已关闭','reaction is off by default')
            expect('评价间隔：45 秒','reaction gap is shown')
            expect('评价字数：25 字','reaction length is shown')
            send(ord('R'),'r')
            expect('自发评价已开启','R turns the reaction on')
            send(ord(']'),']')
            expect('评价间隔：50 秒','gap adjustable in the settings panel')
            send(ord('-'),'-')
            expect('评价字数：20 字','length adjustable in the settings panel')
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
            expect('S 或 Esc 跳过','guide offers a skip hint')
            send(ord('S'),'s')

            command('推荐一首轻音乐')
            expect('演示模式 · 模型请求请退出后运行 mora','demo chat boundary')
            command('/quit')
            child.wait(timeout=5)
            assert child.returncode==0
        finally:
            if child.poll() is None: child.terminate();child.wait(timeout=5)
        # --- 78x24 layout: start a player in a console that is already at the minimum --------
        # A live window resize is a no-op while the player owns the VT alternate screen buffer, so
        # the compact layout is measured by starting a fresh player in an already-small console.
        resize(78,24)
        small_snapshot=ROOT/'build/ui-smoke-small.txt'
        small_snapshot.unlink(missing_ok=True)
        small_child=subprocess.Popen([str(ROOT/'build/mora.exe'),'--demo','--seconds','25','--snapshot',str(small_snapshot)],stdin=input_file,stdout=output_file,stderr=output_file,startupinfo=startup)
        flush()
        try:
            expect('阿木','78x24 home shows the persona line',path=small_snapshot)
            expect('♪ 晴天','78x24 home shows the now-playing card',path=small_snapshot)
            expect('浏览','78x24 home keeps the browse entry',path=small_snapshot)
            expect('Enter 发送','78x24 home keeps the composer hint',path=small_snapshot)
            small=console_rows(78,24)
            def rows_with(needle): return [i for i,line in enumerate(small) if needle in line]
            title_rows=rows_with('♪ 晴天')
            persona_rows=rows_with('小阿木')
            browse_rows=rows_with('浏览')
            assert title_rows and persona_rows and browse_rows, \
                'expected home regions missing at 78x24\n'+'\n'.join(small)
            assert all('小阿木' not in small[i] for i in title_rows), 'now playing overlaps the persona line'
            assert all('♪ 晴天' not in small[i] for i in persona_rows), 'persona line overlaps the now playing card'
            assert min(title_rows) > max(persona_rows), 'now playing card is not below the persona block'
            assert min(title_rows) > max(browse_rows), 'now playing card is not below the browse row'
            assert all(len(line)<=78 for line in small), 'a row overflowed the 78 column window'
            checks.append('78x24 home layout keeps regions separate')
            command('/queue')
            expect('队列 8 首','78x24 queue page keeps its summary',path=small_snapshot)
            expect('顺序','78x24 queue page keeps its mode',path=small_snapshot)
            command('/guide')
            expect('S 或 Esc 跳过','guide fits the minimum terminal',path=small_snapshot)
            send(27);time.sleep(.2)
            command('/quit')
            small_child.wait(timeout=5)
            assert small_child.returncode==0
        finally:
            if small_child.poll() is None: small_child.terminate();small_child.wait(timeout=5)
        resize(116,36)
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
            flush()
            try:
                expect_console('这台电脑保存过网易云音乐登录','saved account requires explicit choice')
                send(ord('D'),'d')
                deadline=time.monotonic()+6
                while time.monotonic()<deadline and (Path(isolated)/'session.bin').exists():
                    time.sleep(.1)
                assert not (Path(isolated)/'session.bin').exists(), 'local account file not cleared'
                checks.append('forget saved account removes local session')
                # The first-run name prompt follows the local logout; then the guide can be skipped.
                expect_console('怎么称呼你？','first run asks for the user name')
                type_text('smoke');send(13)
                expect_console('新手引导 · 1 / 3','guide follows the name prompt')
                send(ord('S'),'s')
                # --- Empty states without any account or playback -----------------------
                expect_console('还没有正在播放的歌曲','home placeholder without playback')
                command('/queue')
                expect_console('队列里还没有歌曲','empty queue tells the truth instead of showing blanks')
                expect_console('队列 0 首','empty queue summary shows zero entries')
                command('/mode shuffle')
                expect_console('随机','/mode shuffle works with an empty queue')
                command('/queue')
                expect_console('队列 0 首 · 随机','empty queue also reports the current mode')
                command('/detail')
                expect_console('歌曲详情','detail page is reachable without playback')
                expect_console('还没有正在播放的歌曲','detail page placeholder without playback')
                send(27);time.sleep(.2)
                command('/settings')
                expect_console('自发评价：已关闭','reaction stays off by default')
                send(ord('R'),'r')
                expect_console('自发评价已开启','reaction setting toggles in the panel')
                send(ord(']'),']')
                expect_console('评价间隔：50 秒','reaction gap updated')
                send(ord('-'),'-')
                expect_console('评价字数：20 字','reaction length updated')
                send(27);time.sleep(.2)
                # --- Stats, review and the model catalogue stay honest without data ---------
                command('/models')
                expect_console('还没有缓存的模型目录','/models reports an empty cache instead of inventing models')
                command('/stats')
                expect_console('听歌统计 · 日','stats panel opens for the default period')
                expect_console('这个周期还没有听歌记录','stats reports an empty period truthfully')
                send(27);time.sleep(.2)
                command('/review')
                expect_console('还没有听歌记录，先播放几首再来写锐评','review refuses to invent a comment without data')
                command('/ai')
                expect_console('从端点发现模型','/ai exposes endpoint model discovery')
                expect_console('查看已缓存的模型目录','/ai exposes the cached model catalogue')
                send(27);time.sleep(.2)
                # --- A chat turn with no key must never fabricate a compression note --------
                command('你好')
                expect_console('模型请求失败','chat without a model key fails honestly')
                assert '已压缩' not in ''.join(console_rows()), \
                    'a compression note was shown although nothing was compressed'
                checks.append('no compression note while nothing was compressed')
                command('/home')
                expect_console('还没有正在播放的歌曲','home after the failed chat turn')
                command('/quit')
                saved_child.wait(timeout=5)
                assert saved_child.returncode==0
                stored=json.loads((Path(isolated)/'settings.json').read_text(encoding='utf-8'))
                assert stored.get('queueMode')=='shuffle', f'queue mode not persisted: {stored}'
                assert stored.get('reactEnabled') is True, f'reaction flag not persisted: {stored}'
                assert stored.get('reactMinSeconds')==50, f'reaction gap not persisted: {stored}'
                assert stored.get('reactMaxChars')==20, f'reaction length not persisted: {stored}'
                checks.append('queue mode and reaction settings survive a restart')
            finally:
                if saved_child.poll() is None: saved_child.terminate();saved_child.wait(timeout=5)
    report={'passed':True,'checks':checks,'count':len(checks),'realWindowsConsoleInput':True}
    (ROOT/'build/ui-smoke.json').write_text(json.dumps(report,indent=2,ensure_ascii=False)+'\n',encoding='utf-8')
    print(json.dumps(report,ensure_ascii=True))
finally:
    k.CloseHandle(conin);k.CloseHandle(conout)
    if allocated:k.FreeConsole()
