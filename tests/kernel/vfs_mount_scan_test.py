"""Actual mount routing versus repeated-prefix-scan oracle."""
from pathlib import Path
import subprocess
import tempfile
import re
root=Path(__file__).resolve().parents[2]
source=(root/'kernel/fs/vfs/core.c').read_text()
mount_limit=int(re.search(r'#define VFS_FAT32_MOUNT_LIMIT (\d+)U',
                         (root/'kernel/fs/vfs/internal.h').read_text()).group(1))
helpers=source[source.index('static bool vfs_volatile_path('):source.index('vfs_fat32_mount_t *vfs_fat32_mount_for_path(')]
helpers=helpers.replace('size_t prefix_length) {',
                        'size_t prefix_length) {\n    ++current_matches;')
route=source[source.index('vfs_fat32_mount_t *vfs_fat32_mount_for_path('):source.index('bool vfs_fat32_relative_path(')]
shared_copy = '''    } else {
        size_t offset = selected_length == 1U ? 1U : selected_length + 1U;
        size_t length = path_length - offset;
        if (length >= VFS_PATH_LIMIT) return 0;
        for (size_t index = 0U; index < length; ++index) {
            relative[index] = path[offset + index];
        }
        relative[length] = 0;
'''
previous_copy = '''    } else if (selected_length == 1U) {
        size_t length = path_length - 1U;
        if (length >= VFS_PATH_LIMIT) return 0;
        for (size_t index = 0U; index < length; ++index) {
            relative[index] = path[index + 1U];
        }
        relative[length] = 0;
    } else {
        size_t length = path_length - selected_length - 1U;
        if (length >= VFS_PATH_LIMIT) return 0;
        for (size_t index = 0U; index < length; ++index) {
            relative[index] = path[selected_length + 1U + index];
        }
        relative[length] = 0;
'''
assert shared_copy in route
old=route.replace(shared_copy, previous_copy).replace('vfs_fat32_mount_for_path(', 'previous_route(').replace(
 '        if (!candidate->mounted || candidate->filesystem == 0) continue;\n','').replace(
 'prefix_length <= selected_length ||\n'
 '            !vfs_mount_matches(candidate, path, path_length, prefix_length)',
 '!previous_matches(candidate, path, path_length) ||\n'
 '            prefix_length <= selected_length')
