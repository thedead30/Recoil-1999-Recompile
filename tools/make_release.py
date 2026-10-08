"""make_release.py - build the two public downloads into dist/:

  dist/RecoilRemake-Player-<ver>.zip   Setup.exe + payload/Recoil Remake.exe + README.txt + LICENSE.txt  (for players; no Python needed)
  dist/RecoilRemake-Source-<ver>.zip   the source, tools and research ledger from git HEAD  (for developers)

Neither contains anything of Recoil: the player's own copy supplies every game byte. Before writing anything the script
checks that (a fails = no release):
  * the data image in the source is the runtime variant (no section bytes)
  * leak scan: no byte table in the source (a {...} list of 64+ numbers) shares a 32-byte run with Recoil.exe
  * no game files (.zbd .avi .wav .png .bmp .exe .dll .zip ...) and no personal paths / e-mail addresses in the source
  * the player exe was built with RECOIL_PLAYER_RELEASE (no build-machine paths, no Recoil resources)
usage: python tools/make_release.py [--version V] [--exe ORIGINAL_Recoil.exe] [--no-build] [--check] [--draft]
  --check     run the checks only        --no-build  package the existing build\\Player instead of building it
"""
import datetime, io, os, re, subprocess, sys, zipfile

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
DIST = os.path.join(ROOT, 'dist')
PLAYER = os.path.join(ROOT, '05_remake', 'build', 'Player')

# what the public source contains (paths from git HEAD); everything else stays private (RELEASE_AUDIT.md)
INCLUDE = ['05_remake/CMakeLists.txt', '05_remake/build.bat', '05_remake/boot/', '05_remake/setup/', '05_remake/src/',
           '05_remake/tests/', 'tools/', '03_re/ledger/', '03_re/scripts/', '03_re/decomp/', '03_re/WORKFLOW.md',
           '03_re/CAVEATS.md', '04_spec/', '06_tools/', 'CLAUDE.md', 'ROADMAP.md', 'STAGE1.md', 'STAGE2.md', 'GOAL.md',
           'ORIGINAL_DEFECTS.md', 'status.bat', '.gitignore']
EXCLUDE = re.compile(r'(^|/)(_tmp[^/]*|__pycache__/.*|.*\.pyc)$|^tools/asm_port/mutants\.json$')
NEW_DOCS = 'release_docs'   # tools/release_docs/*: README.md, BUILDING.md, CONTRIBUTING.md, LICENSE, NOTICE -> source root
GAME_EXT = re.compile(r'\.(zbd|avi|wav|png|bmp|pcx|tga|exe|dll|zip|bin|iso|nrg|cue|cab|dat|idx|mp3|ogg)$', re.I)


def personal_pattern():
    """this machine's user folder and the git author e-mail - taken from the machine, so no name is written here"""
    terms = [r'@gmail\.com']
    home = os.environ.get('USERPROFILE', '')
    if home:
        user = os.path.basename(home)
        terms += [re.escape('Users\\' + user), re.escape('Users\\\\' + user), re.escape('Users/' + user)]
    try:
        mail = subprocess.run(['git', '-C', ROOT, 'config', 'user.email'], capture_output=True, text=True).stdout.strip()
        if mail:
            terms += [re.escape(mail), re.escape(mail.split('@')[0])]
    except OSError:
        pass
    return re.compile('|'.join(terms).encode(), re.I)


PERSONAL = personal_pattern()
ARRAY = re.compile(rb'\{\s*((?:(?:0x[0-9a-fA-F]+|\d+)\s*,\s*){63,}(?:0x[0-9a-fA-F]+|\d+))\s*,?\s*\}')


def git(*a):
    return subprocess.run(['git', '-C', ROOT] + list(a), capture_output=True, check=True).stdout


def source_files():
    names = git('ls-files', '-z').decode('utf-8').split('\0')
    keep = [n for n in names if n and any(n == i or (i.endswith('/') and n.startswith(i)) for i in INCLUDE) and not EXCLUDE.search(n)]
    return sorted(keep)


