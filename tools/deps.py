"""Downloads the pinned VitaSDK and LLVM into <repo>/.deps and prints the folder.

Usage: python -I tools/deps.py vitasdk|llvm

Each archive is checked against its SHA-256 before it is unpacked. A finished folder holds a .installed file with
that hash; the download is skipped while it matches. Of LLVM only the tools the build runs and clang's own headers
are unpacked.
"""
import hashlib
import os
import shutil
import sys
import tarfile
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
DEPS = os.path.normpath(os.path.join(HERE, '..', '.deps'))

PACKAGES = {
    'vitasdk': {
        'url': 'https://github.com/vitasdk/autobuilds/releases/download/sdk-snapshot-20261015.850.1/'
               'vitasdk-x86_64-w64-mingw32-2026-10-15_05-17-40.tar.bz2',
        'sha256': 'bc7af73fa8c873ecc9f6ab89239e449bf94c2912231f62a6151ba2a13ff45326',
        'folder': 'vitasdk',
    },
    'llvm': {
        'url': 'https://github.com/llvm/llvm-project/releases/download/llvmorg-22.1.8/'
               'clang+llvm-22.1.8-x86_64-pc-windows-msvc.tar.xz',
        'sha256': 'd96c2cc1736f4eb7fa43cb9bbdf56d93551a9ae0a9aadb9c99c3c3b2b712a234',
        'folder': 'llvm-22.1.8',
    },
}

LLVM_TOOLS = {'clang.exe', 'clang++.exe', 'ld.lld.exe', 'lld.exe', 'llvm-ar.exe', 'llvm-ranlib.exe',
              'llvm-profdata.exe', 'llvm-nm.exe', 'llvm-objcopy.exe', 'llvm-readelf.exe'}


def download(url, sha256, path):
    if os.path.exists(path) and file_hash(path) == sha256:
        return
    os.makedirs(os.path.dirname(path), exist_ok=True)
    part = path + '.part'
    print('Downloading ' + url, file=sys.stderr)
    digest = hashlib.sha256()
    with urllib.request.urlopen(url) as response, open(part, 'wb') as out:
        total = int(response.headers.get('Content-Length') or 0)
        done, shown = 0, -1
        while True:
            block = response.read(1 << 20)
            if not block:
                break
            out.write(block)
            digest.update(block)
            done += len(block)
            if total and done * 10 // total != shown:
                shown = done * 10 // total
                print('  %d %% of %d MB' % (shown * 10, total >> 20), file=sys.stderr)
    if digest.hexdigest() != sha256:
        os.remove(part)
        raise RuntimeError('SHA-256 mismatch for ' + url)
    os.replace(part, path)


def file_hash(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def strip_top(name):
    parts = name.replace('\\', '/').split('/', 1)
    return parts[1] if len(parts) == 2 else ''


def unpack_vitasdk(archive, target):
    with tarfile.open(archive, 'r:bz2') as tar:
        members = []
        for member in tar.getmembers():
            member.name = strip_top(member.name)
            if member.name:
                members.append(member)
        if hasattr(tarfile, 'data_filter'):
            tar.extractall(target, members=members, filter='data')
        else:
            tar.extractall(target, members=members)


def unpack_llvm(archive, target):
    with tarfile.open(archive, 'r|xz') as tar:
        for member in tar:
            name = strip_top(member.name)
            keep = (name.startswith('bin/') and name[4:] in LLVM_TOOLS) or name.startswith('lib/clang/')
            if not keep or not member.isfile():
                continue
            path = os.path.join(target, *name.split('/'))
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with tar.extractfile(member) as src, open(path, 'wb') as out:
                shutil.copyfileobj(src, out)
    clangxx = os.path.join(target, 'bin', 'clang++.exe')
    if not os.path.exists(clangxx):
        shutil.copyfile(os.path.join(target, 'bin', 'clang.exe'), clangxx)


def install(name):
    package = PACKAGES[name]
    target = os.path.join(DEPS, package['folder'])
    marker = os.path.join(target, '.installed')
    if os.path.exists(marker) and open(marker).read().strip() == package['sha256']:
        return target
    archive = os.path.join(DEPS, 'downloads', package['url'].rsplit('/', 1)[1])
    download(package['url'], package['sha256'], archive)
    print('Unpacking ' + os.path.basename(archive), file=sys.stderr)
    if os.path.exists(target):
        shutil.rmtree(target)
    os.makedirs(target)
    (unpack_vitasdk if name == 'vitasdk' else unpack_llvm)(archive, target)
    with open(marker, 'w') as f:
        f.write(package['sha256'] + '\n')
    os.remove(archive)
    return target


if __name__ == '__main__':
    if len(sys.argv) != 2 or sys.argv[1] not in PACKAGES:
        sys.exit(__doc__)
    print(install(sys.argv[1]))
