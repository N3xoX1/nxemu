import math, random
import numpy as np

def pair_sample(start,end,i):
    w0=max(0.0,min(end,i+1)-max(start,i))
    w1=max(0.0,min(end,i+2)-max(start,i+1))
    w=w0+w1
    return i+0.5+(w1/max(w,1e-8)),w

def reference(src, start, end):
    h,w=src.shape[:2]
    acc=0.0
    for y in range(math.floor(start[1]),math.ceil(end[1])):
        wy=max(0.0,min(end[1],y+1)-max(start[1],y))
        for x in range(math.floor(start[0]),math.ceil(end[0])):
            wx=max(0.0,min(end[0],x+1)-max(start[0],x))
            acc+=wx*wy*src[max(0,min(h-1,y)), max(0,min(w-1,x))]
    return acc/((end[0]-start[0])*(end[1]-start[1]))

def bilinear(src,x,y):
    h,w=src.shape[:2]
    x-=0.5;y-=0.5
    ix=math.floor(x);iy=math.floor(y); fx=x-ix;fy=y-iy
    def get(a,b):return src[max(0,min(h-1,b)),max(0,min(w-1,a))]
    return (get(ix,iy)*(1-fx)*(1-fy)+get(ix+1,iy)*fx*(1-fy)+
            get(ix,iy+1)*(1-fx)*fy+get(ix+1,iy+1)*fx*fy)

def optimized(src,start,end):
    first=(math.floor(start[0]),math.floor(start[1]))
    count=(math.ceil(end[0])-first[0],math.ceil(end[1])-first[1])
    if max(count)<=2:
        x,_=pair_sample(start[0],end[0],first[0]);y,_=pair_sample(start[1],end[1],first[1])
        return bilinear(src,x,y),1
    acc=0.;num=0
    for y in range(first[1],first[1]+count[1],2):
        py,wy=pair_sample(start[1],end[1],y)
        for x in range(first[0],first[0]+count[0],2):
            px,wx=pair_sample(start[0],end[0],x)
            acc+=wx*wy*bilinear(src,px,py);num+=1
    return acc/((end[0]-start[0])*(end[1]-start[1])),num

rng=random.Random(4382)
src=np.array([[rng.random() for _ in range(25)] for _ in range(24)],dtype=float)
cases=0;mx=0.;sum_eden=0;sum_fast=0;cnt1=0
for width in [0.2,0.5,0.75,0.99,1.,1.3,1.5,1.9,2.,2.3,3,3.3,4.,5.1,7.,10.2]:
 for height in [0.4,0.75,1.,1.6,2.,2.5,3.,4.1,5.5]:
  for _ in range(50):
   x=rng.uniform(-1,23);y=rng.uniform(-1,22)
   a=(x,y);b=(x+width,y+height)
   expected=reference(src,a,b)
   got,fetch=optimized(src,a,b)
   err=abs(expected-got)
   mx=max(mx,err);cases+=1
   sum_fast+=fetch;sum_eden+=(math.ceil(b[0])-math.floor(x))*(math.ceil(b[1])-math.floor(y))
   cnt1+= fetch==1
   assert err<1e-10,(a,b,expected,got,err)
print('OK: ',cases,'random area / crop / out-of-bounds CPU analog cases')
print('Max absolute error:',mx)
print('Single-fetch cases:',cnt1)
print('Reference texel reads:',sum_eden,'optimized texture reads:',sum_fast,'reduction:',round((1-sum_fast/sum_eden)*100,2),'%')
