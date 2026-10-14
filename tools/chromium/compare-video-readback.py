#!/usr/bin/env python3
"""Compare captured Vulkan readback with CPU decode of the exact same packets."""
import argparse
import json
import os
from pathlib import Path
import re
import select
import struct
import subprocess
import time

ROOT=Path(__file__).resolve().parents[2]


def packets(data):
    if len(data)>32*1024*1024:
        raise ValueError('Packet capture exceeds bound')
    records=[]
    offset=0
    while offset<len(data):
        if offset+12>len(data):
            raise ValueError('Truncated packet header')
        pts,size=struct.unpack_from('<qI',data,offset)
        offset+=12
        if not 0<size<=4*1024*1024 or offset+size>len(data):
            raise ValueError('Invalid or truncated packet payload')
        if records and pts<=records[-1][0]:
            raise ValueError('This comparison requires monotonic packet PTS; no guessed B-frame alignment')
        payload=data[offset:offset+size]
        types={nal[0]&31 for nal in re.split(b'\x00\x00\x00?\x01',payload) if nal}
        if not types.intersection((1,5)):
            raise ValueError('Expected one Annex-B picture per packet')
        records.append((pts,payload,types))
        offset+=size
        if len(records)>256:
            raise ValueError('Too many captured packets')
    if not records:
        raise ValueError('Empty packet capture')
    return records


def compare_luma(actual,expected):
    if not actual or len(actual)!=len(expected):
        raise ValueError('Mismatched luma plane lengths')
    changes=[abs(a-b) for a,b in zip(actual,expected)]
    return dict(pixels_different=sum(d!=0 for d in changes),
                delta_over_8=sum(d>8 for d in changes),max_delta=max(changes))


def read_frame(stream,size,deadline):
    data=bytearray()
    while len(data)<size:
        remaining=deadline-time.monotonic()
        if remaining<=0 or not select.select([stream],[],[],remaining)[0]:
            raise TimeoutError('Bounded software decode timed out')
        chunk=os.read(stream.fileno(),min(65536,size-len(data)))
        if not chunk:
            if data:
                raise ValueError('Incomplete software frame')
            return b''
        data.extend(chunk)
    return bytes(data)


def run(directory,prefix,ffmpeg,save_reference=False):
    if not re.fullmatch(r'[a-z0-9-]{1,64}',prefix):
        raise ValueError('Invalid filename prefix')
    capture=directory/(prefix+'.packets')
    if capture.stat().st_size>32*1024*1024:
        raise ValueError('Packet capture exceeds bound')
    records=packets(capture.read_bytes())
    start=next((i for i,row in enumerate(records) if {5,7,8}<=row[2]),None)
    if start is None:
        raise ValueError('No self-contained SPS/PPS/IDR packet; cannot compare')
    records=records[start:]
    stream=directory/(prefix+'.h264')
    stream.write_bytes(b''.join(row[1] for row in records))
    text=(directory/(prefix+'-capture.log')).read_text()
    entries=re.findall(r'DECODED_FRAME index=(\d+) pts_us=(\d+) size=(\d+)x(\d+)',text)
    if not entries:
        raise ValueError('No captured hardware frames')
    dimensions={(int(w),int(h)) for _,_,w,h in entries}
    if len(dimensions)!=1:
        raise ValueError('Resolution changes require separate captures')
    width,height=dimensions.pop()
    if not (0<width<=4096 and 0<height<=2160 and width%2==height%2==0):
        raise ValueError('Invalid NV12 dimensions')
    matched={int(pts):int(index) for index,pts,_,_ in entries}
    if len(matched)!=len(entries):
        raise ValueError('Duplicate hardware PTS')
    command=[str(ffmpeg),'-nostdin','-hide_banner','-loglevel','error',
             '-hwaccel','none','-threads','1','-c:v','h264','-f','h264',
             '-i',str(stream),'-pix_fmt','yuv420p','-c:v','rawvideo',
             '-threads','1','-f','rawvideo','-y','/dev/stdout']
    count=0
    results=[]
    with (directory/(prefix+'-software.log')).open('w') as log:
        process=subprocess.Popen(command,stdout=subprocess.PIPE,stderr=log,bufsize=0)
        try:
            deadline=time.monotonic()+30
            while True:
                frame=read_frame(process.stdout,width*height*3//2,deadline)
                if not frame:
                    break
                if count>=len(records):
                    raise ValueError('Software output count exceeds packet count')
                pts=records[count][0]
                if pts in matched:
                    index=matched[pts]
                    pgm=(directory/('%s-%02d.pgm'%(prefix,index))).read_bytes()
                    magic,shape,maximum,actual=pgm.split(b'\n',3)
                    captured_width,rows=map(int,shape.split())
                    if (magic!=b'P5' or maximum!=b'255' or captured_width!=width or
                            not 0<rows<=height or len(actual)!=width*rows):
                        raise ValueError('Invalid captured Y-plane')
                    expected=frame[:width*rows]
                    results.append(dict(index=index,pts_us=pts,**compare_luma(actual,expected)))
                    if save_reference:
                        (directory/('%s-software-%02d.pgm'%(prefix,index))).write_bytes(
                            b'P5\n'+shape+b'\n255\n'+expected)
                count+=1
            status=process.wait(timeout=max(.1,deadline-time.monotonic()))
        finally:
            process.stdout.close()
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)
    if status or count!=len(records) or not results:
        raise ValueError('Software decode failed or frame alignment is incomplete')
    report=dict(command=command,software_frames=count,matched_frames=len(results),frames=results)
    (directory/(prefix+'-comparison.json')).write_text(json.dumps(report,indent=2)+'\n')
    return report


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory',type=Path,required=True)
    parser.add_argument('--prefix',default='video-match')
    parser.add_argument('--ffmpeg',type=Path,default=ROOT/'build/nvidia/vulkan/ffmpeg-7.1.1/ffmpeg')
    parser.add_argument('--save-reference',action='store_true')
    args=parser.parse_args()
    report=run(args.directory.resolve(),args.prefix,args.ffmpeg.resolve(),args.save_reference)
    bad=sum(row['delta_over_8']>0 for row in report['frames'])
    print(json.dumps(dict(software_frames=report['software_frames'],
                          matched_frames=report['matched_frames'],mismatched_frames=bad)))
    raise SystemExit(1 if bad else 0)
