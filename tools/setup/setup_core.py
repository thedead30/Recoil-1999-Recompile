"""setup_core.py - what the player Setup does, without the window (recoil_setup.py is the window; this file can run alone).

Installs the Recoil remake from the players' download plus the player's own copy of Recoil:
  1. dgVoodoo2   - DDraw.dll + D3DImm.dll (the zip's MS\\x86 folder), dgVoodooCpl.exe and dgVoodoo.conf with the tested settings
  2. Recoil copy - any form tools/recoil_source.py reads; must be the supported 1999-01-29 build. Game files are copied (the
                   original Recoil.exe goes to original\\Recoil.exe, where the remake reads its data image at start-up), CD audio
                   tracks become music\\trackNN.wav (only full disc images carry them)
  3. the remake  - payload\\Recoil Remake.exe, plus the original's resources (menus, dialogs, icons) copied from the player's exe
  4. a desktop shortcut
Nothing from Recoil is in the download: every game byte comes from the copy the player points at.
usage: python tools/setup/setup_core.py --source COPY --dgvoodoo ZIP --dest DIR [--payload EXE] [--no-shortcut]
"""
import os, re, shutil, subprocess, sys, zipfile

HERE = os.path.dirname(os.path.abspath(sys.executable if getattr(sys, 'frozen', False) else __file__))
sys.path.insert(0, os.path.normpath(os.path.join(HERE, '..')))   # tools/ (recoil_source, copy_resources) when run from the source tree
import recoil_source                       # noqa: E402
from copy_resources import copy_resources  # noqa: E402

EXE_NAME = 'Recoil Remake.exe'
DGV_TESTED = '2.8.7.3 and 2.8.7.5'
DGV_LINK = 'https://github.com/dege-diosg/dgVoodoo2/releases'
# dgVoodoo.conf values the remake was tested with (PLATFORM: dgVoodoo settings, from the tested conf of 2026-09-09; not Recoil values)
DGV_SETTINGS = {
    'General': {'OutputAPI': 'bestavailable', 'FullScreenMode': 'true', 'KeepWindowAspectRatio': 'true'},
    'Glide': {'EnableInactiveAppState': 'false'},
    'DirectX': {'VideoCard': 'internal3D', 'VRAM': '256', 'AppControlledScreenMode': 'false',
                'DisableAltEnterToToggleScreenMode': 'true', 'dgVoodooWatermark': 'false'},
}


class SetupError(Exception):
    pass


def default_payload():
    for p in (os.path.join(HERE, 'payload', EXE_NAME),                                                  # the players' download
              os.path.join(HERE, '..', '..', '05_remake', 'build', 'Player', 'recoil_boot.exe')):     # source tree, player build
        if os.path.exists(p):
            return os.path.normpath(p)
    return None


# ---------------------------------------------------------------- dgVoodoo
def dgvoodoo_members(zip_path):
    """{'DDraw.dll': member, 'D3DImm.dll': member, 'dgVoodoo.conf': member or None, 'dgVoodooCpl.exe': member or None}"""
    if not zipfile.is_zipfile(zip_path):
        raise SetupError('%s is not a zip file - select the dgVoodoo2 zip you downloaded (%s)' % (zip_path, DGV_LINK))
    names = zipfile.ZipFile(zip_path).namelist()
    found = {}
    for want in ('DDraw.dll', 'D3DImm.dll'):
        m = [n for n in names if re.search(r'(^|/)MS/x86/' + re.escape(want) + '$', n, re.I)]
        if not m:
            raise SetupError('%s has no MS/x86/%s - is it the dgVoodoo2 zip? (%s)' % (os.path.basename(zip_path), want, DGV_LINK))
        found[want] = m[0]
    for want in ('dgVoodoo.conf', 'dgVoodooCpl.exe'):
        m = sorted((n for n in names if n.lower().split('/')[-1] == want.lower()), key=lambda n: n.count('/'))
        found[want] = m[0] if m else None
    return found


