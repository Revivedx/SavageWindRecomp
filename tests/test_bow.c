#include <assert.h>
#include <stdio.h>
#include "../runtime/pc_bow.h"
int main(void) {
    const uint64_t zr=1ull<<9, cancel=1ull<<1;
    for(int fps=30;fps<=120;fps*=2) {
        PcBow b={0}; uint64_t t=1000000000, step=1000000000/fps;
        pc_bow_aim(&b,true,t);
        assert(pc_bow_buttons(&b,t)==zr);
        // Merely aiming doesn't fire, however often the game polls.
        for(int i=0;i<fps;i++) assert(pc_bow_buttons(&b,t+i*step)==zr);
        t+=1000000000;pc_bow_fire(&b,t);
        assert(pc_bow_buttons(&b,t)==0);
        assert(pc_bow_buttons(&b,t+150000000)==zr);
        // Releasing aim cancels with B before ZR goes up, then keeps B through the release.
        t+=500000000;pc_bow_aim(&b,false,t);
        bool saw_cancel=false,saw_settle=false;
        for(int i=0;i<fps;i++) {
            uint64_t v=pc_bow_buttons(&b,t+i*step);
            if(v== (zr|cancel)) saw_cancel=true;
            if(v==cancel) {assert(saw_cancel);saw_settle=true;}
            if(v==0) assert(saw_settle);
        }
        assert(saw_cancel && saw_settle && b.phase==PC_BOW_IDLE);
        printf("%d FPS: aim, click-shot, redraw, cancel-before-trigger-release PASS\n",fps);
    }
}
