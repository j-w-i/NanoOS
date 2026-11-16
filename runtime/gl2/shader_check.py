#!/usr/bin/env python3
"""Compile GLSL ES 1.00 shader sources with the NanoOS GL2 compilers on the host and report
what the device will do with each one: compile errors, whether a fragment shader gets a native
(TSP combiner) plan or only the slow CPU path, approximations, and vertex program length.

  python3 NanoOS/runtime/gl2/shader_check.py shaders/*_vs.glsl shaders/*_fs.glsl

A file is treated as a vertex shader when its name contains "_vs" or ends in .vert, otherwise
as a fragment shader. Feed it the exact sources your app passes to glShaderSource (headers and
#defines included); OpenLara dumps them with tools/host_ref (LARA_DUMP_SHADERS).
"""
from pathlib import Path
import subprocess, sys, tempfile

ROOT = Path(__file__).resolve().parent
HARNESS = r'''
#include "gl2api.c"
#include <stdio.h>
#include <string.h>
static char src[65536], pp[65536];
int main(int argc, char **argv) {
  int bad = 0;
  for (int a = 1; a < argc; ++a) {
    FILE *f = fopen(argv[a], "rb"); if (!f) { printf("%s: cannot open\n", argv[a]); bad = 1; continue; }
    size_t n = fread(src, 1, sizeof src - 1, f); src[n] = 0; fclose(f);
    const char *base = strrchr(argv[a], '/'); base = base ? base + 1 : argv[a];
    int vs = strstr(base, "_vs") || strstr(base, ".vert");
    if (gl2_preprocess(src, pp, sizeof pp)) { printf("%-28s FAIL preprocess\n", base); bad = 1; continue; }
    if (vs) {
      static vsc_result r; int rc = gl2vsc_compile(pp, &r);
      if (rc) { printf("%-28s FAIL vs: %s\n", base, r.err); bad = 1; }
      else printf("%-28s ok   vs: %u VGP instructions, %u literals\n", base, r.ncode, r.nlit);
    } else {
      static gl2_fs_owned o; char log[64] = {0}; int rc = compile_fragment(pp, &o, log);
      if (rc) { printf("%-28s FAIL fs: %s\n", base, log); bad = 1; continue; }
      printf("%-28s ok   fs: %s%s%s\n", base, o.e.nplan ? (o.e.nplan > 1 ? "native, 2 stages" : "native, 1 stage") : "CPU only (GPU frames reject its draws)",
             o.e.uv_approx ? ", approximated texture lookups" : "",
             (o.e.fold_mask || o.e.fold_const[0] || o.e.fold_const[1] || o.e.fold_const[2] || o.e.uv_divide) ? ", per-vertex folds" : "");
    }
  }
  return bad;
}
'''


def main():
    if len(sys.argv) < 2:
        print(__doc__); return 2
    with tempfile.TemporaryDirectory() as d:
        c = Path(d) / 'shader_check.c'; c.write_text(HARNESS); exe = Path(d) / 'shader_check'
        subprocess.run(['cc', '-std=c99', '-O1', '-w', '-I' + str(ROOT), '-include', str(ROOT / 'std/gl2prefix.h'), str(c),
                        str(ROOT / 'gl2rt.c'), str(ROOT / 'gl2vsc.c'), str(ROOT / 'gl2tex.c'), '-lm', '-o', str(exe)], check=True)
        return subprocess.run([str(exe), *sys.argv[1:]]).returncode


if __name__ == '__main__':
    sys.exit(main())
