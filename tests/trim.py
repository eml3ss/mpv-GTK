#!/usr/bin/env python3
"""Exercise lazy GTK Trim Mode and real copy/replacement exports on scratch media.

Requires a usable native Wayland display and FFmpeg. Run explicitly with
make test-trim; this never reads or modifies the user's player configuration.
"""
from pathlib import Path
import hashlib, json, os, shutil, socket, subprocess, tempfile, time
if not os.environ.get('WAYLAND_DISPLAY') or not shutil.which('ffmpeg') or not shutil.which('ffprobe'):
    print('SKIP: native Wayland and FFmpeg are required.')
    raise SystemExit(77)
root=Path(__file__).resolve().parent.parent
tmp=Path(tempfile.mkdtemp(prefix='mpv-gtk-trim-check-'))
print('Fixture:',tmp,flush=True)
source=tmp/'Unicode Ω video.mp4'
subprocess.run(['ffmpeg','-nostdin','-v','error','-f','lavfi','-i','testsrc2=size=160x90:rate=25:duration=4','-f','lavfi','-i','sine=sample_rate=48000:duration=4','-c:v','libx264','-preset','ultrafast','-g','25','-keyint_min','25','-sc_threshold','0','-threads','1','-c:a','aac','-shortest',str(source)],check=True)
before=hashlib.sha256(source.read_bytes()).hexdigest()
(tmp/'script-opts').mkdir()
(tmp/'script-opts/trim-mode.conf').write_text('remember=no\nopen-copy=no\nopen-replace=yes\n')
ipc=tmp/'ipc.sock'
(tmp/'mpv.conf').write_text(f'hwdec=no\nao=null\npause=yes\ninput-ipc-server={ipc}\n')
(tmp/'input.conf').write_text('')
(tmp/'interface.conf').write_text('[Interface]\nlanguage=en\n')
env=os.environ.copy();env['MPV_GTK_CONFIG_FILE']=str(tmp/'mpv.conf')
log=open(tmp/'player.log','w')
process=subprocess.Popen([os.environ.get('MPV_GTK_BINARY',str(root/'build/mpv-gtk')),str(source)],env=env,stdout=log,stderr=log)
client=None
success=False
try:
    deadline=time.monotonic()+12
    while not ipc.exists():
        if process.poll() == 77: raise SystemExit(77)
        if process.poll() is not None: raise RuntimeError('Player exited: '+(tmp/'player.log').read_text())
        if time.monotonic()>deadline: raise RuntimeError('IPC startup timeout')
        time.sleep(.02)
    client=socket.socket(socket.AF_UNIX);client.settimeout(10);client.connect(str(ipc));stream=client.makefile('rwb',buffering=0)
    serial=0
    def call(*args, optional=False):
        global serial
        serial+=1;rid=serial
        stream.write((json.dumps({'command':list(args),'request_id':rid})+'\n').encode())
        while True:
            line=stream.readline()
            if not line: raise RuntimeError('IPC closed')
            result=json.loads(line)
            if result.get('request_id')!=rid: continue
            if result.get('error')!='success':
                if optional:return None
                raise RuntimeError(str(args)+': '+str(result))
            return result.get('data')
    def wait(get, valid=lambda x:bool(x), seconds=10):
        end=time.monotonic()+seconds
        while time.monotonic()<end:
            value=get()
            if valid(value):return value
            if process.poll() is not None:raise RuntimeError('Player exited')
            time.sleep(.025)
        raise RuntimeError('Timeout: '+str(value)+'\n'+(tmp/'player.log').read_text())
    def prop(name):return call('get_property',name,optional=True)
    def action(name,*args):return call('script-message-to','trim_mode',name,*args)
    wait(lambda:prop('time-pos'),lambda x:x is not None)
    call('set_property','pause',True)
    wait(lambda:prop('pause'))
    assert prop('user-data/trim-mode') is None, 'Backend loaded before first use'
    call('script-message','mpv-gtk','trim')
    state=wait(lambda:prop('user-data/trim-mode'),lambda x:x and x.get('enabled') and len(x.get('keyframes',[]))>=4)
    controls=wait(lambda:prop('user-data/trim-controls'),lambda x:x and x.get('available'))
    assert controls['start_ms']==0 and controls['end_ms']==4000,controls
    print('PASS lazy GTK activation, keyframe scan, and initial range',flush=True)
    call('seek',1.1,'absolute+exact')
    wait(lambda:prop('time-pos'),lambda x:x is not None and abs(x-1.1)<.05)
    call('keypress','i')
    wait(lambda:prop('user-data/trim-controls'),lambda x:x and abs(x['start_ms']-1100)<50)
    action('keyframe','previous')
    wait(lambda:prop('time-pos'),lambda x:x is not None and abs(x-1)<.03)
    action('keyframe','next')
    wait(lambda:prop('time-pos'),lambda x:x is not None and abs(x-2)<.03)
    print('PASS scoped I shortcut and previous/next keyframes',flush=True)
    action('set-time','start','00:00:01.000','1')
    result=wait(lambda:prop('user-data/trim-field-result'),lambda x:x and x.get('serial')=='1');assert result['valid'],result
    action('set-time','end','3.000','2')
    result=wait(lambda:prop('user-data/trim-field-result'),lambda x:x and x.get('serial')=='2');assert result['valid'],result
    action('set-time','end','00:00:00.500','3')
    result=wait(lambda:prop('user-data/trim-field-result'),lambda x:x and x.get('serial')=='3');assert not result['valid'],result
    assert prop('user-data/trim-controls')['end_ms']==3000
    print('PASS millisecond editing and invalid interval rejection',flush=True)
    action('export','keep')
    result=wait(lambda:prop('user-data/trim-export'),lambda x:x and 'success' in x,seconds=12)
    assert result['success'],result
    copy=Path(result['path']);assert copy.exists(),result
    assert hashlib.sha256(source.read_bytes()).hexdigest()==before
    print('PASS copy export; source bytes unchanged',flush=True)
    action('export','replace')
    wait(lambda:prop('user-data/trim-export'),lambda x:x and x.get('busy'))
    result=wait(lambda:prop('user-data/trim-export'),lambda x:x and not x.get('busy') and 'success' in x,seconds=12)
    assert result['success'],result
    assert hashlib.sha256(source.read_bytes()).hexdigest()!=before
    wait(lambda:prop('duration'),lambda x:x and 1.8<x<2.3)
    assert Path(prop('path'))==source
    assert prop('pause') is True
    print('PASS replacement export, verification, reopening, and paused state',flush=True)
    call('script-message','mpv-gtk','trim')
    wait(lambda:prop('user-data/trim-mode'),lambda x:x and not x.get('enabled'))
    print('PASS return to player mode',flush=True)
    call('quit')
    process.wait(timeout=5)
    assert process.returncode==0,process.returncode
    output=(tmp/'player.log').read_text()
    assert 'CRITICAL' not in output and 'Lua error' not in output,output
    print('PASS clean shutdown; no GTK criticals or Lua errors',flush=True)
    success=True
except Exception:
    print((tmp/'player.log').read_text(),flush=True)
    raise
finally:
    if process.poll() is None:
        process.terminate()
        try:process.wait(timeout=4)
        except subprocess.TimeoutExpired:process.kill();process.wait()
    if client:client.close()
    log.close()
    if success:shutil.rmtree(tmp)
