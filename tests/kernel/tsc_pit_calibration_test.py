#!/usr/bin/env python3
"""Production calibration arithmetic, bounded polling and port restoration."""
import pathlib,subprocess,tempfile
root=pathlib.Path(__file__).resolve().parents[2]
s=(root/'kernel/arch/x86_64/cpu/cpu.c').read_text()
start=s.index('static uint64_t cpu_tsc_pit_sample(')
end=s.index('\nvoid x86_cpu_detect_features',start)
mock=r'''
#include <stdint.h>
#include <assert.h>
static unsigned reads,writes,done_after;static uint8_t restored;
static uint64_t delta;static unsigned tsc_calls;
static uint8_t cpu_pit_read(uint16_t p){assert(p==0x61);
 return reads++==0?3:((reads>done_after)?0x20:0);}
static void cpu_pit_write(uint16_t p,uint8_t v){++writes;if(p==0x61)restored=v;}
static uint64_t x86_read_tsc(void){return tsc_calls++?100+delta:100;}
static uint64_t cpu_tsc_pit_sample(void);
static uint64_t run(unsigned done,uint64_t d){reads=writes=tsc_calls=0;
 done_after=done;delta=d;uint64_t hz=cpu_tsc_pit_sample();
 assert(restored==3&&writes==5);return hz;}
'''
cases=r'''
int main(void){
 uint64_t hz=run(100,100000000);
 assert(hz>3999900000ULL&&hz<4000100000ULL);
 assert(run(0,100000000)==0);
 assert(run(2000000,100000000)==0&&reads<=1000001);
 assert(run(100,0)==0);assert(run(100,UINT64_MAX)==0);
 assert(run(100,100)==0);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-tsc-pit-') as d:
 binary=str(pathlib.Path(d)/'test')
 subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-x','c','-','-o',binary],
                input=mock+s[start:end]+cases,text=True,check=True)
 subprocess.run([binary],check=True)
print('TSC PIT: 4 GHz calibration, missing/stuck timer, overflow and port restoration passed')
