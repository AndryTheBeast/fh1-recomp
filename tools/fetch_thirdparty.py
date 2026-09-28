"""Fetches the third-party sources of the SDK into sdk/thirdparty.

sdk/thirdparty only carries the files this port changed (see THIRD_PARTY_NOTICES.md). Everything else comes from the
ReXGlue SDK release this port is based on and its submodules. This script clones that release with its submodules
into a temporary folder and copies every file that sdk/thirdparty does not have yet; files already there are kept.

Usage: python tools/fetch_thirdparty.py
Needs git.
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


def resolve_link_stub(path, limit=8):
    """On Windows, git checks symlinks out as small text files holding the target path.
    Follow such stubs (and real symlinks) to the file they point to; None if it is not one."""
    for _ in range(limit):
        if os.path.islink(path):
            target = os.path.join(os.path.dirname(path), os.readlink(path))
        else:
            try:
                if os.path.getsize(path) > 400:
                    return path
                text = open(path, 'rb').read().decode('utf-8')
            except (OSError, UnicodeDecodeError):
                return path
            if '\n' in text.strip() or not text.strip() or text.strip().startswith(('#', '/*', '//')):
                return path
            target = os.path.join(os.path.dirname(path), text.strip())
        target = os.path.normpath(target)
        if not os.path.isfile(target) and not os.path.islink(target):
            return path if not os.path.islink(path) else None
        path = target
    return path


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
        copied = kept = repaired = 0
        for dirpath, dirnames, filenames in os.walk(source):
            dirnames[:] = [d for d in dirnames if d != '.git']
            for name in filenames:
                if name == '.git':
                    continue
                src = os.path.join(dirpath, name)
                dst = os.path.join(target, os.path.relpath(src, source))
                real = resolve_link_stub(src)
                if real is None:
                    continue  # dangling link (MoltenVK headers outside macOS)
                if real != src and os.path.isfile(dst) and not os.path.islink(dst) \
                        and os.path.getsize(dst) <= 400 and os.path.getsize(real) > 400:
                    # A stub copied by an earlier run on Windows: replace it with the real file.
                    shutil.copyfile(real, dst)
                    repaired += 1
                    continue
                if os.path.lexists(dst):
                    kept += 1
                    continue
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                shutil.copyfile(real, dst)
                copied += 1
        # sdk/thirdparty/CMakeLists.txt checks each submodule for a .git entry. Mark the folders we filled.
        for name in os.listdir(source):
            if os.path.exists(os.path.join(source, name, '.git')):
                marker = os.path.join(target, name, '.git')
                if os.path.isdir(os.path.join(target, name)) and not os.path.lexists(marker):
                    open(marker, 'w').write('gitdir: fetched-by-tools/fetch_thirdparty.py\n')
        print(f'{copied} files copied into sdk/thirdparty, {kept} files of this port kept, {repaired} link stubs repaired')
    finally:
        if sys.version_info >= (3, 12):
            shutil.rmtree(work, onexc=remove_readonly)
        else:
            shutil.rmtree(work, onerror=remove_readonly)


if __name__ == '__main__':
    main()