assert 'vfs_mount_matches(' not in old
program=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define VFS_PATH_LIMIT 256U
#define VFS_FAT32_MOUNT_LIMIT MOUNT_LIMIT_VALUE
typedef struct {bool mounted;void *filesystem;char prefix[256];} vfs_fat32_mount_t;
static vfs_fat32_mount_t g_fat32_mounts[VFS_FAT32_MOUNT_LIMIT];
static unsigned prefix_scans,current_matches,old_matches;
static size_t vfs_string_length(const char *p){
 for(unsigned i=0;i<VFS_FAT32_MOUNT_LIMIT;++i)if(p==g_fat32_mounts[i].prefix)++prefix_scans;
 size_t n=0;while(n<256&&p[n])++n;return n;
}
static bool vfs_path_range_equals(const char *a,size_t n,const char *b,size_t m){return n==m&&!memcmp(a,b,n);}
static bool previous_matches(const vfs_fat32_mount_t *mount,const char *path,size_t length){
 if(!mount||!mount->mounted||!mount->filesystem||!path||!length||path[0]!='/')return false;
 ++old_matches;
 size_t n=vfs_string_length(mount->prefix);
 if(!n||length<n||!vfs_path_range_equals(path,n,mount->prefix,n))return false;
 return n==1||length==n||path[n]=='/';
}
''' + helpers + old + route + r'''
int main(void){
 const char *prefixes[]={"","/","/vol","/vol/nested","/tmp","/v","/vol/","x"};
 const char *paths[]={0,"","x","/","/vol","/vol/","/vol/x","/volx","/vol/nested/x","/tmp","/tmp/x","/tmpx","/v/x"};
 unsigned cases=0,reduced=0;
 for(unsigned config=0;config<512;++config)
 for(unsigned flags=0;flags<64;++flags){
  unsigned value=config,active=0;
  for(unsigned i=0;i<3;++i){
   strcpy(g_fat32_mounts[i].prefix,prefixes[value%8]);value/=8;
   g_fat32_mounts[i].mounted=(flags&(1U<<i))!=0;
   g_fat32_mounts[i].filesystem=(flags&(8U<<i))?(void*)(uintptr_t)(i+1):0;
   active+=g_fat32_mounts[i].mounted&&g_fat32_mounts[i].filesystem;
  }
  for(unsigned p=0;p<sizeof(paths)/sizeof(paths[0]);++p){
   char before[256],after[256];memset(before,0x75,256);memset(after,0x75,256);
   prefix_scans=0;vfs_fat32_mount_t *expected=previous_route(paths[p],before);unsigned old_reads=prefix_scans;
   prefix_scans=0;vfs_fat32_mount_t *actual=vfs_fat32_mount_for_path(paths[p],after);
   assert(actual==expected&&!memcmp(before,after,256));
   assert(prefix_scans<=active&&old_reads>=prefix_scans);
   if(old_reads){assert(old_reads==VFS_FAT32_MOUNT_LIMIT+active&&prefix_scans==active);++reduced;}
   ++cases;
  }
 }
 char path[256],out[256];memset(path,'x',256);path[0]='/';
 prefix_scans=0;assert(!vfs_fat32_mount_for_path(path,out)&&!prefix_scans);
 assert(!vfs_fat32_mount_for_path("/vol",0));
 unsigned overlaps=0;
 memset(g_fat32_mounts,0,sizeof(g_fat32_mounts));
 for(unsigned mount=0;mount<3;++mount){
  strcpy(g_fat32_mounts[0].prefix,mount==0?"/":mount==1?"/vol":"/vol/nested");
  g_fat32_mounts[0].mounted=true;g_fat32_mounts[0].filesystem=(void*)(uintptr_t)1;
  for(unsigned length=1;length<256;++length)
  for(int delta=-32;delta<=32;++delta){
   char before[768],after[768];memset(before,0x71,sizeof(before));
   const char *stem="/vol/nested/";size_t n=strlen(stem);
   for(unsigned i=0;i<length;++i)before[64+i]=i<n?stem[i]:(char)('a'+i%26);
   before[64+length]=0;memcpy(after,before,sizeof(after));
   vfs_fat32_mount_t *expected=previous_route(before+64,before+64+delta);
   vfs_fat32_mount_t *actual=vfs_fat32_mount_for_path(after+64,after+64+delta);
   assert(actual==expected&&!memcmp(before,after,sizeof(before)));++overlaps;
  }
 }
 printf("Shared relative copy: %u exact whole-buffer alias/length/root/nested cases preserve original forward-copy behavior\n",overlaps);
 unsigned full_cases=0,avoided=0;uint32_t random=0x51a3c0de;
 for(unsigned iteration=0;iteration<10000;++iteration){
  for(unsigned i=0;i<VFS_FAT32_MOUNT_LIMIT;++i){
   random=random*1664525U+1013904223U;
   unsigned bits=random>>16;
   strcpy(g_fat32_mounts[i].prefix,prefixes[bits%8]);
   g_fat32_mounts[i].mounted=(bits&16U)!=0;
   g_fat32_mounts[i].filesystem=(bits&32U)?(void*)(uintptr_t)(i+1):0;
  }
  for(unsigned p=0;p<sizeof(paths)/sizeof(paths[0]);++p){
   char before[256],after[256];memset(before,0x75,256);memset(after,0x75,256);
   old_matches=current_matches=0;
   vfs_fat32_mount_t *expected=previous_route(paths[p],before);
   vfs_fat32_mount_t *actual=vfs_fat32_mount_for_path(paths[p],after);
   assert(actual==expected&&!memcmp(before,after,256));
   assert(current_matches<=old_matches);avoided+=old_matches-current_matches;++full_cases;
  }
 }
 for(unsigned mode=0;mode<3;++mode){
  for(unsigned i=0;i<VFS_FAT32_MOUNT_LIMIT;++i){
   unsigned depth=mode==0?1:mode==1?i:VFS_FAT32_MOUNT_LIMIT-1-i;
   char *p=g_fat32_mounts[i].prefix;
   if(!depth){p[0]='/';p[1]=0;}
   else{for(unsigned j=0;j<depth;++j){p[2*j]='/';p[2*j+1]='a';}p[2*depth]=0;}
   g_fat32_mounts[i].mounted=true;g_fat32_mounts[i].filesystem=(void*)(uintptr_t)(i+1);
  }
  unsigned depth=VFS_FAT32_MOUNT_LIMIT-1;
  for(unsigned j=0;j<depth;++j){path[2*j]='/';path[2*j+1]='a';}
  path[2*depth]='/';path[2*depth+1]='x';path[2*depth+2]=0;
  char before[256],after[256];memset(before,0x75,256);memset(after,0x75,256);
  old_matches=current_matches=0;
  vfs_fat32_mount_t *expected=previous_route(path,before);
  assert(vfs_fat32_mount_for_path(path,after)==expected&&!memcmp(before,after,256));
  assert(old_matches==VFS_FAT32_MOUNT_LIMIT);
  assert(current_matches==(mode==1?VFS_FAT32_MOUNT_LIMIT:1));
 }
 printf("Candidate pruning: %u full-%u-slot route/output cases, %u avoided match calls; ties and descending nested mounts compare once instead of %u times\n",full_cases,VFS_FAT32_MOUNT_LIMIT,avoided,VFS_FAT32_MOUNT_LIMIT);
 printf("Mount scans: %u exact route/output cases across longest prefixes, ties, inactive/missing filesystems, boundary/volatile paths; %u routes skip duplicate and inactive scans\n",cases,reduced);
}
'''
program=program.replace('MOUNT_LIMIT_VALUE',str(mount_limit)+'U')
with tempfile.TemporaryDirectory(prefix='liteos-mount-scan-') as directory:
    binary=str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie','-x','c','-','-o',binary],
                   input=program,text=True,check=True)
    subprocess.run([binary],check=True)
