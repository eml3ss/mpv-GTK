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
source=tmp/'Unicode Ω video.mkv'
subtitles=tmp/'captions.srt'
subtitles.write_text('1\n00:00:00,000 --> 00:00:04,000\nTrim export subtitle\n')
subprocess.run(['ffmpeg','-nostdin','-v','error',
    '-f','lavfi','-i','testsrc2=size=160x90:rate=25:duration=4',
    '-f','lavfi','-i','sine=frequency=440:sample_rate=48000:duration=4',
    '-f','lavfi','-i','sine=frequency=880:sample_rate=48000:duration=4',
    '-i',str(subtitles),'-map','0:v','-map','1:a','-map','2:a','-map','3:s',
    '-c:v','libx264','-preset','ultrafast','-g','25','-keyint_min','25',
    '-sc_threshold','0','-threads','1','-c:a','pcm_s16le','-c:s','srt',
    '-metadata:s:a:0','language=eng','-metadata:s:a:0','title=English original',
    '-metadata:s:a:1','language=rus','-metadata:s:a:1','title=Russian commentary',
    '-metadata:s:s:0','language=eng','-shortest',str(source)],check=True)
external=tmp/'external.flac'
subprocess.run(['ffmpeg','-nostdin','-v','error','-f','lavfi','-i',
    'sine=frequency=220:sample_rate=48000:duration=4','-c:a','flac',str(external)],check=True)
before=hashlib.sha256(source.read_bytes()).hexdigest()
(tmp/'script-opts').mkdir()
# A transparent tool wrapper permits deterministic failures without touching
# anything outside this generated fixture directory.
wrapper=tmp/'ffmpeg-wrapper'
wrapper.write_text(
    '#!/usr/bin/env python3\n'
    'from pathlib import Path\nimport subprocess, sys, time\n'
    'root=Path(__file__).parent\n'
    'if (root/"fail-tool").exists():sys.exit(23)\n'
    f'code=subprocess.run([{shutil.which("ffmpeg")!r},*sys.argv[1:]]).returncode\n'
    'if code==0 and "copy" in sys.argv and (root/"hold-export").exists():\n'
    ' (root/"export-ready").write_text("ready")\n'
    ' deadline=time.monotonic()+8\n'
    ' while not (root/"release-export").exists() and time.monotonic()<deadline:time.sleep(.01)\n'
    'sys.exit(code)\n')
wrapper.chmod(0o700)
(tmp/'script-opts/trim-mode.conf').write_text(
    f'remember=no\nopen-copy=no\nopen-replace=yes\nffmpeg_path={wrapper}\n')
