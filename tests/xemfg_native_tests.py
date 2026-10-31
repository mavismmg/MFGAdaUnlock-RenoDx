"""Run the native ReShade transport in a separate directory, never in a game."""
import argparse
import ctypes
import configparser
import datetime
import hashlib
import json
import re
from pathlib import Path
import shutil
import subprocess
import evidence

ap=argparse.ArgumentParser()
for name in ('directory','addon','exe','fixture','reshade','sdk-fg','sdk-ll'):
    ap.add_argument('--'+name,type=Path,required=True)
ap.add_argument('--multiplier',type=int,choices=range(2,7),default=4)
ap.add_argument('--latency-output',choices=('XeLL','Reflex'),default='XeLL')
ap.add_argument('--runtime-layout',choices=('adjacent','storage-only','preloaded-storage','mutated-storage','missing'),default='adjacent')
ap.add_argument('--overlay-keys',action='store_true')
ap.add_argument('--overlay-foreground',action='store_true')
ap.add_argument('--capcom-flags',action='store_true')
ap.add_argument('--retire-sleep',action='store_true')
ap.add_argument('--producer-shadow',action='store_true')
ap.add_argument('--legacy-factory',action='store_true')
ap.add_argument('--legacy-markers',action='store_true')
ap.add_argument('--mixed-markers',action='store_true')
ap.add_argument('--alias-markers',action='store_true')
ap.add_argument('--stall-trace',action='store_true')
ap.add_argument('--queue-proxy',action='store_true')
ap.add_argument('--parallel-factory',action='store_true')
ap.add_argument('--foreign-refs',action='store_true')
ap.add_argument('--retained-output',action='store_true')
ap.add_argument('--secondary-viewport',action='store_true')
ap.add_argument('--normal-depth',action='store_true')
ap.add_argument('--hud-conflict',action='store_true')
ap.add_argument('--tail-sleep',action='store_true')
ap.add_argument('--game-vsync',action='store_true')
ap.add_argument('--force-vsync-off',action='store_true')
ap.add_argument('--steam-observer',type=Path)
ap.add_argument('--stale-input',action='store_true')
ap.add_argument('--active-support',action='store_true')
ap.add_argument('--zero-resource',action='store_true')
ap.add_argument('--jittered-motion',action='store_true')
ap.add_argument('--simulation-sleep',action='store_true')
ap.add_argument('--midsim-sleep',action='store_true')
ap.add_argument('--struct-probe',action='store_true')
ap.add_argument('--reflex-v2',action='store_true')
ap.add_argument('--reflex-next',action='store_true')
ap.add_argument('--constants-v3',action='store_true')
ap.add_argument('--tag-next',action='store_true')
ap.add_argument('--backbuffer-subrect',action='store_true')
ap.add_argument('--scrgb-output',action='store_true')
ap.add_argument('--native-game-xefg',action='store_true')
ap.add_argument('--ffx-adapter',action='store_true')
ap.add_argument('--ffx-fixture',type=Path)
ap.add_argument('--dlss-sr-adapter',action='store_true')
ap.add_argument('--ngx-fixture',type=Path)
ap.add_argument('--dlss-sr-default-off',action='store_true')
# Metro Exodus field report: the DLSS-SR output enters exclusive fullscreen.
ap.add_argument('--exclusive-transition',action='store_true')
ap.add_argument('--exclusive-first',action='store_true')
ap.add_argument('--dlss-sr-pipelined',action='store_true')
ap.add_argument('--dlss-sr-double',action='store_true')
# 1.5.8: [XeFG] ExclusiveAsBorderless=1 (transition, first frame, creation).
for name in ('--exclusive-borderless','--exclusive-borderless-first','--exclusive-borderless-create'):
    ap.add_argument(name,action='store_true')
ap.add_argument('--hdr10',action='store_true')
ap.add_argument('--duplicate-tags',action='store_true')
ap.add_argument('--reserved-state',action='store_true')
ap.add_argument('--hud-srgb',action='store_true')
ap.add_argument('--hud-typeless',action='store_true')
ap.add_argument('--display-cap',action='store_true')
ap.add_argument('--scrgb-hudless',action='store_true')
ap.add_argument('--game-xefg-probe',action='store_true')
ap.add_argument('--ffx-with-streamline',action='store_true')
ap.add_argument('--ffx-with-reflex',action='store_true')
ap.add_argument('--ffx-with-dlssg',action='store_true')
ap.add_argument('--menu-viewport-first',action='store_true')
ap.add_argument('--late-interposer',action='store_true')
ap.add_argument('--sl1-interposer',action='store_true')
ap.add_argument('--sleep-gap',action='store_true')
ap.add_argument('--constant-sleep-token',action='store_true')
ap.add_argument('--late-interposer-hooked',action='store_true')
ap.add_argument('--unsupported-dlssg',action='store_true')
ap.add_argument('--learned-depth',action='store_true')
ap.add_argument('--d3d-debug',action='store_true')
ap.add_argument('--reframework',choices=('detached','not-tracked','blocked','invalid','exception','late','legacy','ambiguous'))
ap.add_argument('--ref-v1',type=Path)
ap.add_argument('--ref-legacy',type=Path)
args=ap.parse_args()
exclusive_borderless=args.exclusive_borderless or args.exclusive_borderless_first or args.exclusive_borderless_create
if args.exclusive_transition or args.exclusive_first or exclusive_borderless or args.dlss_sr_pipelined or args.dlss_sr_double: args.dlss_sr_adapter=True
d=args.directory.resolve()/datetime.datetime.now().strftime('native-%Y%m%d-%H%M%S-%f')
d.mkdir(parents=True)
executable_name='DS2.exe' if args.jittered_motion else args.exe.name
inputs={args.addon:'renodx-mfgunlock-native.addon64',args.exe:executable_name,
        args.fixture:'sl.interposer.dll',args.reshade:'dxgi.dll',
        args.sdk_fg:'libxess_fg.dll',args.sdk_ll:'libxell.dll'}
