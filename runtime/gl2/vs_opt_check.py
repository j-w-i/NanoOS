#!/usr/bin/env python3
"""Prove the GPU vertex-code optimisations exact on real shaders (see vs_opt_check.c).

  python3 NanoOS/runtime/gl2/vs_opt_check.py a_vs.glsl a_fs.glsl [b_vs.glsl b_fs.glsl ...]

Pass the sources exactly as the app gives them to glShaderSource (OpenLara: dump them with
tools/host_ref built with -DLARA_DUMP_SHADERS). Exit status 1 on any mismatch."""
from pathlib import Path
import subprocess, sys, tempfile
ROOT = Path(__file__).resolve().parent
def main():
    if len(sys.argv) < 3 or len(sys.argv) % 2 == 0:
        print(__doc__); return 2
    with tempfile.TemporaryDirectory() as d:
        exe = Path(d) / 'vs_opt_check'
        subprocess.run(['cc', '-std=c99', '-O1', '-w', '-DGL2_MAX_SHADERS=32', '-DGL2_MAX_PROGRAMS=32', '-DGL2_SHADER_SOURCE_BYTES=16384',
                        '-I' + str(ROOT), '-include', str(ROOT / 'std/gl2prefix.h'), str(ROOT / 'vs_opt_check.c'),
                        str(ROOT / 'gl2rt.c'), str(ROOT / 'gl2vsc.c'), str(ROOT / 'gl2tex.c'), '-lm', '-o', str(exe)], check=True)
        return subprocess.run([str(exe), *sys.argv[1:]]).returncode
if __name__ == '__main__':
    sys.exit(main())
