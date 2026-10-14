#!/usr/bin/env python3
"""Compile real libc wrappers and ensure Linux descriptors never seek."""
import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[2]
source = (root / 'user/libc/syscall.c').read_text()
functions = []
for name, following in [('pread', 'pread64'), ('pwrite', 'pwrite64')]:
    begin = source.index('ssize_t ' + name + '(')
    functions.append(source[begin:source.index('ssize_t ' + following + '(', begin)])
harness = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <assert.h>
typedef long ssize_t;
typedef long off_t;
#define SEEK_CUR 1
#define SEEK_SET 0
#define EIO 5
#define LINUX_SYS_PREAD64 17
#define LINUX_SYS_PWRITE64 18
static int errno, calls, seeks;
static int64_t syscall_result=16;
static uint64_t expected_number;
static bool libc_linux_personality(void){return true;}
static bool __libc_descriptor_is_linux_fd(int fd){return fd==42;}
static int __libc_linux_fd_for_descriptor(int fd){assert(fd==42);return 7;}
static int libc_error(int64_t value){errno=(int)-value;return -1;}
static long __libc_linux_syscall6(uint64_t n,uint64_t fd,uint64_t b,
 uint64_t len,uint64_t offset,uint64_t a4,uint64_t a5) {
 assert(n==expected_number && fd==7 && b==123 && len==16 && offset==99);
 assert(a4==0 && a5==0);++calls;return syscall_result;
}
static off_t lseek(int fd,off_t off,int whence){(void)fd;(void)off;(void)whence;++seeks;return 0;}
static ssize_t read(int fd,void*b,size_t n){(void)fd;(void)b;return n;}
static ssize_t write(int fd,const void*b,size_t n){(void)fd;(void)b;return n;}
'''
main = r'''
int main(void){
 expected_number=17;assert(pread(42,(void*)123,16,99)==16);
 expected_number=18;assert(pwrite(42,(void*)123,16,99)==16);
 assert(calls==2 && seeks==0);
 syscall_result=0;assert(pwrite(42,(void*)123,16,99)==0);
 expected_number=17;syscall_result=-22;
 assert(pread(42,(void*)123,16,99)==-1 && errno==22);
 assert(seeks==0);return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='system-libc-pio-') as directory:
    path = pathlib.Path(directory) / 'test.c'
    binary = pathlib.Path(directory) / 'test'
    path.write_text(harness + '\n'.join(functions) + main)
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', str(path), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('libc positioned I/O: translated Linux fd, explicit offset, zero seek calls')