manifest={}
for src,name in inputs.items():
    shutil.copy2(src,d/name)
    manifest[name]=hashlib.sha256((d/name).read_bytes()).hexdigest()
if args.dlss_sr_adapter or args.dlss_sr_default_off:
    if args.ngx_fixture is None: ap.error('--dlss-sr-adapter requires --ngx-fixture')
    shutil.copy2(args.ngx_fixture,d/'_nvngx.dll')
    manifest['_nvngx.dll']=hashlib.sha256(args.ngx_fixture.read_bytes()).hexdigest()
    # Streamline is never loaded by this executable; the staged copy stays inert.
if args.ffx_adapter or args.ffx_with_streamline or args.ffx_with_reflex or args.ffx_with_dlssg:
    if args.ffx_fixture is None: ap.error('the FidelityFX cases require --ffx-fixture')
    shutil.copy2(args.ffx_fixture,d/'amd_fidelityfx_loader_dx12.dll')
    manifest['amd_fidelityfx_loader_dx12.dll']=hashlib.sha256(args.ffx_fixture.read_bytes()).hexdigest()
if args.steam_observer:
    shutil.copy2(args.steam_observer,d/'gameoverlayrenderer64.dll')
    manifest['gameoverlayrenderer64.dll']=hashlib.sha256(args.steam_observer.read_bytes()).hexdigest()
if args.reframework:
    for src in (args.ref_v1,args.ref_legacy):
        if src is None: ap.error('REFramework cases require both fixture paths')
        shutil.copy2(src,d/src.name)
        manifest[src.name]=hashlib.sha256(src.read_bytes()).hexdigest()
if args.runtime_layout!='adjacent':
    storage=d/'_storage_'; storage.mkdir()
    for name in ('libxess_fg.dll','libxell.dll'):
        if args.runtime_layout!='missing':
            shutil.copy2(d/name,storage/name)
        if args.runtime_layout in ('storage-only','missing'):
            (d/name).unlink()
    if args.runtime_layout=='mutated-storage':
        # Change only the unused DOS stub, retaining a loadable PE image with
        # the original exports and instruction profiles but an unadmitted SHA.
        path=storage/'libxess_fg.dll'
        content=bytearray(path.read_bytes()); content[0x40]^=1; path.write_bytes(content)
        manifest['_storage_/libxess_fg.dll']=hashlib.sha256(content).hexdigest()
(d/'MFGAdaUnlock.ini').write_text(f'[Backend]\nSelected=XeMFG\n[XeFG]\nMultiplier={args.multiplier}\n'
    +('InvertedDepth=0\n' if args.normal_depth else '')+('ForceVSyncOff=1\n' if args.force_vsync_off else '')+('DisplayCap=1\n' if args.display_cap else '')+('ExclusiveAsBorderless=1\n' if exclusive_borderless else '')
    +f'[Latency]\nOutput={args.latency_output}\n',encoding='ascii')
# One [Diagnostics] section: a second one would be ignored by GetPrivateProfile*.
diagnostics=(['StallTrace=1'] if args.d3d_debug else [])+(['SummaryIntervalMs=200'] if args.game_vsync else [])
if diagnostics:
    with (d/'MFGAdaUnlock.ini').open('a') as f: f.write('[Diagnostics]\n'+''.join(line+'\n' for line in diagnostics))
if args.dlss_sr_adapter:
    with (d/'MFGAdaUnlock.ini').open('a') as f: f.write('[Adapters]\nDlssSr=1\n')
if args.retained_output:
    with (d/'MFGAdaUnlock.ini').open('a') as f: f.write('[Compatibility]\nRetainNativeOutput=1\n')
(d/'ReShade.ini').write_text('[GENERAL]\nNoDebugInfo=1\n[INPUT]\nInputProcessing=2\nKeyOverlay=36,0,0,0\n[OVERLAY]\nTutorialProgress=4\n',encoding='ascii')
ctypes.windll.kernel32.SetErrorMode(3)
flags=(['--overlay-foreground'] if args.overlay_foreground else ['--overlay-keys'] if args.overlay_keys else [])
if args.d3d_debug: flags+=['--d3d-debug']
if args.capcom_flags: flags+=['--capcom-flags']
if args.retire_sleep: flags+=['--retire-sleep']
if args.producer_shadow: flags+=['--producer-shadow']
if args.legacy_factory: flags+=['--legacy-factory']
if args.reframework: flags+=['--ref-'+args.reframework]
if args.steam_observer: flags+=['--steam-observer']
if args.stale_input: flags+=['--stale-input']
if args.active_support: flags+=['--active-support']
for name in ('game_vsync','legacy_markers','mixed_markers','alias_markers','stall_trace','queue_proxy','parallel_factory','foreign_refs','retained_output','secondary_viewport','normal_depth','hud_conflict','tail_sleep','zero_resource','jittered_motion','simulation_sleep','midsim_sleep','struct_probe','reflex_v2','reflex_next','constants_v3','tag_next','backbuffer_subrect','menu_viewport_first','late_interposer','sl1_interposer','sleep_gap','constant_sleep_token','late_interposer_hooked','unsupported_dlssg','scrgb_output','native_game_xefg','ffx_adapter','dlss_sr_adapter','dlss_sr_default_off','duplicate_tags','reserved_state','hud_srgb','hud_typeless','display_cap','scrgb_hudless','game_xefg_probe','ffx_with_streamline','ffx_with_reflex','ffx_with_dlssg','exclusive_transition','exclusive_first','dlss_sr_pipelined','dlss_sr_double','hdr10','exclusive_borderless','exclusive_borderless_first','exclusive_borderless_create'):
    if getattr(args,name): flags+=['--'+name.replace('_','-')]
