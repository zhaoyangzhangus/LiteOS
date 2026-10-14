"""Actual firmware path validation against an independent split-component oracle."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'kernel/drivers/core/firmware.c').read_text()
code=r'''
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
''' + source + r'''
static bool oracle(const char *name) {
    if(!name)return false;
    size_t n=strnlen(name,FIRMWARE_NAME_CAP);
    if(!n || n==FIRMWARE_NAME_CAP)return false;
    const char *allowed="abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-./";
    for(size_t i=0;i<n;++i)if(!strchr(allowed,(unsigned char)name[i]))return false;
    size_t start=0;
    for(size_t end=0;end<=n;++end)if(end==n || name[end]=='/') {
        size_t length=end-start;
        if(!length || (length==2 && !memcmp(name+start,"..",2)))return false;
        start=end+1;
    }
    return true;
}
static unsigned cases;
static void check(const char *name) { assert(firmware_logical_name_valid(name)==oracle(name)); ++cases; }
int main(void) {
    assert(firmware_core_self_test()); check(0);
    char name[FIRMWARE_NAME_CAP+1];
    const char alphabet[]="a./@";
    for(unsigned length=0;length<=8;++length) {
        unsigned combinations=1U<<(2*length);
        for(unsigned value=0;value<combinations;++value) {
            unsigned remaining=value;
            for(unsigned i=0;i<length;++i) { name[i]=alphabet[remaining&3]; remaining>>=2; }
            name[length]=0; check(name);
        }
    }
    for(unsigned length=1;length<FIRMWARE_NAME_CAP;++length) {
        memset(name,'a',length); name[length]=0;
        for(unsigned position=0;position<length;++position)for(unsigned byte=1;byte<=255;++byte) {
            name[position]=(char)byte; check(name); name[position]='a';
        }
    }
    const char *special[]={".","...","a/./b","a/.../b","a..b","..a","a..","a/../b","../a","a/..","..","/a","a/","a//b"};
    for(unsigned i=0;i<sizeof(special)/sizeof(special[0]);++i)check(special[i]);
    size_t page=(size_t)sysconf(_SC_PAGESIZE);
    char *guard=mmap(0,page*3,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(guard!=MAP_FAILED && !mprotect(guard+page,page,PROT_READ|PROT_WRITE));
    for(unsigned n=0;n<FIRMWARE_NAME_CAP;++n) {
        char *tail=guard+page*2-n-1; memset(tail,'a',n); tail[n]=0; check(tail);
        if(n>=2) { tail[n-2]='.'; tail[n-1]='.'; check(tail); }
    }
    char *tail=guard+page*2-FIRMWARE_NAME_CAP;
    memset(tail,'a',FIRMWARE_NAME_CAP); check(tail); /* Unterminated at the exact cap. */
    assert(!munmap(guard,page*3));
    printf("Firmware names: %u oracle comparisons, all byte values/positions through the name cap, parent-component rules and exact-tail guard pages passed\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-firmware-name-') as directory:
    binary=str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-I'+str(root/'include'),'-fsanitize=address,undefined',
                    '-fno-pie','-no-pie','-x','c','-','-o',binary],input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
