#!/usr/bin/env python3
"""Generate an original 256x256 H.264 Baseline I_PCM moving-square clip."""
import argparse
from pathlib import Path

class Bits:
    def __init__(self): self.bits=[]
    def put(self, value, width):
        self.bits.extend((value>>i)&1 for i in range(width-1,-1,-1))
    def ue(self,value):
        width=(value+1).bit_length()
        self.put(0,width-1);self.put(value+1,width)
    def se(self,value): self.ue(-2*value if value<=0 else 2*value-1)
    def align(self):
        while len(self.bits)%8:self.bits.append(0)
    def bytes(self):
        return bytes(sum(self.bits[i+j]<<(7-j) for j in range(8)) for i in range(0,len(self.bits),8))
    def trailing(self): self.put(1,1);self.align();return self.bytes()

def nal(header, rbsp):
    output=bytearray(b'\x00\x00\x00\x01'+bytes([header]));zeros=0
    for value in rbsp:
        if zeros==2 and value<=3:output.append(3);zeros=0
        output.append(value);zeros=zeros+1 if value==0 else 0
    return output

def clip(frames):
    s=Bits();s.put(66,8);s.put(0xc0,8);s.put(30,8)
    for v in (0,0,2,0):s.ue(v)
    s.put(0,1);s.ue(15);s.ue(15);s.put(1,1);s.put(1,1);s.put(0,1);s.put(0,1)
    p=Bits();p.ue(0);p.ue(0);p.put(0,1);p.put(0,1);p.ue(0);p.ue(0);p.ue(0)
    p.put(0,1);p.put(0,2);p.se(0);p.se(0);p.se(0);p.put(1,1);p.put(0,1);p.put(0,1)
    output=nal(0x67,s.trailing())+nal(0x68,p.trailing())
    for frame in range(frames):
        h=Bits();h.ue(0);h.ue(2);h.ue(0);h.put(0,4);h.ue(frame%2)
        h.put(0,1);h.put(0,1);h.se(0);h.ue(1)
        for my in range(16):
            for mx in range(16):
                h.ue(25);h.align()
                for y in range(16):
                    for x in range(16):
                        px,py=mx*16+x,my*16+y
                        inside=(frame*3)%192<=px<(frame*3)%192+64 and 96<=py<160
                        h.put(180 if inside else 32+px//4,8)
                for chroma in (96,192):
                    for _ in range(64):h.put(chroma,8)
        output+=nal(0x65,h.trailing())
    return output

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output',type=Path);parser.add_argument('--frames',type=int,default=60)
    args=parser.parse_args()
    if not 1<=args.frames<=300:parser.error('frames must be between 1 and 300')
    args.output.write_bytes(clip(args.frames))