if args.runtime_layout in ('preloaded-storage','mutated-storage'): flags+=['--preload-runtimes']
if args.runtime_layout in ('mutated-storage','missing'): flags+=['--expect-startup-rejection']
learned_verified=None
if args.learned_depth:
    # Plan 3.3: a first session with normal-depth constants and no override
    # learns the convention; the next session must interpolate unaided.
    probe=subprocess.run([str(d/executable_name),str(args.multiplier),'--normal-depth','--convention-probe'],cwd=d,capture_output=True,text=True,timeout=120,creationflags=0x08000000)
    learned=configparser.ConfigParser(); learned.read(d/'MFGAdaUnlock.ini',encoding='ascii')
    learned_verified=(probe.returncode==0 and 'PASS: convention probe' in probe.stdout
        and learned.get('XeFG.Learned','InvertedDepth',fallback='')=='0' and learned.get('XeFG.Learned','Executable',fallback='')!='')
    (d/'MFGAdaUnlock-XeMFG.log').replace(d/'MFGAdaUnlock-XeMFG.probe.log')
    flags+=['--normal-depth']
result=subprocess.run([str(d/executable_name),str(args.multiplier),*flags],cwd=d,capture_output=True,text=True,timeout=120,creationflags=0x08000000)
prefs=configparser.ConfigParser(); prefs.read(d/'MFGAdaUnlock.ini',encoding='ascii')
events=[json.loads(line) for line in (d/'MFGAdaUnlock-XeMFG.log').read_text(encoding='utf-8').splitlines()] if (d/'MFGAdaUnlock-XeMFG.log').exists() else []
if args.dlss_sr_default_off:
    saved=configparser.ConfigParser(); saved.read(d/'MFGAdaUnlock.ini',encoding='ascii')
    verified=('PASS: DLSS-SR adapter is off by default' in result.stdout
        and not any(e.get('event') in ('native_ngx_hooks','native_synthetic_frames') for e in events)
        # 1.5.1: the panel's opt-in persists for the next launch only.
        and saved.get('Adapters','DlssSr',fallback='')=='1'
        and any(e.get('event')=='native_dlss_sr_saved' and e.get('enabled') is True for e in events))
    (d/'result.json').write_text(json.dumps(dict(inputs=manifest,exit=result.returncode,stdout=result.stdout,stderr=result.stderr,
        dlss_sr_default_off_verified=verified),indent=2))
    print(result.stdout,end='');print(result.stderr,end='');print('Evidence:',d)
    evidence.finish(d,result.returncode==0 and verified)
if args.dlss_sr_adapter:
    # Plan 5.4: opt-in NGX DLSS-SR adapter; DLSS itself keeps running.
    verified=('PASS: DLSS-SR adapter reached ready' in result.stdout
        and any(e.get('event')=='native_ngx_hooks' and e.get('hooked') is True for e in events)
        and any(e.get('event')=='native_ngx_feature' for e in events)
        and any(e.get('event')=='native_synthetic_frames' and e.get('adapter')=='dlss_sr' for e in events)
        and (not args.dlss_sr_pipelined or any(e.get('event')=='native_synthetic_pipelined' for e in events))
        and (not args.dlss_sr_double or not any(e.get('event')=='native_synthetic_pipelined' for e in events))
        and not any(e.get('event')=='native_synthetic_sleep' for e in events))
    (d/'result.json').write_text(json.dumps(dict(inputs=manifest,exit=result.returncode,stdout=result.stdout,stderr=result.stderr,
        dlss_sr_adapter_verified=verified),indent=2))
    print(result.stdout,end='');print(result.stderr,end='');print('Evidence:',d)
    evidence.finish(d,result.returncode==0 and verified)
if args.ffx_adapter or args.ffx_with_reflex or args.ffx_with_dlssg:
    # Plan 5.3: FSR 3.1 contexts replaced by the Intel output; markers synthesized.
    hooks=[e for e in events if e.get('event')=='native_ffx_hooks']
    active=any(e.get('event')=='native_synthetic_active' for e in events)
    deferred=any(e.get('event')=='native_synthetic_deferred' and e.get('reason')=='streamline_active' for e in events)
    verified=(any(e.get('hooked') is True for e in hooks)
        and any(e.get('event')=='native_ffx_swapchain' and e.get('adapter') is True for e in events)
        and any(e.get('event')=='native_ffx_generation' for e in events)
        and not any(e.get('event')=='native_synthetic_sleep' for e in events))
    if args.ffx_with_dlssg:
        # 1.5.1: a DLSS-G request with Streamline markers keeps the FSR frames passive.
        verified=verified and 'PASS: FidelityFX stayed passive while Streamline DLSS-G was requested' in result.stdout and deferred and not active
    elif args.ffx_with_reflex:
        # 1.5.1: Reflex markers alone (no DLSS-G request) leave FSR frame generation available.
        verified=verified and 'reached ready with Reflex markers' in result.stdout and active and not deferred
    else:
        verified=verified and 'PASS: FidelityFX frame generation adapter reached ready' in result.stdout and active
    (d/'result.json').write_text(json.dumps(dict(inputs=manifest,exit=result.returncode,stdout=result.stdout,stderr=result.stderr,
        ffx_adapter_verified=verified),indent=2))
    print(result.stdout,end='');print(result.stderr,end='');print('Evidence:',d)
    evidence.finish(d,result.returncode==0 and verified)