def check(files, exe_path):
    problems = []
    exe = open(exe_path, 'rb').read() if exe_path and os.path.exists(exe_path) else None
    chunks = None
    if exe:
        chunks = set()
        for o in range(0, len(exe) - 32, 4):     # every 32-byte run of the original that is not trivial filler
            c = exe[o:o + 32]
            if len(set(c)) > 4:
                chunks.add(c)
    tables = 0
    for n in files:
        data = git('show', 'HEAD:' + n)
        if GAME_EXT.search(n):
            problems.append('game/binary file type: ' + n)
        if PERSONAL.search(data):
            problems.append('personal path or e-mail: ' + n)
        if n.endswith('platform/image/original_data.cpp') and b'RUNTIME variant' not in data:
            problems.append('original_data.cpp is the compiled-in variant (holds Recoil.exe bytes) - regenerate with --runtime')
        if chunks is not None and n.endswith(('.cpp', '.h', '.inc', '.json', '.py')):
            for m in ARRAY.finditer(data):
                try:
                    vals = [int(v, 0) for v in re.findall(rb'0x[0-9a-fA-F]+|\d+', m.group(1))]
                except ValueError:
                    continue
                if max(vals) > 255:
                    continue
                tables += 1
                b = bytes(vals)
                hit = next((i for i in range(0, len(b) - 31) if b[i:i + 32] in chunks), None)
                if hit is not None:
                    problems.append('byte table copied from Recoil.exe (%d bytes, at line %d): %s'
                                    % (len(b), data[:m.start()].count(b'\n') + 1, n))
    if exe is None:
        problems.append('leak scan not run: pass --exe <original Recoil.exe>')
    return problems, tables


def build_player():
    bat = os.path.join(ROOT, 'tools', 'build_player.bat')
    r = subprocess.run(['cmd', '/c', bat], capture_output=True, text=True)
    if r.returncode:
        sys.exit('player build failed:\n' + r.stdout[-3000:] + r.stderr[-2000:])


def original_data_in(path, exe_path):
    """32-byte runs of Recoil.exe's data sections (.rdata/.data/.rsrc; .text is ported code, identical by design) found in
    a built file -> (text bytes, other bytes, other runs). DirectInput GUIDs (Microsoft's, {...-BFC7-444553540000}) are
    not Recoil data and are not counted."""
    import pefile
    o = pefile.PE(exe_path)
    ob = open(exe_path, 'rb').read()
    p = open(path, 'rb').read()
    text = other = 0
    runs_other = []
    for s in o.sections:
        if s.Name.rstrip(b'\0') == b'.text':
            continue
        d = ob[s.PointerToRawData:s.PointerToRawData + s.SizeOfRawData]
        ch = {}
        for i in range(0, len(d) - 32, 4):
            if len(set(d[i:i + 32])) > 6:
                ch.setdefault(d[i:i + 32], i)
        hits = sorted({ch[p[j:j + 32]] for j in range(len(p) - 32) if p[j:j + 32] in ch})
        runs = []
        for h in hits:
            if runs and h <= runs[-1][1] + 4:
                runs[-1][1] = h
            else:
                runs.append([h, h])
        for a, b in runs:
            blob = d[a:b + 32]
            if blob.count(b'\xbf\xc7DEST') * 16 >= len(blob) // 2:
                continue   # DirectInput GUID table
            printable = sum(32 <= c < 127 or c in (0, 9, 10, 13) for c in blob)
            if printable >= 0.9 * len(blob):
                text += len(blob)
            elif len(set(blob)) <= 24 and len(blob) <= 64:
                continue   # a few maths constants (0.0, 1.0, float limits) laid out in the same order
            else:
                other += len(blob)
                runs_other.append('%s+0x%x (%d bytes)' % (s.Name.rstrip(b'\0').decode(), a, len(blob)))
    return text, other, runs_other


def check_player_exe(path, exe_path):
    data = open(path, 'rb').read()
    out = []
    text, other, runs = original_data_in(path, exe_path)
    print('player exe: %d bytes of the original\'s text (kStr_ error messages in ported code), %d bytes of other data' % (text, other))
    if other:
        out.append('player exe holds data copied from Recoil.exe: ' + ', '.join(runs[:10]))
    if PERSONAL.search(data):
        out.append('player exe contains a build-machine path')
    if b'MENU' in data and b'R\x00E\x00C\x00O\x00I\x00L' in data:   # resource text the Setup copies in later
        out.append('player exe already holds Recoil resources (built without RECOIL_PLAYER_RELEASE?)')
    return out


