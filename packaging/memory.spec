from PyInstaller.utils.hooks import collect_all, copy_metadata

datas, binaries, hiddenimports = collect_all('mnemosyne')
datas += copy_metadata('mnemosyne-memory', recursive=True)
# Runtime selection in uvicorn and numpy is dynamic.
hiddenimports += ['uvicorn.logging', 'uvicorn.loops.auto',
                  'uvicorn.protocols.http.auto', 'uvicorn.protocols.websockets.auto',
                  'uvicorn.lifespan.on', 'numpy', 'numpy._core',
                  # imported only when a PDF book is read (mnemosyne.skills)
                  'pypdf']
a = Analysis(['memory-entry.py'], pathex=[], binaries=binaries, datas=datas,
             hiddenimports=hiddenimports, excludes=['tkinter', 'pytest'])
pyz = PYZ(a.pure)
exe = EXE(pyz, a.scripts, [], exclude_binaries=True, name='mneme-mcp',
          console=True, upx=False)
coll = COLLECT(exe, a.binaries, a.datas, name='mneme-mcp', upx=False)
