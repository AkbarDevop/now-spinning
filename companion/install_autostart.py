#!/usr/bin/env python3
"""Install/remove this project's per-user macOS helper. Never starts Chrome."""
import argparse
import os
from pathlib import Path
import plistlib
import subprocess
import sys
import shutil
import serial

ROOT=Path(__file__).resolve().parents[1]
LABEL='one.akbar.music-matrix'
PLIST=Path.home()/'Library/LaunchAgents'/f'{LABEL}.plist'
INSTALLED=Path.home()/'Library/Application Support/Akbar Matrix'

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--remove',action='store_true');args=parser.parse_args()
    domain=f'gui/{os.getuid()}'
    if args.remove:
        subprocess.run(['launchctl','bootout',f'{domain}/{LABEL}'],check=False,capture_output=True)
        PLIST.unlink(missing_ok=True);print('Music Matrix automatic startup removed.');return
    # Run outside Documents so login startup does not depend on Terminal/Codex
    # retaining access to protected project folders. Bundle only this helper.
    companion=INSTALLED/'companion';companion.mkdir(parents=True,exist_ok=True,mode=0o700)
    INSTALLED.chmod(0o700)
    for name in ('bridge.py','setup.html','pairing-token.txt'):
        shutil.copy2(ROOT/'companion'/name,companion/name)
    (companion/'pairing-token.txt').chmod(0o600)
    shutil.copytree(Path(serial.__file__).parent,companion/'serial',dirs_exist_ok=True,ignore=shutil.ignore_patterns('__pycache__','*.pyc'))
    if (ROOT/'companion/device.json').exists() and not (companion/'device.json').exists():
        shutil.copy2(ROOT/'companion/device.json',companion/'device.json')
    logdir=INSTALLED/'logs';logdir.mkdir(exist_ok=True,mode=0o700)
    settings={'Label':LABEL,'ProgramArguments':[str(Path(sys._base_executable).resolve()),str(companion/'bridge.py')],
              'WorkingDirectory':str(INSTALLED),'RunAtLoad':True,'KeepAlive':True,'ThrottleInterval':10,
              'StandardOutPath':str(logdir/'helper.log'),'StandardErrorPath':str(logdir/'helper-error.log')}
    PLIST.parent.mkdir(parents=True,exist_ok=True)
    PLIST.write_bytes(plistlib.dumps(settings));PLIST.chmod(0o600)
    subprocess.run(['launchctl','bootout',f'{domain}/{LABEL}'],check=False,capture_output=True)
    subprocess.run(['launchctl','bootstrap',domain,str(PLIST)],check=True)
    print('Music Matrix helper starts automatically at login.')

if __name__=='__main__':main()
