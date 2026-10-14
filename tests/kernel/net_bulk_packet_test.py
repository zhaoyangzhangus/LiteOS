"""Production packet builders/parsers with actual kernel bulk string routines."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
code = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <kernel/net_core.h>
#include <uapi/socket.h>
static _Thread_local unsigned preempt_depth;
void sched_preempt_disable(void) {++preempt_depth;}
void sched_preempt_enable(void) {assert(preempt_depth);--preempt_depth;}
static unsigned char payload[66000],arena[66064];
static net_arp_cache_t arp;
static net_ipv6_neighbor_cache_t ndp;
static const uint8_t neighbor[16]={0x20,1,0,0,0,0,0,0,0,0,0,0,0,0,0,1};
static void *cache_worker(void *unused) {
 (void)unused;uint8_t mac[6],out[6];
 for(unsigned i=0;i<5000;++i) {
  memset(mac,(int)i,sizeof(mac));
  assert(net_arp_cache_update(&arp,1,mac,i)==K_OK && net_arp_cache_lookup(&arp,1,out));
  for(unsigned j=1;j<6;++j)assert(out[j]==out[0]);
  assert(net_ipv6_neighbor_cache_update(&ndp,neighbor,mac,i)==K_OK && net_ipv6_neighbor_cache_lookup(&ndp,neighbor,out));
  for(unsigned j=1;j<6;++j)assert(out[j]==out[0]);
 }
 return 0;
}
int main(void) {
 assert(net_core_self_test() && net_arp_self_test() && net_ipv6_self_test());
 assert(net_tcp_self_test() && net_firewall_self_test() && net_ndp_self_test());
 for(unsigned nesting=0;nesting<32;++nesting) {
  for(unsigned i=0;i<nesting;++i)sched_preempt_disable();
  assert(net_firewall_self_test() && preempt_depth==nesting);
  net_firewall_t firewall;net_firewall_init(&firewall,NET_FIREWALL_DENY);
  net_firewall_rule_t rule={.enabled=true,.family=OS_AF_INET4,.action=NET_FIREWALL_ALLOW};
  uint32_t id=UINT32_MAX;
  for(unsigned i=0;i<NET_FIREWALL_MAX_RULES;++i) {
   assert(net_firewall_add(&firewall,&rule,&id)==K_OK && id==i && preempt_depth==nesting);
  }
  assert(net_firewall_add(&firewall,&rule,&id)==K_ENOMEM && preempt_depth==nesting);
  assert(net_firewall_add(0,&rule,&id)==K_EINVAL && preempt_depth==nesting);
  assert(net_firewall_remove(&firewall,NET_FIREWALL_MAX_RULES)==K_EINVAL && preempt_depth==nesting);
  for(unsigned i=0;i<NET_FIREWALL_MAX_RULES;++i) {
   assert(net_firewall_remove(&firewall,i)==K_OK && preempt_depth==nesting);
   assert(net_firewall_remove(&firewall,i)==K_ENOENT && preempt_depth==nesting);
  }
  assert(!net_firewall_check(&firewall,0) && preempt_depth==nesting);
  for(unsigned i=0;i<nesting;++i)sched_preempt_enable();
 }
 net_arp_cache_init(&arp);net_ipv6_neighbor_cache_init(&ndp);pthread_t threads[4];
 for(unsigned i=0;i<4;++i)assert(!pthread_create(&threads[i],0,cache_worker,0));
 for(unsigned i=0;i<4;++i)assert(!pthread_join(threads[i],0));
 assert(!atomic_load(&arp.lock.state) && !atomic_load(&ndp.lock.state));
 net_arp_cache_init(&arp);net_ipv6_neighbor_cache_init(&ndp);
 for(unsigned i=0;i<NET_ARP_CACHE_SIZE*4;++i) {
  uint8_t mac[6];memset(mac,(int)i,sizeof(mac));
  assert(net_arp_cache_update(&arp,i+1,mac,i)==K_OK);
  assert(arp.entries[i%NET_ARP_CACHE_SIZE].address==i+1);
  assert(arp.entries[i%NET_ARP_CACHE_SIZE].updated==i);
 }
 for(unsigned i=0;i<NET_NDP_CACHE_SIZE*4;++i) {
  uint8_t address[16]={0x20,1},mac[6];address[14]=(i+1)>>8;address[15]=i+1;memset(mac,(int)i,sizeof(mac));
  assert(net_ipv6_neighbor_cache_update(&ndp,address,mac,i)==K_OK);
  assert(!memcmp(ndp.entries[i%NET_NDP_CACHE_SIZE].address,address,16));
  assert(ndp.entries[i%NET_NDP_CACHE_SIZE].updated==i);
 }
 const uint8_t source_mac[6]={2,1,2,3,4,5},destination_mac[6]={2,6,7,8,9,10};
 const uint8_t source6[16]={0x20,1,0xd,0xb8,0,0,0,0,0,0,0,0,0,0,0,1};
 const uint8_t destination6[16]={0x20,1,0xd,0xb8,0,0,0,0,0,0,0,0,0,0,0,2};
 const unsigned lengths[]={0,1,17,63,64,65,511,512,513,1400,1472,4096,32768,65000};
 for(unsigned i=0;i<sizeof(payload);++i)payload[i]=(unsigned char)(i*17+3);
 unsigned cases=0;
 for(unsigned offset=0;offset<16;++offset)for(unsigned l=0;l<sizeof(lengths)/sizeof(lengths[0]);++l)for(unsigned variant=0;variant<4;++variant) {
  unsigned n=lengths[l];uint8_t *frame=arena+offset;size_t length=0;
  memset(arena,0x75,sizeof(arena));kstatus_t status;
  if(variant==0)status=net_udp_build_ipv4(frame,66000,source_mac,destination_mac,0xc0a80002,0xc0a80001,1234,4321,payload,n,&length);
  else if(variant==1)status=net_udp_build_ipv6(frame,66000,source_mac,destination_mac,source6,destination6,1234,4321,payload,n,&length);
  else if(variant==2)status=net_tcp_build_ipv4(frame,66000,source_mac,destination_mac,0xc0a80002,0xc0a80001,1234,4321,0x12345678,0xabcdef01,0x10,4096,payload,n,&length);
  else status=net_tcp_build_ipv6(frame,66000,source_mac,destination_mac,source6,destination6,1234,4321,0x12345678,0xabcdef01,0x10,4096,payload,n,&length);
  assert(status==K_OK);
  unsigned header=NET_ETHERNET_HEADER_SIZE+((variant&1)?NET_IPV6_HEADER_SIZE:NET_IPV4_HEADER_SIZE)+(variant<2?NET_UDP_HEADER_SIZE:NET_TCP_HEADER_SIZE);
  assert(length==header+n && !memcmp(frame+header,payload,n));
  assert(!memcmp(frame,destination_mac,6) && !memcmp(frame+6,source_mac,6));
  for(unsigned i=0;i<offset;++i)assert(arena[i]==0x75);
  for(unsigned i=0;i<16;++i)assert(frame[length+i]==0x75);
  if(variant==0) {
   net_udp_view_t view;assert(net_udp_parse_ipv4(frame,length,&view)==K_OK);
   assert(view.source_port==1234 && view.destination_port==4321 && view.payload_length==n);
   assert(!memcmp(view.payload,payload,n));
  } else if(variant==1) {
   net_ipv6_udp_view_t view;assert(net_udp_parse_ipv6(frame,length,&view)==K_OK);
   assert(view.source_port==1234 && view.destination_port==4321 && view.payload_length==n);
   assert(!memcmp(view.payload,payload,n));
  } else {
   net_tcp_view_t view;assert((variant==2?net_tcp_parse_ipv4(frame,length,&view):net_tcp_parse_ipv6(frame,length,&view))==K_OK);
   assert(view.source_port==1234 && view.destination_port==4321 && view.payload_length==n);
   assert(view.sequence==0x12345678 && view.acknowledgement==0xabcdef01 && !memcmp(view.payload,payload,n));
  }
  ++cases;
 }
 printf("Network: six production protocol/cache/firewall self-tests, 20000 concurrent updates/lookups per ARP/NDP cache, four complete replacement cycles, %u IPv4/IPv6 UDP/TCP bulk packets and canaries passed\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-net-bulk-') as directory:
    path = Path(directory)
    objects = []
    rename = ['-Dmemcpy=liteos_memcpy','-Dmemmove=liteos_memmove',
              '-Dmemset=liteos_memset','-Dmemcmp=liteos_memcmp']
    for name, source in [('core','kernel/net/core/core.c'),
                         ('self_test','kernel/net/core/self_test.c'),
                         ('firewall','kernel/net/core/firewall.c'),
                         ('string','kernel/string.c')]:
        obj = str(path/(name+'.o'));objects.append(obj)
        subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                        '-fno-builtin','-mno-sse','-mno-mmx','-fsanitize=address,undefined',
                        '-fno-pie','-I'+str(root/'include'),*rename,
                        '-c',str(root/source),'-o',obj],check=True)
    binary = str(path/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-pthread',
                    '-I'+str(root/'include'),'-fsanitize=address,undefined',
                    '-fno-pie','-no-pie','-x','c','-','-x','none',*objects,'-o',binary],
                   input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
