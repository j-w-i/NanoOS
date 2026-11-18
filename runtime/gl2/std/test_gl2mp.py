from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory() as temp:
 d=Path(temp);objs=[];big=['-DGL2_MAX_PROGRAMS=24','-DGL2_MAX_SHADERS=8']
 for src in ['gl2api.c','gl2rt.c','gl2vsc.c','gl2tex.c']:
  o=d/(src+'.o');subprocess.run(['cc','-std=c99','-O2','-g','-Wall','-Wextra','-Werror','-I'+str(root),'-include',str(root/'std/gl2prefix.h'),*big,'-c',str(root/src),'-o',str(o)],check=True);objs.append(str(o))
 for src in ['gl2std.c','test_gl2mp_host.c','test_gl2mp.c']:
  o=d/(src+'.o');subprocess.run(['cc','-std=c99','-O2','-g','-Wall','-Wextra','-Werror','-I'+str(root),'-I'+str(root/'std'),*big,'-c',str(root/'std'/src),'-o',str(o)],check=True);objs.append(str(o))
 exe=d/'mp';subprocess.run(['cc',*objs,'-lm','-o',str(exe)],check=True);subprocess.run([str(exe)],check=True)
