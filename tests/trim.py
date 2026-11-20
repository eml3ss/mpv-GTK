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
second=tmp/'Second clip.mkv'
shutil.copy(source,second)
timecoded=tmp/'timecode.mp4'
subprocess.run(['ffmpeg','-nostdin','-v','error',
    '-f','lavfi','-i','testsrc2=size=160x90:rate=25:duration=4',
    '-f','lavfi','-i','sine=frequency=440:sample_rate=48000:duration=4',
    '-c:v','libx264','-preset','ultrafast','-g','25','-threads','1','-c:a','aac',
    '-timecode','01:00:00:00','-shortest',str(timecoded)],check=True)
def clip(path,codec,extra):
    subprocess.run(['ffmpeg','-nostdin','-v','error',
        '-f','lavfi','-i','testsrc2=size=320x180:rate=25:duration=6',
        '-f','lavfi','-i','sine=frequency=440:sample_rate=48000:duration=6',
        '-c:v',codec,*extra,'-threads','1','-c:a','aac','-shortest',str(path)],check=True)
# Keyframes every two seconds; x265 defaults to open GOPs with leading frames.
precise_h264=tmp/'precise h264.mp4'
clip(precise_h264,'libx264',['-preset','ultrafast','-g','50','-keyint_min','50','-sc_threshold','0','-bf','2'])
precise_hevc=tmp/'precise hevc.mp4'
clip(precise_hevc,'libx265',['-preset','ultrafast','-pix_fmt','yuv420p10le','-tag:v','hvc1',
    '-x265-params','keyint=50:min-keyint=50:scenecut=0:log-level=error'])
