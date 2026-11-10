"""Fetches the third-party sources of the SDK into sdk/thirdparty.

sdk/thirdparty only carries the files this project changed (see THIRD_PARTY_NOTICES.md). Everything else comes from
the ReXGlue SDK release the SDK is based on and its submodules. This script clones that release with its submodules
into a temporary folder and copies every file that sdk/thirdparty does not have yet; files already there are kept.

Usage: python tools/fetch_thirdparty.py
Needs git.

Derived from GPL-3.0 code; modified for bo2-zm-nx in 2026.
"""
import os
import shutil
import stat
import subprocess
import sys
import tempfile

UPSTREAM = 'https://github.com/rexglue/rexglue-sdk.git'
COMMIT = 'c94f5ebdcb3c9d1a460ca48e04f9758448f8d518'  # v0.10.0


def run(*args, cwd=None):
    print('>', ' '.join(args))
    subprocess.run(args, cwd=cwd, check=True)


def remove_readonly(func, path, _):
    os.chmod(path, stat.S_IWRITE)
    func(path)


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    target = os.path.join(root, 'sdk', 'thirdparty')
    work = tempfile.mkdtemp(prefix='rexglue-sdk-')
    try:
        run('git', 'init', '-q', work)
        run('git', 'remote', 'add', 'origin', UPSTREAM, cwd=work)
        run('git', 'fetch', '-q', '--depth', '1', 'origin', COMMIT, cwd=work)
        run('git', 'checkout', '-q', 'FETCH_HEAD', cwd=work)
        run('git', 'submodule', 'update', '--init', '--recursive', cwd=work)
        source = os.path.join(work, 'thirdparty')
        copied = kept = 0
        for dirpath, dirnames, filenames in os.walk(source):
            dirnames[:] = [d for d in dirnames if d != '.git']
            # os.walk lists symlinks to directories as dirnames; copy them as links too
            links = [d for d in dirnames if os.path.islink(os.path.join(dirpath, d))]
            dirnames[:] = [d for d in dirnames if d not in links]
            for name in filenames + links:
                if name == '.git':
                    continue
                src = os.path.join(dirpath, name)
                dst = os.path.join(target, os.path.relpath(src, source))
                if os.path.lexists(dst):
                    kept += 1
                    continue
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                if os.path.islink(src):
                    # Some submodules (MoltenVK) ship relative symlinks whose targets are not fetched
                    os.symlink(os.readlink(src), dst)
                else:
                    shutil.copy2(src, dst)
                copied += 1
        print(f'{copied} files copied into sdk/thirdparty, {kept} files of this project kept')
    finally:
        if sys.version_info >= (3, 12):
            shutil.rmtree(work, onexc=remove_readonly)
        else:
            shutil.rmtree(work, onerror=remove_readonly)


if __name__ == '__main__':
    main()