ipc=tmp/'ipc.sock'
(tmp/'mpv.conf').write_text(
    f'hwdec=no\nao=null\npause=yes\ninput-ipc-server={ipc}\n'
    'load-scripts=no\nosc=no\nytdl=no\nload-stats-overlay=no\nload-console=no\n'
    'load-auto-profiles=no\nload-select=no\nload-positioning=no\nload-commands=no\nload-context-menu=no\n')
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
    def audio_state(count=None):
        return wait(lambda:prop('user-data/trim-audio'),
            lambda x:x and (count is None or x.get('selected_count')==count))
    def audio_all(include):
        state=audio_state()
        action('audio-all','yes' if include else 'no',str(state['file_serial']))
        return audio_state(state['total_count'] if include else 0)
    def audio_one(track):
        state=audio_all(False)
        action('set-audio',str(track['ff_index']),'yes',str(state['file_serial']))
        return audio_state(1)
    def assert_streams(path, languages):
        result=json.loads(subprocess.check_output(['ffprobe','-v','error','-show_streams',
            '-of','json',str(path)],text=True))['streams']
        assert sum(x['codec_type']=='video' for x in result)==1,result
        assert sum(x['codec_type']=='subtitle' for x in result)==1,result
        audio=[x for x in result if x['codec_type']=='audio']
        assert [x.get('tags',{}).get('language') for x in audio]==languages,audio
        assert all(x['codec_name']=='pcm_s16le' for x in audio),audio
    wait(lambda:prop('time-pos'),lambda x:x is not None)
    call('set_property','pause',True)
    wait(lambda:prop('pause'))
    assert prop('user-data/trim-mode') is None, 'Backend loaded before first use'
    call('script-message','mpv-gtk','trim')
    state=wait(lambda:prop('user-data/trim-mode'),lambda x:x and x.get('enabled') and len(x.get('keyframes',[]))>=4)
    controls=wait(lambda:prop('user-data/trim-controls'),lambda x:x and x.get('available'))
    assert controls['start_ms']==0 and controls['end_ms']==4000,controls
    print('PASS native activation and keyframes with all Lua scripts disabled',flush=True)
    audio=audio_state(2)
    assert audio['total_count']==2 and len(audio['tracks'])==2,audio
    assert all(x['included'] for x in audio['tracks']),audio
    assert {x['lang'] for x in audio['tracks']}=={'eng','rus'},audio
    assert len({x['ff_index'] for x in audio['tracks']})==2,audio
    russian=next(x for x in audio['tracks'] if x['lang']=='rus')
    assert russian['title']=='Russian commentary' and russian['codec']=='pcm_s16le',russian
    selected_aid=prop('aid')
    audio_one(russian)
    assert prop('aid')==selected_aid
    call('script-message','mpv-gtk','trim')
    wait(lambda:prop('user-data/trim-mode'),lambda x:x and not x.get('enabled'))
    call('script-message','mpv-gtk','trim')
    wait(lambda:prop('user-data/trim-mode'),lambda x:x and x.get('enabled'))
    assert audio_state()['selected_count']==1
    # Playback selection and external-track catalog updates must not replace
    # the independent export selection or add an external file to stream maps.
    call('set_property','aid',russian['id'])
    wait(lambda:prop('aid'),lambda x:x==russian['id'])
    assert audio_state()['selected_count']==1
    call('audio-add',str(external),'auto')
    tracks=wait(lambda:prop('track-list'),lambda x:x and any(t.get('external') for t in x))
    time.sleep(.05)
    assert audio_state()['total_count']==2 and audio_state()['selected_count']==1
    call('audio-remove',str(next(t['id'] for t in tracks if t.get('external'))))
    wait(lambda:prop('track-list'),lambda x:x and not any(t.get('external') for t in x))
    stale=str(audio_state()['file_serial']+1)
    action('audio-all','yes',stale)
    time.sleep(.05)
    assert audio_state()['selected_count']==1
    print('PASS independent export audio selection, mode persistence, external-track filtering, and stale-action rejection',flush=True)
    call('seek',1.1,'absolute+exact')
    wait(lambda:prop('time-pos'),lambda x:x is not None and abs(x-1.1)<.05)
    call('keypress','i')
    wait(lambda:prop('user-data/trim-controls'),lambda x:x and abs(x['start_ms']-1100)<50)
    call('seek',1.2,'absolute+exact')
    wait(lambda:prop('time-pos'),lambda x:x is not None and abs(x-1.2)<.03)
    call('script-binding','trim-mode-set-start')
    wait(lambda:prop('user-data/trim-controls'),lambda x:x and abs(x['start_ms']-1200)<30)
    action('keyframe','previous')
    wait(lambda:prop('time-pos'),lambda x:x is not None and abs(x-1)<.03)
    action('keyframe','next')
    wait(lambda:prop('time-pos'),lambda x:x is not None and abs(x-2)<.03)
    print('PASS scoped keys, legacy binding names, and previous/next keyframes',flush=True)
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
    assert_streams(copy,['rus'])
    assert hashlib.sha256(source.read_bytes()).hexdigest()==before
    print('PASS selected-audio copy export; video/subtitles retained and source bytes unchanged',flush=True)
    copy_hash=hashlib.sha256(copy.read_bytes()).hexdigest()
    audio_all(False)
    action('export','keep')
    wait(lambda:prop('user-data/trim-export'),lambda x:x and x.get('busy'))
    result=wait(lambda:prop('user-data/trim-export'),lambda x:x and not x.get('busy') and 'success' in x,seconds=12)
    assert result['success'],result
    assert_streams(Path(result['path']),[])
    assert Path(result['path'])!=copy
    assert hashlib.sha256(copy.read_bytes()).hexdigest()==copy_hash
    assert hashlib.sha256(source.read_bytes()).hexdigest()==before
    print('PASS silent copy export; existing copy and original remain unchanged',flush=True)
    audio_all(True)
    action('export','keep')
    wait(lambda:prop('user-data/trim-export'),lambda x:x and x.get('busy'))
    result=wait(lambda:prop('user-data/trim-export'),lambda x:x and not x.get('busy') and 'success' in x,seconds=12)
    assert result['success'],result
    assert_streams(Path(result['path']),['eng','rus'])
    assert hashlib.sha256(source.read_bytes()).hexdigest()==before
    print('PASS all-audio copy export',flush=True)
    audio_one(russian)
    action('export','replace')
    wait(lambda:prop('user-data/trim-export'),lambda x:x and x.get('busy'))
    result=wait(lambda:prop('user-data/trim-export'),lambda x:x and not x.get('busy') and 'success' in x,seconds=12)
    assert result['success'],result
    assert_streams(source,['rus'])
    assert hashlib.sha256(source.read_bytes()).hexdigest()!=before
    wait(lambda:prop('duration'),lambda x:x and 1.8<x<2.3)
    assert Path(prop('path'))==source
    assert prop('pause') is True
    reset_audio=audio_state(1)
    assert reset_audio['total_count']==1 and reset_audio['file_serial']!=audio['file_serial'],reset_audio
    assert all(x['included'] for x in reset_audio['tracks']),reset_audio
    action('audio-all','no',str(audio['file_serial']))
    time.sleep(.05)
    assert audio_state()['selected_count']==1
    print('PASS replacement export, verification, reopening, and paused state',flush=True)
    call('script-message','mpv-gtk','trim')
    wait(lambda:prop('user-data/trim-mode'),lambda x:x and not x.get('enabled'))
    print('PASS return to player mode',flush=True)
    call('script-message','mpv-gtk','trim')
    wait(lambda:prop('user-data/trim-mode'),lambda x:x and x.get('enabled'))
    replacement_bytes=source.read_bytes()
    (tmp/'fail-tool').touch()
    action('export','replace')
    result=wait(lambda:prop('user-data/trim-export'),lambda x:x and not x.get('busy') and x.get('success') is False)
    assert not result['success'],result
    assert source.read_bytes()==replacement_bytes
    (tmp/'fail-tool').unlink()
    print('PASS failed FFmpeg never replaces the source',flush=True)
    (tmp/'hold-export').touch()
    action('export','replace')
    wait(lambda:(tmp/'export-ready').exists())
    modified_bytes=source.read_bytes()+b'\0Native trim concurrent edit\n'
    source.write_bytes(modified_bytes)
    (tmp/'release-export').touch()
    result=wait(lambda:prop('user-data/trim-export'),lambda x:x and not x.get('busy') and 'success' in x)
    assert not result['success'],result
    assert 'changed' in result.get('error','').lower(),result
    assert source.read_bytes()==modified_bytes
    print('PASS concurrent source changes are preserved and replacement refused',flush=True)
    (tmp/'export-ready').unlink()
    (tmp/'release-export').unlink()
    action('export','replace')
    wait(lambda:(tmp/'export-ready').exists())
    call('quit')
    process.wait(timeout=5)
    assert process.returncode==0,process.returncode
    assert source.read_bytes()==modified_bytes
    print('PASS shutdown during export preserves source bytes',flush=True)
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
