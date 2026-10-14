"""Production device/driver initializers linked to the actual kernel memset."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'kernel/drivers/core/device.c').read_text()
code=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
void *liteos_memset(void *,int,size_t);
''' + source.replace('memset(', 'liteos_memset(') + r'''
static kstatus_t probe(device_t *d) { (void)d; return K_OK; }
static void remove_device(device_t *d) { (void)d; }
static device_ops_t ops;
int main(void) {
    _Alignas(16) unsigned char storage[sizeof(device_t)+128];
    _Alignas(16) unsigned char driver_storage[sizeof(driver_t)+128];
    unsigned cases=0;
    for(unsigned offset=16;offset<=64;offset+=_Alignof(device_t))for(unsigned mode=0;mode<4;++mode) {
        memset(storage,0x75,sizeof(storage));
        device_t *d=(void *)(storage+offset); device_t expected;
        memset(&expected,0,sizeof(expected));
        uint64_t id=mode?UINT64_MAX-mode:0; uint32_t cls=mode?UINT32_MAX-mode:0;
        const device_ops_t *selected=mode&1?&ops:0;
        void *bus=mode&2?(void *)(uintptr_t)0x12345678:0;
        device_object_init(d,id,cls,selected,bus);
        refcount_init(&expected.object.refs,1); expected.object.type=KOBJECT_TYPE_DEVICE;
        expected.object.ops=&g_device_object_ops;
        expected.device_id=id; expected.class_id=cls; expected.ops=selected; expected.bus_data=bus;
        atomic_init(&expected.state,DEVICE_DISCOVERED);
        expected.sibling_node.next=expected.sibling_node.prev=&d->sibling_node;
        expected.children.next=expected.children.prev=&d->children;
        expected.io_requests.next=expected.io_requests.prev=&d->io_requests;
        spinlock_init(&expected.state_lock); atomic_init(&expected.io_inflight,0);
        expected.power_timeout_ns=DEVICE_DEFAULT_POWER_TIMEOUT_NS;
        assert(!memcmp(d,&expected,sizeof(expected)));
        for(unsigned i=0;i<offset;++i)assert(storage[i]==0x75);
        for(unsigned i=offset+sizeof(*d);i<sizeof(storage);++i)assert(storage[i]==0x75);
        assert(!strcmp(d->object.ops->type_name,"Device")); d->object.ops->destroy(d);
        assert(!memcmp(d,&expected,sizeof(expected))); ++cases;
    }
    for(unsigned offset=16;offset<=64;offset+=_Alignof(driver_t))for(unsigned mode=0;mode<8;++mode) {
        memset(driver_storage,0x39,sizeof(driver_storage));
        driver_t *d=(void *)(driver_storage+offset); driver_t expected;
        memset(&expected,0,sizeof(expected));
        const char *name=mode&1?"driver":0;
        kstatus_t (*p)(device_t *)=mode&2?probe:0;
        void (*r)(device_t *)=mode&4?remove_device:0;
        driver_object_init(d,name,p,r);
        refcount_init(&expected.object.refs,1); expected.object.type=DRIVER_OBJECT_TYPE;
        expected.object.ops=&g_driver_object_ops; expected.name=name; expected.probe=p; expected.remove=r;
        expected.api_version=OS_DRIVER_API_VERSION; expected.struct_size=sizeof(*d);
        assert(!memcmp(d,&expected,sizeof(expected)));
        for(unsigned i=0;i<offset;++i)assert(driver_storage[i]==0x39);
        for(unsigned i=offset+sizeof(*d);i<sizeof(driver_storage);++i)assert(driver_storage[i]==0x39);
        assert(!strcmp(d->object.ops->type_name,"Driver")); d->object.ops->destroy(d);
        assert(!memcmp(d,&expected,sizeof(expected))); ++cases;
    }
    device_object_init(0,0,0,0,0); driver_object_init(0,0,0,0);
    printf("Device/driver init: %u aligned poison/canary cases, exact object fields/padding, list self-links, optional callbacks and null inputs with kernel memset passed\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-device-init-') as directory:
    path=Path(directory); obj=str(path/'string.o'); binary=str(path/'test')
    subprocess.run(['cc','-std=c11','-O2','-fno-builtin','-mno-sse','-mno-mmx',
                    '-I'+str(root/'include'),'-fsanitize=address,undefined','-fno-pie',
                    '-Dmemcpy=liteos_memcpy','-Dmemmove=liteos_memmove',
                    '-Dmemset=liteos_memset','-Dmemcmp=liteos_memcmp',
                    '-c',str(root/'kernel/string.c'),'-o',obj],check=True)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-I'+str(root/'include'),'-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
                    '-x','c','-','-x','none',obj,'-o',binary],input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
