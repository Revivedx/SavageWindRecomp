#!/bin/bash
# Deterministic input script used for the exploration runs (prints SAVAGEWIND_INPUT)
python3 -c "
import random
import os; random.seed(int(os.environ.get('SEED','7')))
f=2500; s='400:A:6,520:A:6,900:ZL:8,900:ZR:8,1300:A:6,1600:A:6,1900:A:6,2200:A:6'
dirs=['STICK_LEFT','STICK_DOWN','STICK_RIGHT','STICK_UP']
for i in range(int(os.environ.get('STEPS','60'))):
    d=random.choice(dirs); n=random.randint(60,240)
    s+=f',{f}:{d}:{n}'; f+=n
    if random.random()<0.3: s+=f',{f}:A:6'; f+=20
    if random.random()<0.2: s+=f',{f}:B:6'; f+=20
print(s)"
