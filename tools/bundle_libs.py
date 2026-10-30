"""Copy the shared libraries a Linux build ships next to it, so it starts where the system lacks them (make bundle).

usage: python tools/bundle_libs.py <out dir> <glibc max> <program>... --libs <soname>... [--licenses <dir>]

Each soname (libSDL3.so.0: Ubuntu 24.04 and its derivatives have no SDL3, Debian 13 and SteamOS an SDL 3.2 without
SDL_SavePNG) is found where the dynamic linker finds it for the first program that needs it (ldd), and copied to
<out dir> under that name when missing or different. The programs are linked with the run path $ORIGIN (Makefile
LDFLAGS), so the copy beside them is used first, and the system's is never needed. A library that needs a glibc
newer than <glibc max> (the programs' own limit: Makefile GLIBC_CHECK) gets a warning.

--licenses: the license files of the package each library comes from go to <dir>/<package>/, and <dir>/BUNDLED.txt
lists each library with its package, version and where its source is. Read from pacman (Arch, where releases are
made) or dpkg.
"""
import filecmp
import os
import re
import shutil
import subprocess
import sys


def run(*cmd):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, check=False).stdout
    except FileNotFoundError:
        return ''


def find(programs, soname):
    """Where the dynamic linker finds soname for the first program that needs it."""
    for p in programs:
        m = re.search(r'^\s*' + re.escape(soname) + r' => (\S+)', run('ldd', p), re.M)
        if m and m.group(1) != 'not':
            return os.path.realpath(m.group(1))
    return None


def newest_glibc(path):
    """The newest glibc version (major, minor) the library's symbols need, or None."""
    versions = [tuple(map(int, v.split('.')[:2])) for v in re.findall(r'GLIBC_(\d+\.\d+(?:\.\d+)?)', run('objdump', '-T', path))]
    return max(versions) if versions else None


def package(path):
    """(name, version, license files, where its source is) of the package that installed path, or None."""
    name = run('pacman', '-Qqo', path).strip()
    if name:
        version = run('pacman', '-Q', name).split()[-1]
        folder = f'/usr/share/licenses/{name}'
        files = [os.path.join(folder, n) for n in sorted(os.listdir(folder))] if os.path.isdir(folder) else []
        return name, version, files, f'https://archlinux.org/packages/?q={name} (source: its PKGBUILD)'
    out = run('dpkg', '-S', path).strip()
    if out:
        name = out.split(':')[0]
        version = run('dpkg-query', '-W', '-f=${Version}', name).strip()
        copyright = f'/usr/share/doc/{name}/copyright'
        return name, version, [copyright] if os.path.isfile(copyright) else [], f'apt-get source {name}={version}'
    return None


def main():
    args = sys.argv[1:]
    licenses = None
    if '--licenses' in args:
        i = args.index('--licenses')
        licenses = args[i + 1]
        del args[i:i + 2]
    if '--libs' not in args:
        sys.exit(__doc__)
    i = args.index('--libs')
    libs = args[i + 1:]
    out, glibc_max, programs = args[0], tuple(map(int, args[1].split('.')[:2])), args[2:i]
    listed = []
    for soname in libs:
        src = find(programs, soname)
        if not src:
            sys.exit(f'bundle: none of {", ".join(programs)} finds {soname} (is it installed?)')
        need = newest_glibc(src)
        if need and need > glibc_max:  # (a warning: a development build still runs here; a release must not ship it)
            print(f'bundle: WARNING: {src} needs glibc {need[0]}.{need[1]}, newer than {glibc_max[0]}.{glibc_max[1]} '
                  f'(GLIBC_MAX): a release needs one built against an older glibc')
        dst = os.path.join(out, soname)
        if not os.path.isfile(dst) or not filecmp.cmp(src, dst, shallow=False):
            shutil.copyfile(src, dst + '.tmp')
            os.chmod(dst + '.tmp', 0o755)
            os.replace(dst + '.tmp', dst)
            print(f'bundle: {soname} <- {src}')
        if licenses is None:
            continue
        pkg = package(src)
        if not pkg:
            print(f'bundle: warning: no package owns {src}: add its license to {licenses} by hand')
            listed.append(f'{soname}: from {src} (no package information)')
            continue
        name, version, files, source = pkg
        if not files:
            print(f'bundle: warning: package {name} has no license files: add them to {licenses}/{name} by hand')
        os.makedirs(os.path.join(licenses, name), exist_ok=True)
        for f in files:
            if os.path.isfile(f):
                shutil.copyfile(f, os.path.join(licenses, name, os.path.basename(f)))
        listed.append(f'{soname}: package {name} {version}; licenses in {name}/; source: {source}')
    if licenses is not None:
        os.makedirs(licenses, exist_ok=True)
        with open(os.path.join(licenses, 'BUNDLED.txt'), 'w', newline='\n') as f:
            f.write('Shared libraries shipped beside the program (tools/bundle_libs.py), each under its own license:\n\n')
            f.write(''.join(line + '\n' for line in listed))


if __name__ == '__main__':
    main()
