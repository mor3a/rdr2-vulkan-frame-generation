#include "../src/frame_association.h"
#include <cstdio>
int main(){fgvk::FrameAssociation a;
 a.Record(1,7);a.Submit(100,{1},{},{20});if(a.Present({20})!=7)return 1;
 a.Record(2,8);if(a.Present({20})!=7)return 2;
 a.Submit(100,{}, {},{21});if(a.Present({21})!=7)return 3;
 a.Submit(200,{}, {21},{22});if(a.Present({22})!=7)return 4;
 a.Submit(200,{}, {},{23});if(a.Present({23})!=7)return 5;
 a.Execute(3,{2});a.Submit(100,{3},{},{24});if(a.Present({24})!=8)return 6;
 a.Retire(7);if(a.Present({20})!=0 || a.Present({24})!=8)return 7;
 a.Forget(3);if(a.CommandToken(3))return 8;
 a.Forget(2);if(a.CommandToken(2))return 9;
 a.Submit(300,{999},{},{24});if(a.Present({24}))return 10;
 a.Execute(5,{6});a.Record(6,9);a.Submit(400,{5},{},{30});if(a.Present({30})!=9)return 11;
 a.Record(7,10);a.Submit(400,{7},{},{31});if(a.Present({30})!=9 || a.Present({31})!=10)return 12;
 puts("GPU association: secondary buffers, queue ordering, semaphore dependencies, reset/reuse: PASS");}