if args.native_game_xefg:
    # Plan 5.2: the game's own XeFG context receives the user's multiplier.
    counts=[e for e in events if e.get('event')=='count_result' and e.get('owner')=='game']
    verified=(f'PASS: game-owned XeFG uses the user multiplier ({args.multiplier}x)' in result.stdout
        and bool(counts) and all(e.get('generated')==args.multiplier-1 and e.get('result')==0 for e in counts)
        and any(e.get('event')=='game_multiplier_applied' for e in events))
    (d/'result.json').write_text(json.dumps(dict(inputs=manifest,exit=result.returncode,stdout=result.stdout,stderr=result.stderr,
        game_multiplier_verified=verified),indent=2))
    print(result.stdout,end='');print(result.stderr,end='');print('Evidence:',d)
    evidence.finish(d,result.returncode==0 and verified)
if args.reserved_state:
    # 1.5.6 (Witcher 3): a reserved state is replaced by the last valid state
    # of the SAME resource, never by a released resource's at the same address.
    reserved=[e for e in events if e.get('event')=='native_tag_reserved_state']
    verified=('PASS: reserved resource state substituted per resource and not inherited' in result.stdout
        and any(e.get('substituted')==64 for e in reserved)
        and any(e.get('substituted') is None for e in reserved)
        and not any(e.get('event') in ('native_present_failure','native_device_failure') for e in events))
    (d/'result.json').write_text(json.dumps(dict(inputs=manifest,exit=result.returncode,stdout=result.stdout,stderr=result.stderr,
        reserved_state_verified=verified),indent=2))
    print(result.stdout,end='');print(result.stderr,end='');print('Evidence:',d)
    evidence.finish(d,result.returncode==0 and verified)
if args.scrgb_output:
    # Plan 4.4: FP16 scRGB is converted to HDR10 PQ for the Intel proxy.
    inits=[e for e in events if e.get('event')=='native_swapchain_init']
    converted=[e for e in events if e.get('event')=='native_scrgb']
    verified=('PASS: scRGB output converted to HDR10 and interpolated' in result.stdout
        and any(e.get('event')=='native_scrgb_ready' and e.get('result')==0 for e in events)
        and bool(inits) and all(e.get('scrgb') is True for e in inits)
        and bool(converted) and all(e.get('ok') is True for e in converted))
    (d/'result.json').write_text(json.dumps(dict(inputs=manifest,exit=result.returncode,stdout=result.stdout,stderr=result.stderr,
        scrgb_verified=verified),indent=2))
    print(result.stdout,end='');print(result.stderr,end='');print('Evidence:',d)
    evidence.finish(d,result.returncode==0 and verified)
if args.sl1_interposer:
    untouched='PASS: SL 1.x interposer left untouched' in result.stdout
    (d/'result.json').write_text(json.dumps(dict(inputs=manifest,exit=result.returncode,stdout=result.stdout,stderr=result.stderr,
        sl1_untouched_verified=untouched),indent=2))
    print(result.stdout,end='');print(result.stderr,end='');print('Evidence:',d)
    evidence.finish(d,result.returncode==0 and untouched)
if args.reframework:
    blocked=args.reframework in ('blocked','invalid','exception','ambiguous')
    decisions=[e for e in events if e.get('event')=='native_reframework_retire']
    expected={'detached':2,'not-tracked':1,'blocked':3,'invalid':4,'exception':5,'late':2,'legacy':0,'ambiguous':7}[args.reframework]
    verified=bool(decisions) and all(e['decision']==expected for e in decisions)
    if blocked:
        verified=verified and 'PASS: REFramework refusal preserved proxy/context, blocked recreation and avoided retry' in result.stdout
        verified=verified and not any(e.get('event')=='native_output_retired' for e in events)
    else:
        verified=verified and 'REFramework safe handoff: reentrant callback, module lease and recreation verified' in result.stdout
    (d/'result.json').write_text(json.dumps(dict(inputs=manifest,exit=result.returncode,stdout=result.stdout,stderr=result.stderr,scenario=args.reframework,retirement_verified=verified),indent=2))
    print(result.stdout,end='');print(result.stderr,end='');print('Evidence:',d)
    evidence.finish(d,result.returncode==0 and verified)
if args.parallel_factory:
    verified='PASS: simultaneous factory callers published one Intel owner; both outputs and contexts retired' in result.stdout
    (d/'result.json').write_text(json.dumps(dict(inputs=manifest,exit=result.returncode,stdout=result.stdout,stderr=result.stderr,parallel_factory_verified=verified),indent=2))
    print(result.stdout,end='');print(result.stderr,end='');print('Evidence:',d)
    evidence.finish(d,result.returncode==0 and verified)
