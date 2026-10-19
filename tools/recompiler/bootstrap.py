"""Makes capstone importable for the recompiler without needing pip or administrator rights.

An installed capstone is used as is. Otherwise the pinned version from requirements.txt goes into <repo>/.pydeps:
with pip when Python has it, else the matching wheel is downloaded from PyPI, checked against PyPI's SHA-256 and
unpacked there (a wheel is a zip archive).
"""
import hashlib
import importlib
import io
import json
import os
import platform
import subprocess
import sys
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
TARGET = os.path.normpath(os.path.join(HERE, '..', '..', '.pydeps'))


def pinned_version():
    with open(os.path.join(HERE, 'requirements.txt'), encoding='utf-8') as f:
        for line in f:
            name, _, version = line.strip().partition('==')
            if name == 'capstone':
                return version
    raise RuntimeError('capstone is not pinned in requirements.txt')


def importable():
    # The folder may have been filled since the last attempt: drop the cached directory listings.
    importlib.invalidate_caches()
    try:
        import capstone  # noqa: F401
        return True
    except ImportError:
        return False


def wheel_matches(filename):
    """True for a capstone wheel that runs on this machine."""
    machine = platform.machine().lower()
    arm = machine in ('arm64', 'aarch64')
    if sys.platform.startswith('win'):
        return filename.endswith('win_amd64.whl') if not arm else filename.endswith('win_arm64.whl')
    if sys.platform == 'darwin':
        return 'macosx' in filename and (('arm64' in filename or 'universal2' in filename) if arm
                                         else ('x86_64' in filename or 'universal2' in filename))
    return 'manylinux' in filename and (('aarch64' in filename) if arm else ('x86_64' in filename))


def download(version):
    with urllib.request.urlopen('https://pypi.org/pypi/capstone/%s/json' % version, timeout=60) as response:
        release = json.load(response)
    wheels = [f for f in release['urls'] if f['packagetype'] == 'bdist_wheel' and wheel_matches(f['filename'])]
    if not wheels:
        raise RuntimeError('no capstone %s wheel for %s %s on PyPI' % (version, sys.platform, platform.machine()))
    wheel = wheels[0]
    print('Downloading %s...' % wheel['filename'])
    with urllib.request.urlopen(wheel['url'], timeout=300) as response:
        data = response.read()
    if hashlib.sha256(data).hexdigest() != wheel['digests']['sha256']:
        raise RuntimeError('%s: SHA-256 does not match PyPI' % wheel['filename'])
    os.makedirs(TARGET, exist_ok=True)
    zipfile.ZipFile(io.BytesIO(data)).extractall(TARGET)


def ensure_capstone():
    if TARGET not in sys.path:
        sys.path.insert(0, TARGET)
    if importable():
        return
    version = pinned_version()
    print('capstone not found: installing capstone %s into %s' % (version, TARGET))
    pip = subprocess.run([sys.executable, '-m', 'pip', 'install', '--quiet', '--target', TARGET,
                          'capstone==%s' % version], capture_output=True, text=True)
    if pip.returncode != 0 or not importable():
        download(version)
    if not importable():
        raise RuntimeError('capstone could not be installed into %s' % TARGET)


if __name__ == '__main__':
    ensure_capstone()
    import capstone
    print('capstone %s' % capstone.__version__)
