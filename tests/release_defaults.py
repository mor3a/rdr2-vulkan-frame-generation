"""Release policy checks. No game is started and no network request is sent."""
from pathlib import Path
import configparser
import re
root=Path(__file__).resolve().parents[1]
cfg=configparser.ConfigParser(interpolation=None)
cfg.read(root/'RDR2VulkanFG.ini',encoding='utf-8-sig')
c=cfg['fgvk']
for key,value in {'FGProvider':'DLSSG','DLSSGBackend':'Native','CameraSource':'Neutral',
                  'ReflexSleep':'0','ReflexMode':'1','CameraAudit':'0','Diagnostics':'0',
                  'IntervalLogging':'0','DebugLogging':'0','OutputFPSCap':'0','MenuKey':'0x2D'}.items():
    assert c[key]==value,(key,c[key])
assert 'CheckUpdates' not in c
assert not any('developer' in key or 'simulate' in key or 'benchmark' in key for key in c)
menu=(root/'src/menu.cpp').read_text(encoding='utf-8')
assert '0.80 experiments' not in menu
assert 'DLSS-G backend"' not in menu and 'Camera source"' not in menu
assert 'RequestNativeRestart' in menu and 'PresetSelection' in menu
assert 'requestedPreset!=Rt().appliedPreset' not in menu
update=(root/'src/update_check.cpp').read_text(encoding='utf-8')
config=(root/'src/config.cpp').read_text(encoding='utf-8')
assert 'checkUpdates' not in update and 'CheckUpdates' not in config
assert 'StartUpdateCheck' in menu and 'OpenUpdateDownload' in menu
assert 'reflexSleep=false' in config and '"ReflexSleep=0\\n"' in config
assert not re.search(r'Checkbox\([^\n]*(?:Reflex|SDK sleep|Native Vulkan)',menu)
print('Release defaults, mandatory updates, INI-only Native controls and preset/restart menu policy: PASS')