if args.foreign_refs or args.retained_output:
    verified='PASS: foreign native references block HWND recreation; only observer-owned releases permit recovery' in result.stdout
    if args.retained_output:
        verified='PASS: retained native HWND rebound to a new Intel context with observer references preserved' in result.stdout
        verified=verified and any(e.get('event')=='native_output_reused' and e.get('result')==0 for e in events)
    (d/'result.json').write_text(json.dumps(dict(inputs=manifest,exit=result.returncode,stdout=result.stdout,stderr=result.stderr,foreign_reference_verified=verified),indent=2))
    print(result.stdout,end='');print(result.stderr,end='');print('Evidence:',d)
    evidence.finish(d,result.returncode==0 and verified)
if args.retire_sleep:
    # 1.5.6: the game's native Reflex wait runs before any XeLL operation, so
    # nothing is in flight on the Intel side when the output retires during it
    # (sleep_in_flight false). Retirement during an in-flight XeLL wait is
    # covered by xemfg_latency_translation.
    verified=any(e.get('event')=='native_output_retired' and e.get('sleep_in_flight') is False and e.get('destroy_result')==0 for e in events)
    (d/'result.json').write_text(json.dumps(dict(inputs=manifest,exit=result.returncode,stdout=result.stdout,stderr=result.stderr,retirement_during_sleep_verified=verified),indent=2))
    print(result.stdout,end='');print(result.stderr,end='');print('Evidence:',d)
    evidence.finish(d,result.returncode==0 and verified and 'same HWND rebound before sleep returned' in result.stdout)
if args.game_vsync:
    # The game presents with sync interval 1. It is reported once; the
    # override happens only with ForceVSyncOff, and native_summary carries
    # the interval while the output presents (SummaryIntervalMs=200).
    reported=[e for e in events if e.get('event')=='native_game_vsync']
    forced=[e for e in events if e.get('event')=='native_vsync_forced_off']
    summaries=[e for e in events if e.get('event')=='native_summary']
    verified=len(reported)==1 and reported[0].get('sync_interval')==1 and reported[0].get('force_vsync_off')==int(args.force_vsync_off)
    verified=verified and (len(forced)==1 and forced[0].get('game_sync_interval')==1 if args.force_vsync_off else not forced)
    verified=verified and any(e.get('frames',0)>0 and e.get('game_sync_interval')==1 and e.get('force_vsync_off')==int(args.force_vsync_off)
                              and e.get('interval_ms',0)>=200 and e.get('tags_rejected')==0 and 'hud_filtered' in e for e in summaries)
    (d/'result.json').write_text(json.dumps(dict(inputs=manifest,exit=result.returncode,stdout=result.stdout,stderr=result.stderr,
        game_vsync_verified=verified,reported=reported,forced=forced,summaries=summaries[:3]),indent=2))
    print(result.stdout,end=''); print(result.stderr,end=''); print('Evidence:',d)
    if not verified: print('game VSync not reported as expected:',reported,forced,summaries[:2])
    evidence.finish(d,result.returncode==0 and 'PASS:' in result.stdout and verified)
if args.runtime_layout in ('mutated-storage','missing'):
    expected='unreviewed_Intel_runtime_pair' if args.runtime_layout=='mutated-storage' else 'missing_Intel_runtime:libxess_fg.dll'
    refused=any(e.get('event')=='startup_rejected' and e.get('reason')==expected for e in events)
    unpublished=not any(e.get('event') in ('ready','runtime_selection','native_context_created') for e in events)
    # 1.5.5: the refusal names each file's state before the reason code.
    files=[e for e in events if e.get('event')=='runtime_files']
    fg=files[-1].get('libxess_fg.dll',{}) if files else {}; ll=files[-1].get('libxell.dll',{}) if files else {}
    if args.runtime_layout=='mutated-storage':
        described=fg.get('state')=='different' and bool(fg.get('version')) and len(fg.get('sha256',''))==64 \
            and '_storage_' in fg.get('path','') and ll.get('state') in ('pinned','accepted_older')
    else:
        described=fg.get('state')=='missing' and ll.get('state')=='missing' and not fg.get('sha256')
    (d/'result.json').write_text(json.dumps(dict(inputs=manifest,runtime_layout=args.runtime_layout,exit=result.returncode,
        stdout=result.stdout,stderr=result.stderr,startup_rejection_verified=refused,model_unpublished=unpublished,
        runtime_files_described=described),indent=2))
    print(result.stdout,end=''); print(result.stderr,end=''); print('Evidence:',d)
    if not described: print('runtime_files did not describe the refused files:',files[-1] if files else None)
    evidence.finish(d,result.returncode==0 and 'PASS:' in result.stdout and refused and unpublished and described)
selections=[e for e in events if e.get('event')=='runtime_selection']
runtime_selection=bool(selections) and all('_storage_' in e['provider_path'] and '_storage_' in e['xell_path'] for e in selections) if args.runtime_layout!='adjacent' else bool(selections)
sdk_deadlines=[e for e in events if e.get('event')=='dlssg_interval_deadline' and e.get('sdk_deadlines')]
policy=[e.get('deadlines') for e in events if e.get('event')=='pacing_policy']
policy_activation=policy==['sdk','adaptive']
# An occluded output may not execute the deadline body. Distinguish that lack
# of observation from either proving passthrough or witnessing a violation.
passthrough=all(e['deadline_ns']==e['sdk_deadline_ns'] and not e['flip_metered'] for e in sdk_deadlines) if sdk_deadlines else None
hud_filter=len([e for e in events if e.get('event')=='native_hud_filtered'])>=2
passthrough_identity=any(e.get('event')=='native_bypass_present_id' and e.get('id')==35 and e.get('result')==0 for e in events)
native_keys=(('Overlay key transitions: opens=2 closes=2' in result.stdout
    and 'Overlay keys after resize/FG-off: opens=3 closes=3' in result.stdout
    and 'Production panel draws=' in result.stdout) if args.overlay_keys else None)
