"""Compile and run the actual firmware merge helper with fake profiles only."""
import argparse
import os
import subprocess
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--zig',type=Path)
    parser.add_argument('--compiler',default='c++')
    args=parser.parse_args()
    folder=ROOT/'android/.build-tools/account-patch-test'
    folder.mkdir(parents=True,exist_ok=True)
    executable=folder/'account_patch.exe'
    command=[str(args.zig.resolve()),'c++'] if args.zig else [args.compiler]
    command+=['-std=c++17','-O0','-I'+str(ROOT/'firmware/include'),'-I'+str(ROOT/'firmware/.pio/libdeps/esp32-s3-devkitc-1/ArduinoJson/src'),str(ROOT/'firmware/tests/account_patch_test.cpp'),'-o',str(executable)]
    env=os.environ.copy()
    for name in ('TEMP','TMP','ZIG_GLOBAL_CACHE_DIR','ZIG_LOCAL_CACHE_DIR'):
        location=folder/name;location.mkdir(exist_ok=True);env[name]=str(location)
    subprocess.run(command,check=True,env=env)
    subprocess.run([str(executable)],check=True,env=env)
    print('PASS: production account patch preserves blanks, secrets, location and disabled state; invalid fields rejected')

if __name__=='__main__':main()
