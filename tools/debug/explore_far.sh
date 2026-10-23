#!/bin/bash
# Long-stride random walk (prints SAVAGEWIND_INPUT): leaves the starting screen; SEED/STEPS env.
python3 -c "
import random, os
random.seed(int(os.environ.get('SEED','1')))
f=3200; s='400:A:6,520:A:6,900:ZL:8,900:ZR:8,1300:A:6,1600:A:6,1900:A:6,2200:A:6'
dirs=['STICK_LEFT','STICK_DOWN','STICK_RIGHT','STICK_UP']
d=random.choice(dirs)
for i in range(int(os.environ.get('STEPS','80'))):
    if random.random()<0.6: d=random.choice(dirs)
    n=random.randint(150,500)
    s+=f',{f}:{d}:{n}'; f+=n
    if random.random()<0.4: s+=f',{f}:A:6'; f+=30
    if random.random()<0.3: s+=f',{f}:B:6'; f+=30
print(s)"