native_child=(('Overlay keys with child input: opens=4 closes=4' in result.stdout) if args.overlay_keys else None)
foreground_verified=('Foreground ReShade: opens=6 closes=6; production panel drawn; game commands captured/released' in result.stdout) if args.overlay_foreground else None
persisted=(prefs.get('Latency','Input',fallback='')=='Reflex'
    and prefs.get('Latency','Output',fallback='')==args.latency_output
    and prefs.get('XeFG','ForceBorderless',fallback='')=='0'
    and prefs.get('XeFG','XeFGFlipMetering',fallback='')=='on'
    and prefs.get('XeFG','UIComposition',fallback='')=='0'
    and prefs.get('XeFG','SdkDeadlines',fallback='')=='0')
legacy_verified=(('Legacy factory: inference, resize, fallback and recreation verified' in result.stdout
    and 'Legacy native fallback: second output native and unchanged; no extra Intel context or manual final overlay' in result.stdout) if args.legacy_factory else None)
markers_verified=('Historical markers: original calls preserved, real inference and duplicate forwarding verified' in result.stdout) if args.legacy_markers or args.mixed_markers or args.alias_markers else None
tail_verified=(('Tail sleep: first frame bypassed, following frames interpolated, original Reflex token and wait count preserved' in result.stdout)
    and any(e.get('event')=='native_sleep_association' and e.get('target')==e.get('source',0)+1 for e in events)) if args.tail_sleep else None
# Async diagnostics deliberately drop records rather than block a producer.
observer_verified=('Observer isolation: native plugin state preserved; game inference continued after observer OFF' in result.stdout) if args.steam_observer else None
stale_verified=('Stale source input: original return preserved; old epoch discarded before new-session admission' in result.stdout) if args.stale_input else None
input_verified={}
if args.zero_resource:
    input_verified['zero_resource']=('Zero resource metadata: real D3D12 textures, volatile producer copies and inference accepted' in result.stdout)
if args.jittered_motion:
    # Plan 4.2: jittered vectors are converted by our pass; XeFG runs unjittered.
    inits=[e for e in events if e.get('event')=='native_swapchain_init']
    converted=[e for e in events if e.get('event')=='native_dejitter']
    input_verified['jittered_motion']=('Jittered motion: dejittered by the bridge pass and interpolated' in result.stdout
        and any(e.get('event')=='native_dejitter_ready' and e.get('result')==0 for e in events)
        and bool(inits) and all(e.get('motion_dejitter') is True and not (e.get('init_flags',0)&16) for e in inits)
        and bool(converted) and all(e.get('ok') is True for e in converted) and any(e.get('continuous') for e in converted))
if args.simulation_sleep:
    recovered=[e for e in events if e.get('event')=='native_simulation_boundary_sleep']
    input_verified['simulation_sleep']=('Simulation boundary: missing sleep recovered after warmup; explicit source waits resumed without duplicate sleep' in result.stdout
        and bool(recovered) and all(e.get('result')==0 for e in recovered))
if args.midsim_sleep:
    recovered=[e for e in events if e.get('event')=='native_simulation_boundary_sleep']
    refused=[e for e in events if e.get('event')=='native_sleep_rejected']
    input_verified['midsim_sleep']=('Simulation boundary: refused mid-simulation waits recovered after warmup; explicit source waits resumed without duplicate sleep' in result.stdout
        and bool(recovered) and all(e.get('result')==0 for e in recovered)
        and bool(refused) and all(e.get('reason')=='inside_simulation' and e.get('observed',0)&3==1 for e in refused))
compatibility_verified=False
if (d/'compatibility.json').is_file():
    support=json.loads((d/'compatibility.json').read_text(encoding='utf-8'))
    compatibility_verified=(support.get('schema')=='mfgunlock.compatibility' and support.get('version')==1
        and support.get('displayed_frames_measured') is False and support.get('physical_latency_measured') is False
        and support.get('fallback')=='xemfg_selected_passthrough'
        and all(v in ('confirmed','inferred','unavailable','invalid') for v in support['capabilities'].values())
        and support['modules']['frame_generation'].get('sha256')==manifest['libxess_fg.dll']
        and support['modules']['latency'].get('sha256')==manifest['libxell.dll']
        and not re.search(r'[A-Za-z]:[\\/]',json.dumps(support)))
    if args.steam_observer: compatibility_verified=compatibility_verified and support.get('external_observer_resolutions')==2
    if args.stale_input: compatibility_verified=compatibility_verified and support.get('stale_inputs_discarded')==1
active_support_verified=None
if args.active_support:
    active=json.loads((d/'active-compatibility.json').read_text(encoding='utf-8')) if (d/'active-compatibility.json').is_file() else {}
    active_support_verified=(active.get('lifecycle')=='active' and active.get('blocker')=='ready'
        and active.get('frame',{}).get('id')==49
        and all(active.get('capabilities',{}).get(key)=='confirmed' for key in
                ('depth','motion_vectors','constants','source_wait','latency_markers'))
        and len(active.get('frame',{}).get('resource_receipts',[]))==3
        and all(r['accepted'] and r['width']==640 and r['height']==360 and r['rejection']=='none'
                for r in active.get('frame',{}).get('resource_receipts',[])))