def patch_conf(text):
    """set DGV_SETTINGS in a dgVoodoo.conf text (keys added to their section when missing)"""
    lines = text.splitlines() if text else []
    for sec, kv in DGV_SETTINGS.items():
        start = next((i for i, l in enumerate(lines) if l.strip().lower() == '[%s]' % sec.lower()), None)
        if start is None:
            lines += ['', '[%s]' % sec]
            start = len(lines) - 1
        end = next((i for i in range(start + 1, len(lines)) if lines[i].strip().startswith('[')), len(lines))
        for key, val in kv.items():
            hit = next((i for i in range(start + 1, end) if re.match(r'\s*%s\s*=' % re.escape(key), lines[i], re.I)), None)
            new = '%-37s= %s' % (key, val)
            if hit is None:
                lines.insert(end, new)
                end += 1
            else:
                lines[hit] = new
    return '\r\n'.join(lines) + '\r\n'


def install_dgvoodoo(zip_path, dest, log):
    m = dgvoodoo_members(zip_path)
    z = zipfile.ZipFile(zip_path)
    for name in ('DDraw.dll', 'D3DImm.dll', 'dgVoodooCpl.exe'):
        if m[name]:
            with open(os.path.join(dest, name), 'wb') as f:
                f.write(z.read(m[name]))
    conf = z.read(m['dgVoodoo.conf']).decode('latin-1') if m['dgVoodoo.conf'] else ''
    with open(os.path.join(dest, 'dgVoodoo.conf'), 'w', encoding='latin-1', newline='') as f:
        f.write(patch_conf(conf))
    log('dgVoodoo2: DDraw.dll, D3DImm.dll%s and dgVoodoo.conf (full screen) installed' % (', dgVoodooCpl.exe' if m['dgVoodooCpl.exe'] else ''))


# ---------------------------------------------------------------- the player's Recoil copy
def check_source(path):
    """-> (Source, description lines); SetupError when it cannot be used"""
    try:
        s = recoil_source.Source(path)
    except Exception as e:   # unreadable image / archive
        raise SetupError('cannot read %s (%s)' % (path, e))
    if not s.game:
        raise SetupError('no Recoil game files found in %s (looked for Recoil.exe + zbd folder, or the CD installer data1.cab)' % path)
    try:
        exe = s.game.read('Recoil.exe')
    except KeyError:
        raise SetupError('Recoil.exe is missing from %s' % path)
    _h, name, ok = recoil_source.exe_version(exe)
    if not ok:
        raise SetupError('This copy is: %s.\n\nThe remake %s' % (name, recoil_source.SUPPORTED_NOTE.replace('this build tool only accepts', 'only accepts')))
    lines = ['format: %s - %s' % (s.kind, s.game.kind), 'Recoil.exe: %s (supported)' % name]
    if s.audio:
        lines.append('CD music: %d tracks' % len(s.audio))
    else:
        lines.append('CD music: none in this copy (only full disc images .nrg / .bin+.cue have it) - the game runs without music')
    return s, lines


# The CD's installer cabinet keeps some files at its root that the original installer puts elsewhere (CONFIRMED-DATA: the
# cabinet listing of the Recoil Classic CD vs an installed copy, 2026-10-07): the sound banks go to zbd\, and Windows
# redistributables (for the system folder) plus the 3dfx exe are not part of the game folder. A 1998 DLL beside the exe would
# be loaded instead of Windows' own (DLL search order), so they are never copied.
CAB_TO_ZBD = {'soundsh.zbd', 'soundsl.zbd', 'soundsm.zbd'}
NOT_GAME_FILES = {'mfc42.dll', 'msvcirt.dll', 'msvcp50.dll', 'msvcrt.dll', 'msvcrt20.dll', 'msvcrtd.dll', 'msvfw32.dll',
                  'ws2_32.dll', 'ws2help.dll', 'ir50_32.dll', 'eurosti.ttf', 'eurostib.ttf', 'recoil3dfx.exe'}