precise_mpeg4=tmp/'precise mpeg4.mkv'
clip(precise_mpeg4,'mpeg4',['-g','50'])
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
    state=audio_state()
    assert state['video_included'] is True,state
    action('set-video','no',str(state['file_serial']))
    wait(lambda:prop('user-data/trim-audio'),lambda x:x and x.get('video_included') is False)
    action('export','keep')
    wait(lambda:prop('user-data/trim-export'),lambda x:x and x.get('busy'))
    result=wait(lambda:prop('user-data/trim-export'),lambda x:x and not x.get('busy') and 'success' in x,seconds=12)
    assert result['success'],result
    streams=json.loads(subprocess.check_output(['ffprobe','-v','error','-show_streams',
        '-of','json',result['path']],text=True))['streams']
    assert not any(x['codec_type']=='video' for x in streams),streams
    assert sum(x['codec_type']=='audio' for x in streams)==2,streams
    assert not any(x['codec_type']=='subtitle' for x in streams),streams
    assert Path(result['path']).suffix=='.mka',result
    assert hashlib.sha256(source.read_bytes()).hexdigest()==before
    audio_one(russian)
    action('export','keep')
    wait(lambda:prop('user-data/trim-export'),lambda x:x and x.get('busy'))
    result=wait(lambda:prop('user-data/trim-export'),lambda x:x and not x.get('busy') and 'success' in x,seconds=12)
    assert result['success'],result
    assert Path(result['path']).suffix=='.wav',result
    streams=json.loads(subprocess.check_output(['ffprobe','-v','error','-show_streams',
        '-of','json',result['path']],text=True))['streams']
    assert [x['codec_type'] for x in streams]==['audio'],streams
    audio_all(False)
    action('export','keep')
    result=wait(lambda:prop('user-data/trim-export'),lambda x:x and x.get('success') is False)
    assert 'Select video' in result['error'],result
    action('set-video','yes',str(state['file_serial']))
    wait(lambda:prop('user-data/trim-audio'),lambda x:x and x.get('video_included') is True)
    audio_all(True)
    print('PASS video-free copy export and empty-selection rejection',flush=True)
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
    previous=audio_state()['file_serial']
    call('loadfile',str(second))
    audio=wait(lambda:prop('user-data/trim-audio'),
        lambda x:x and x['file_serial']!=previous and x['total_count']==2)
    wait(lambda:prop('user-data/trim-controls'),lambda x:x and x.get('available'))
    audio_one(next(x for x in audio['tracks'] if x['lang']=='eng'))
    action('set-video','no',str(audio['file_serial']))
    wait(lambda:prop('user-data/trim-audio'),lambda x:x and x.get('video_included') is False)
    action('export','replace')
    wait(lambda:prop('user-data/trim-export'),lambda x:x and x.get('busy'))
    result=wait(lambda:prop('user-data/trim-export'),lambda x:x and not x.get('busy') and 'success' in x,seconds=12)
    assert result['success'],result
    renamed=tmp/'Second clip.wav'
    assert Path(result['path'])==renamed and renamed.exists() and not second.exists(),result
    streams=json.loads(subprocess.check_output(['ffprobe','-v','error','-show_streams',
        '-of','json',str(renamed)],text=True))['streams']
    assert [x['codec_type'] for x in streams]==['audio'],streams
    wait(lambda:prop('path'),lambda x:x and Path(x)==renamed)
    assert [x['filename'] for x in prop('playlist')]==[str(renamed)],prop('playlist')
    assert not list(tmp.glob('.mpv-trim-*')),list(tmp.glob('.mpv-trim-*'))
    print('PASS audio-only replacement saves the source codec container and swaps the queue entry',flush=True)
    previous=audio_state()['file_serial']
    call('loadfile',str(timecoded))
    wait(lambda:prop('user-data/trim-audio'),lambda x:x and x['file_serial']!=previous and x['total_count']==1)
    wait(lambda:prop('user-data/trim-controls'),lambda x:x and x.get('available'))
    action('export','keep')
    wait(lambda:prop('user-data/trim-export'),lambda x:x and x.get('busy'))
    result=wait(lambda:prop('user-data/trim-export'),lambda x:x and not x.get('busy') and 'success' in x,seconds=12)
    assert result['success'],result
    streams=json.loads(subprocess.check_output(['ffprobe','-v','error','-show_streams',
        '-of','json',result['path']],text=True))['streams']
    assert [x['codec_type'] for x in streams if x['codec_type']!='data']==['video','audio'],streams
    assert streams[0].get('tags',{}).get('timecode')=='01:00:00:00',streams
    print('PASS MP4 timecode tracks are rebuilt instead of copied',flush=True)
    def frame_psnr(output,source,start,frames):
        # Per-frame PSNR of the export against the source from the exact start.
        text=subprocess.run(['ffmpeg','-v','error','-i',str(output),'-ss',str(start),'-i',str(source),
            '-lavfi','[0:v]setpts=PTS-STARTPTS,format=yuv420p[a];[1:v]setpts=PTS-STARTPTS,format=yuv420p[b];[a][b]psnr=stats_file=-',
            '-frames:v',str(frames),'-f','null','-'],capture_output=True,text=True,check=True).stdout
        return [float('inf') if 'inf' in x.split('psnr_avg:')[1].split()[0] else float(x.split('psnr_avg:')[1].split()[0])
            for x in text.splitlines() if 'psnr_avg:' in x]
    def precise_export(path,mode,start,end=None):
        previous=audio_state()['file_serial']
        call('loadfile',str(path))
        wait(lambda:prop('user-data/trim-audio'),lambda x:x and x['file_serial']!=previous)
        wait(lambda:prop('user-data/trim-mode'),lambda x:x and len(x.get('keyframes',[]))>=3)
        wait(lambda:prop('user-data/trim-controls'),lambda x:x and x.get('available'))
        action('set-precise-cut',mode)
        serial=str(time.monotonic_ns())
        action('set-time','start',start,serial)
        result=wait(lambda:prop('user-data/trim-field-result'),lambda x:x and x.get('serial')==serial);assert result['valid'],result
        if end:
            serial=str(time.monotonic_ns())
            action('set-time','end',end,serial)
            result=wait(lambda:prop('user-data/trim-field-result'),lambda x:x and x.get('serial')==serial);assert result['valid'],result
        action('export','keep')
        wait(lambda:prop('user-data/trim-export'),lambda x:x and x.get('busy'))
        result=wait(lambda:prop('user-data/trim-export'),lambda x:x and not x.get('busy') and 'success' in x,seconds=60)
        assert result['success'],result
        output=Path(result['path'])
        assert not subprocess.run(['ffmpeg','-v','error','-xerror','-i',str(output),'-f','null','-'],
            capture_output=True,text=True).stderr,output
        streams=json.loads(subprocess.check_output(['ffprobe','-v','error','-show_streams',
            '-of','json',str(output)],text=True))['streams']
        return result,output,streams
    # Start at frame 13, between the keyframes at 0 s and 2 s.
    result,output,streams=precise_export(precise_h264,'cpu','00:00:00.520')
    assert result['precise']=='cpu',result
    psnr=frame_psnr(output,precise_h264,0.52,80)
    assert len(psnr)>=80 and min(psnr[:37])>35 and all(x==float('inf') for x in psnr[37:80]),psnr
    starts={x['codec_type']:float(x['start_time']) for x in streams}
    assert abs(starts['video']-starts['audio'])<0.05,streams
    print('PASS precise CPU cut: re-encoded head from the exact frame, bit-exact copied tail, synchronized audio',flush=True)
    result,output,streams=precise_export(precise_hevc,'gpu','00:00:00.520')
    assert result['precise'] in ('gpu','cpu'),result
    psnr=frame_psnr(output,precise_hevc,0.52,80)
    assert len(psnr)>=80 and min(psnr[:37])>35 and all(x==float('inf') for x in psnr[37:80]),psnr
    print('PASS precise '+result['precise'].upper()+' cut of open-GOP 10-bit HEVC: leading frames replaced, no broken references',flush=True)
    def frame_count(path):
        return int(subprocess.check_output(['ffprobe','-v','error','-count_frames','-select_streams','v:0',
            '-show_entries','stream=nb_read_frames','-of','csv=p=0',str(path)],text=True).strip())
    # 0.52-4.70 s at 25 fps holds frames 13..117. Keyframes at 2 s and 4 s:
    # re-encoded head, copied middle, re-encoded ending.
    for path,mode in ((precise_h264,'cpu'),(precise_hevc,'gpu')):
        result,output,streams=precise_export(path,mode,'00:00:00.520','00:00:04.700')
        assert result['precise'] in ((mode,) if mode=='cpu' else ('gpu','cpu')),result
        assert frame_count(output)==105,(result,frame_count(output))
        psnr=frame_psnr(output,path,0.52,105)[:105]
        copied=[i for i,x in enumerate(psnr) if x==float('inf')]
        assert copied and copied==list(range(37,copied[-1]+1)) and len(copied)>=45,psnr
        assert min(x for x in psnr if x!=float('inf'))>35,psnr
        assert psnr[-1]>35,psnr
        print(f'PASS precise {result["precise"].upper()} start and end: {len(copied)} copied frames between re-encoded head and ending, exact frame count',flush=True)
    result,output,streams=precise_export(precise_h264,'off','00:00:00.520')
    assert result['precise']=='',result
    assert frame_psnr(output,precise_h264,0,1)==[float('inf')],result
    result,output,streams=precise_export(precise_mpeg4,'cpu','00:00:00.520')
    assert result['precise']=='fallback',result
    print('PASS keyframe exports when precise cutting is off or the codec is unsupported',flush=True)
    previous=audio_state()['file_serial']
    call('loadfile',str(source))
    wait(lambda:prop('user-data/trim-audio'),lambda x:x and x['file_serial']!=previous and x.get('video_included'))
    wait(lambda:prop('user-data/trim-controls'),lambda x:x and x.get('available'))
    wait(lambda:prop('user-data/trim-mode'),lambda x:x and x.get('enabled'))
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