# Diagnostics every native session must emit (plan items 1.2-1.8).
diagnostics_verified={}
session=[e for e in events if e.get('event')=='native_session']
inventory=[e for e in events if e.get('event')=='native_modules']
diagnostics_verified['session']=(len(session)==1 and re.match(r'\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ',session[0].get('utc','')) is not None
    and session[0].get('executable')==executable_name and bool(session[0].get('gpu')) and session[0].get('windows')
    and bool(inventory) and any(m.get('name')=='sl.interposer.dll' for m in inventory[0].get('modules',[]))
    and not re.search(r'[A-Za-z]:[\\/]',json.dumps(session+inventory)))
# The smoke calls slInit with the kSDKVersion of the Streamline headers it was built with.
# When slInit ran before the hooks, the version must say so instead of guessing.
versions=[e for e in events if e.get('event')=='native_streamline_version']
boundaries=[e.get('boundary') for e in events if e.get('event')=='native_presentation_isolation']
expected_sdk='2.12.0' if 'slInit' in boundaries else 'unobserved'
diagnostics_verified['streamline_version']=(bool(versions) and versions[-1].get('sdk_version')==expected_sdk
    and versions[-1].get('sdk_magic') is (expected_sdk!='unobserved') and 'interposer' in versions[-1]
    and (not (d/'compatibility.json').is_file() or support.get('streamline',{}).get('sdk_version')==expected_sdk))
# Every XeLL context reports its runtime version and a registered SDK log callback.
xell=[e for e in events if e.get('event')=='native_xell_context']
diagnostics_verified['xell']=(bool(xell) and all(e.get('logging_result')==0 and re.match(r'\d+\.\d+\.\d+',e.get('version','')) for e in xell)
    and not any(e.get('event')=='native_xell_failure' for e in events))
# Support queries are observed with the exact result Streamline returned to the game.
# The last query runs after output creation, when the hooks are guaranteed.
queried=re.findall(r'Feature query: DLSS-G supported=(\d+) requirements=(\d+)',result.stdout)
queries={(e.get('api'),e.get('feature'),e.get('result')) for e in events if e.get('event')=='native_feature_query'}
diagnostics_verified['feature_query']=bool(queried) and ('slIsFeatureSupported',1000,int(queried[-1][0])) in queries \
    and ('slGetFeatureRequirements',1000,int(queried[-1][1])) in queries
# One constants sample per output at its first complete frame, and a bounded
# histogram of the buffer types and viewports the game tagged.
samples=[e for e in events if e.get('event')=='native_constants_sample']
diagnostics_verified['constants_sample']=bool(samples) and all(
    all(k in s for k in ('mvec_scale','jitter','camera_near','camera_far','camera_fov','depth_inverted','motion_jittered','motion_dilated','orthographic'))
    for s in samples)
histogram=[e for e in events if e.get('event')=='native_tag_histogram']
diagnostics_verified['tag_histogram']=bool(histogram) and any(
    any(t.get('type')==0 and t.get('count',0)>0 for t in h.get('types',[])) and any(v.get('viewport')==0 for v in h.get('viewports',[]))
    for h in histogram)
# The report is written to the game folder without a button press.
exported=d/'MFGAdaUnlock-XeMFG-compatibility.json'
try:
    exported_report=json.loads(exported.read_text(encoding='utf-8')) if exported.is_file() else {}
except json.JSONDecodeError:
    exported_report={}
diagnostics_verified['report_export']=(exported_report.get('schema')=='mfgunlock.compatibility'
    and exported_report.get('export_reason') in ('steady','retired','shutdown')
    and not re.search(r'[A-Za-z]:[\\/]',json.dumps(exported_report)))
if args.ffx_with_streamline:
    # 1.5.1 (Darktide): the FSR swapchain is ours, but the frames are Streamline's.
    diagnostics_verified['ffx_with_streamline']=('FidelityFX swapchain with Streamline DLSS-G' in result.stdout
        and any(e.get('event')=='native_ffx_swapchain' and e.get('adapter') is True for e in events)
        and not any(e.get('event')=='native_synthetic_active' for e in events))
if args.game_xefg_probe:
    diagnostics_verified['game_xefg_probe']='Game XeFG probe: uninitialized context did not block the Intel output' in result.stdout
if args.duplicate_tags:
    diagnostics_verified['duplicate_tags']=any(e.get('event')=='native_tag_duplicate' for e in events) and not any(
        e.get('event')=='native_sdk_log' and 'already tagged' in e.get('message','') for e in events)
if args.display_cap:
    # 1.5.7: with no game limit, the XeLL interval is the display cap for the
    # measured refresh and multiplier, and 0 again after it is turned off.
    caps=[e for e in events if e.get('event')=='native_display_cap' and e.get('interval_us',0)>0]
    modes=[e.get('interval_us') for e in events if e.get('event')=='native_sleep_mode']
    diagnostics_verified['display_cap']=(bool(caps) and caps[0].get('enabled')==1
        and caps[0]['interval_us'] in modes and 0 in modes[modes.index(caps[0]['interval_us']):])