def game_files(s):
    """[(path in the copy, path in the game folder)]"""
    out = []
    for rel in s.game.list():
        rel = rel.replace('\\', '/')
        low = rel.lower()
        if low in NOT_GAME_FILES:
            continue
        out.append((rel, 'zbd/' + rel if low in CAB_TO_ZBD else rel))
    return out


def install_game(s, dest, log, progress):
    files = game_files(s)
    total = len(files) + len(s.audio)
    for i, (rel, place) in enumerate(files, 1):
        out = os.path.join(dest, 'original', 'Recoil.exe') if place.lower() == 'recoil.exe' else os.path.join(dest, *place.split('/'))
        os.makedirs(os.path.dirname(out), exist_ok=True)
        with open(out, 'wb') as f:
            f.write(s.game.read(rel))
        progress(i, total)
    log('game files: %d copied (the original Recoil.exe is in original\\)' % len(files))
    if s.audio:
        os.makedirs(os.path.join(dest, 'music'), exist_ok=True)
        for j, t in enumerate(s.audio, 1):
            t.save_wav(os.path.join(dest, 'music', 'track%02d.wav' % t.number))
            progress(len(files) + j, total)
        log('CD music: %d tracks saved as music\\trackNN.wav' % len(s.audio))


def install_remake(payload, dest, log):
    if not payload or not os.path.exists(payload):
        raise SetupError('the remake program (payload\\%s) is missing from this download' % EXE_NAME)
    target = os.path.join(dest, EXE_NAME)
    shutil.copyfile(payload, target)
    n = copy_resources(target, os.path.join(dest, 'original', 'Recoil.exe'))
    log('%s installed (%d menus, dialogs and icons copied from your Recoil.exe)' % (EXE_NAME, n))
    return target


def make_shortcut(target, log):
    desktop = os.path.join(os.environ.get('USERPROFILE', ''), 'Desktop')
    lnk = os.path.join(desktop, 'Recoil Remake.lnk')
    ps = ("$s=(New-Object -ComObject WScript.Shell).CreateShortcut($env:RL_LNK); $s.TargetPath=$env:RL_TGT; "
          "$s.WorkingDirectory=$env:RL_DIR; $s.IconLocation=$env:RL_TGT + ',0'; $s.Save()")
    env = dict(os.environ, RL_LNK=lnk, RL_TGT=target, RL_DIR=os.path.dirname(target))
    r = subprocess.run(['powershell', '-NoProfile', '-NonInteractive', '-Command', ps], env=env, capture_output=True,
                       creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    log('desktop shortcut: %s' % ('Recoil Remake' if r.returncode == 0 else 'could not be created (start %s from the install folder)' % EXE_NAME))


def install(source, dgv_zip, dest, payload=None, shortcut=True, log=print, progress=lambda i, n: None):
    payload = payload or default_payload()
    dgvoodoo_members(dgv_zip)            # fail before copying anything
    s, lines = check_source(source)
    for l in lines:
        log(l)
    os.makedirs(dest, exist_ok=True)
    install_game(s, dest, log, progress)
    install_dgvoodoo(dgv_zip, dest, log)
    target = install_remake(payload, dest, log)
    if shortcut:
        make_shortcut(target, log)
    log('done - start Recoil Remake from the desktop shortcut or %s' % target)
    return target


if __name__ == '__main__':
    a = sys.argv[1:]

    def opt(name):
        return a[a.index(name) + 1] if name in a else None
    if not (opt('--source') and opt('--dgvoodoo') and opt('--dest')):
        print(__doc__)
        sys.exit(1)
    try:
        install(opt('--source'), opt('--dgvoodoo'), opt('--dest'), opt('--payload'), '--no-shortcut' not in a)
    except SetupError as e:
        sys.exit('setup: %s' % e)