PLAYER_README = """Recoil Remake {ver}
==================

A rebuild of Zipper Interactive's Recoil (1999) for modern Windows. This download contains no part of the game:
you need your own copy of Recoil, the 1999-01-29 build (as on the "Recoil Classic" CD).

Install
-------
1. Run Setup.exe (keep it next to its payload folder).
2. Step 1: download dgVoodoo2 (free, the page opens from Setup; tested with 2.8.7.3 and 2.8.7.5) and select the zip.
3. Step 2: select your copy of Recoil - the installed game folder, a zip of it, the CD, or a disc image
   (.iso, .bin/.cue, .nrg). A full disc image (.nrg or .bin+.cue) also gives the game's CD music.
4. Choose an install folder and click Install. Start "Recoil Remake" from the desktop shortcut.

Recoil is (c) 1999 Electronic Arts / Zipper Interactive. This project is a non-commercial fan rebuild and is
not affiliated with them.

The remake is free software under the GNU General Public License version 3 or later (LICENSE.txt). Its complete
source code is the RecoilRemake-Source download published alongside this one.
"""


def main():
    a = sys.argv[1:]
    ver = a[a.index('--version') + 1] if '--version' in a else datetime.date.today().strftime('%Y.%m.%d')
    exe = a[a.index('--exe') + 1] if '--exe' in a else os.path.join(ROOT, '00_original', 'game_install', 'Recoil.exe')
    files = source_files()
    docs_dir = os.path.join(ROOT, 'tools', NEW_DOCS)
    problems, tables = check(files, exe)
    draft = '--draft' in a
    if not os.path.exists(os.path.join(docs_dir, 'LICENSE')) and not draft:
        problems.append('no LICENSE in tools/release_docs (choose a licence; --draft packages without one, marked DRAFT)')
    if draft:
        ver += '-DRAFT'
    print('source: %d files from git HEAD (%s), %d byte tables scanned against Recoil.exe' % (len(files), git('rev-parse', '--short', 'HEAD').decode().strip(), tables))
    if '--check' not in a:
        if '--no-build' not in a:
            build_player()
        for f in ('Setup.exe', 'recoil_boot.exe'):
            if not os.path.exists(os.path.join(PLAYER, f)):
                problems.append('missing build output ' + f)
        if not problems:
            problems += check_player_exe(os.path.join(PLAYER, 'recoil_boot.exe'), exe)
    if problems:
        print('NOT RELEASED - %d problem(s):' % len(problems))
        for p in problems[:60]:
            print('  ' + p)
        return 1
    print('checks passed')
    if '--check' in a:
        return 0
    os.makedirs(DIST, exist_ok=True)
    pz = os.path.join(DIST, 'RecoilRemake-Player-%s.zip' % ver)
    with zipfile.ZipFile(pz, 'w', zipfile.ZIP_DEFLATED) as z:
        top = 'RecoilRemake-%s/' % ver
        z.write(os.path.join(PLAYER, 'Setup.exe'), top + 'Setup.exe')
        z.write(os.path.join(PLAYER, 'recoil_boot.exe'), top + 'payload/Recoil Remake.exe')
        z.writestr(top + 'README.txt', PLAYER_README.format(ver=ver).replace('\n', '\r\n'))
        z.write(os.path.join(docs_dir, 'LICENSE'), top + 'LICENSE.txt')
    sz = os.path.join(DIST, 'RecoilRemake-Source-%s.zip' % ver)
    with zipfile.ZipFile(sz, 'w', zipfile.ZIP_DEFLATED) as z:
        top = 'RecoilRemake-Source-%s/' % ver
        for n in files:
            z.writestr(top + n, git('show', 'HEAD:' + n))
        if os.path.isdir(docs_dir):
            for fn in sorted(os.listdir(docs_dir)):
                z.write(os.path.join(docs_dir, fn), top + fn)
    for p in (pz, sz):
        print('wrote %s (%.1f MB)' % (os.path.relpath(p, ROOT), os.path.getsize(p) / 1e6))
    return 0


if __name__ == '__main__':
    sys.exit(main())
