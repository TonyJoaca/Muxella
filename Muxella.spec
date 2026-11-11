# -*- mode: python ; coding: utf-8 -*-
import os
from pathlib import Path

# Tool runtimes on the build PATH can contribute incompatible copies of
# Windows DLLs. They are not dependencies of this application.
os.environ['PATH'] = os.pathsep.join(
    entry for entry in os.environ.get('PATH', '').split(os.pathsep)
    if 'codex-runtimes' not in entry.lower()
)

a = Analysis(
    ['muxella_gui.py'],
    pathex=[],
    binaries=[('build/muxella_server.exe', '.'), ('build/muxella_cables.exe', '.')],
    datas=[('res', 'res'), ('virtual_audio/LICENSE.txt', 'licenses/virtual_audio'),
           ('third_party/nlohmann/LICENSE.MIT', 'licenses/nlohmann')],
    hiddenimports=[],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[],
    noarchive=False,
    optimize=0,
)
# Windows 10/11 supply the Universal CRT and API-set contracts. Bundling
# older copies can make Qt fail with "The specified procedure could not be found".
a.binaries = [
    entry for entry in a.binaries
    if Path(entry[0]).name.lower() != 'ucrtbase.dll'
    and not Path(entry[0]).name.lower().startswith(('api-ms-win-', 'ext-ms-win-'))
]
pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.datas,
    [],
    name='Muxella',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=False,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
    icon=['res/icons/app.ico'],
)
