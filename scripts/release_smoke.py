"""Verify the user download can start from a freshly extracted Windows ZIP."""
import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile
import zipfile


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument('archive', type=Path)
    args = parser.parse_args()
    archive_path = args.archive.resolve()
    with tempfile.TemporaryDirectory(prefix='mora-release-') as temp:
        folder = Path(temp)
        with zipfile.ZipFile(archive_path) as archive:
            names = set(archive.namelist())
            required = {'mora.exe', 'mora.cmd', 'install.cmd', 'install.ps1', 'README.md'}
            assert required <= names, f'missing release files: {required - names}'
            assert not any(name.startswith(('src/', 'tests/', 'assets/')) for name in names)
            archive.extractall(folder)
        exe = folder / 'mora.exe'
        from_cmd = subprocess.run(['cmd', '/d', '/c', r'.\mora.cmd', '--version'],
                                  cwd=folder, capture_output=True, text=True, timeout=15)
        direct = subprocess.run([str(exe), '--version'], cwd=folder,
                                capture_output=True, text=True, timeout=15)
        assert from_cmd.returncode == direct.returncode == 0
        assert from_cmd.stdout == direct.stdout and from_cmd.stdout.startswith('MORA ')
        user_data = folder / 'isolated-user-data'
        commands = folder / 'isolated-commands'
        user_data.mkdir()
        commands.mkdir()
        environment = dict(os.environ, LOCALAPPDATA=str(user_data))
        installed = subprocess.run(
            ['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
             str(folder / 'install.ps1'), '-CommandDir', str(commands)],
            cwd=folder, env=environment, capture_output=True, text=True, timeout=30)
        assert installed.returncode == 0, installed.stderr or installed.stdout
        installed_cmd = subprocess.run(
            ['cmd', '/d', '/c', str(commands / 'mora.cmd'), '--version'],
            cwd=folder, env=environment, capture_output=True, text=True, timeout=15)
        assert installed_cmd.returncode == 0 and installed_cmd.stdout == direct.stdout
        print(f'PASS extracted release: {from_cmd.stdout.strip()}')
        print('PASS isolated install and installed command')
        print(f'EXE SHA256 {hashlib.sha256(exe.read_bytes()).hexdigest().upper()}')


if __name__ == '__main__':
    main()
