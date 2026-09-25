import pathlib
import subprocess
import tempfile
import unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
class SourcePolicyTests(unittest.TestCase):
 def test_paused_heartbeats_never_steal_the_active_source(self):
  source=r'''
#include "source_policy.h"
#include <cassert>
int main(){
 SourcePolicy p;
 assert(p.acceptMac(true,false,100));
 assert(!p.acceptPhone(false,200));
 assert(p.acceptPhone(true,300));
 assert(!p.acceptMac(false,true,400));
 assert(!p.acceptMac(true,true,500));
 // An actually playing Mac may take over when the phone pauses.
 assert(!p.acceptPhone(false,600));
 assert(p.acceptMac(true,false,700));
 assert(p.acceptPhone(true,800));
 assert(!p.acceptMac(false,true,900));
 assert(p.acceptPhone(false,1000));
 for(int i=0;i<100;i++){
  assert(!p.acceptMac(false,false,1100+i*2000));
  assert(p.acceptPhone(false,1101+i*2000));
 }
 assert(p.acceptMac(true,false,210000));
 assert(!p.acceptPhone(false,210001));
}
'''
  with tempfile.TemporaryDirectory() as d:
   d=pathlib.Path(d);(d/'main.cpp').write_text(source)
   subprocess.run(['clang++','-std=c++17','-I',str(ROOT/'album_display'),str(d/'main.cpp'),'-o',str(d/'test')],check=True,capture_output=True)
   subprocess.run([str(d/'test')],check=True,capture_output=True)