if args.scrgb_hudless:
    # 1.5.7: the FP16 HUD-less of an scRGB game is encoded to HDR10 and used.
    copies=[e for e in events if e.get('event')=='native_hud_copy']
    diagnostics_verified['scrgb_hudless']=(any(e.get('event')=='native_scrgb_ready' and e.get('result')==0 for e in events)
        and any(e.get('event')=='native_hud_input' and e.get('type')==0 and e.get('format')==10 and e.get('copied') is True for e in events)
        and bool(copies) and all(e.get('ok') is True for e in copies)
        and not any(e.get('event')=='native_tag_rejected' and e.get('type')==0 and e.get('format')==10 for e in events)
        and not any(e.get('event')=='native_sdk_log' and e.get('level',0)>=3 for e in events))
if args.hud_srgb or args.hud_typeless:
    # 1.5.7: the HUD-less tag in another type of the output's bits is copied
    # into the output format, at the tag (volatile) and at Present.
    copies=[e for e in events if e.get('event')=='native_hud_copy']
    diagnostics_verified['hud_copy']=(any(e.get('event')=='native_hud_input' and e.get('type')==0 and e.get('copied') is True for e in events)
        # Frame 38 tags motion vectors as HUD-less on purpose; only this case's format must pass.
        and not any(e.get('event')=='native_tag_rejected' and e.get('type')==0 and e.get('format')==(29 if args.hud_srgb else 27) for e in events)
        and bool(copies) and all(e.get('ok') is True for e in copies)
        and not any(e.get('event')=='native_sdk_log' and e.get('level',0)>=3 for e in events))
if args.backbuffer_subrect:
    diagnostics_verified['backbuffer_region']=any(e.get('event')=='native_backbuffer_region' and e.get('width')==320 and e.get('height')==180 for e in events)
# Shipped profiles load once per session, with no override error.
loaded=[e for e in events if e.get('event')=='native_profiles']
diagnostics_verified['profiles']=len(loaded)==1 and loaded[0].get('rules',0)>=3 and loaded[0].get('override_error')=='' \
    and 'conflicting-frame-generation-plugins' in loaded[0].get('applied',[])
if args.unsupported_dlssg:
    # RTX 20/30: the game must see DLSS-G as supported while XeMFG generates.
    diagnostics_verified['dlssg_support_presented']=any(e.get('event')=='native_feature_query' and e.get('feature')==1000
        and e.get('result')==0 and e.get('native_result')==7 for e in events)
if args.struct_probe:
    # Each newer or chained structure is named, whether refused or adapted by prefix.
    refused={(e.get('api'),e.get('version'),e.get('next')) for e in events if e.get('event') in ('native_struct_rejected','native_struct_adapted')}
    diagnostics_verified['struct_rejections']=all(k in refused for k in (('slReflexSetOptions',2,False),('slReflexSetOptions',1,True),
        ('slDLSSGSetOptions',6,False),('slSetConstants',3,False)))
# The child checks each real API call, SDK status and output lifetime directly;
# event counts are supporting evidence, not the acceptance oracle.
(d/'result.json').write_text(json.dumps({'latency_output':args.latency_output,'runtime_layout':args.runtime_layout,'runtime_selection_verified':runtime_selection,'inputs':manifest,'exit':result.returncode,
    'stdout':result.stdout,'stderr':result.stderr,'control_preferences_persisted':persisted,
    'sdk_policy_activation_verified':policy_activation,'sdk_deadline_records':len(sdk_deadlines),
    'sdk_deadline_passthrough_verified':passthrough,'hud_filter_verified':hud_filter,'passthrough_identity_verified':passthrough_identity,
    'native_reshade_keys_verified':native_keys,'native_reshade_child_verified':native_child,'foreground_reshade_verified':foreground_verified,
    'production_panel_verified':'Production panel draws=' in result.stdout,
    'producer_shadow_verified':args.producer_shadow and result.returncode==0 and 'Heap producer commands: accepted; non-code implementation: rejected; generation resumed' in result.stdout,
    'legacy_factory_verified':legacy_verified,
    'historical_markers_verified':markers_verified,
    'tail_sleep_verified':tail_verified,
    'observer_isolation_verified':observer_verified,
    'stale_input_verified':stale_verified,
    'input_compatibility_verified':input_verified,
    'compatibility_export_verified':compatibility_verified,
    'active_support_verified':active_support_verified,'learned_depth_verified':learned_verified,'diagnostics_verified':diagnostics_verified,
    'native_queue_identity_verified':('Queue native identity: wrapper unwrapped; inference and lifetime verified' in result.stdout) if args.queue_proxy else None,
    'gpu_queue_timestamps_verified':any(e.get('event')=='gpu_queue_span' for e in events) if args.stall_trace else None,
    'legacy_factory_success_records':sum(e.get('event')=='native_legacy_factory_create' and e.get('replaced') for e in events)},indent=2),encoding='utf-8')
print(result.stdout,end=''); print(result.stderr,end=''); print('Evidence:',d)
evidence.finish(d,result.returncode==0 and 'PASS:' in result.stdout and learned_verified is not False and all(diagnostics_verified.values()) and compatibility_verified and all(input_verified.values()) and active_support_verified is not False and runtime_selection and persisted and policy_activation and passthrough is not False and hud_filter and passthrough_identity and native_keys is not False and native_child is not False and foreground_verified is not False and legacy_verified is not False and markers_verified is not False and tail_verified is not False and observer_verified is not False and stale_verified is not False and (not args.stall_trace or any(e.get('event')=='gpu_queue_span' for e in events)) and (not args.queue_proxy or 'Queue native identity: wrapper unwrapped; inference and lifetime verified' in result.stdout))
