"""Production registry publication, single-scan insertion and upgrade invariants."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'kernel/package.c').read_text()
source=source.replace('atomic_compare_exchange_strong_explicit(&registry->init_state',
                      'counted_cas(&registry->init_state')
source=source.replace('for (uint32_t i = 0; i < PACKAGE_REGISTRY_CAPACITY; ++i) {',
                     'for (uint32_t i = 0; i < PACKAGE_REGISTRY_CAPACITY; ++i) { atomic_fetch_add(&visits,1);')
code=r'''
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
static atomic_uint attempts,visits;
static _Bool parallel;
static _Bool counted_cas(atomic_uint *p,unsigned *expected,unsigned value,
                        memory_order success,memory_order failure) {
    atomic_fetch_add(&attempts,1);
    if(parallel)while(atomic_load(&attempts)<8)sched_yield();
    return atomic_compare_exchange_strong_explicit(p,expected,value,success,failure);
}
''' + source + r'''
static package_registry_t registry;
static package_identity_t identity(unsigned id,unsigned version,unsigned generation) {
    package_identity_t p={0}; p.package_id[0]=(uint8_t)(id+1);
    p.version=version; p.generation=generation; p.payload_digest[0]=0xAA;
    p.name[0]='p'; p.owner_uid=id; return p;
}
static void *initializer(void *unused) {
    (void)unused; assert(package_registry_init(&registry));
    assert(!atomic_load(&registry.lock.state));
    package_identity_t zero={0};
    for(unsigned i=0;i<PACKAGE_REGISTRY_CAPACITY;++i) {
        assert(!registry.entries[i].registered);
        assert(!memcmp(&registry.entries[i].identity,&zero,sizeof(zero)));
    }
    return 0;
}
static void *updater(void *arg) {
    unsigned id=(unsigned)(uintptr_t)arg;
    for(unsigned v=1;v<=1000;++v) {
        package_identity_t p=identity(id,v,v),found;
        assert(package_register(&registry,&p)==K_OK);
        assert(package_lookup(&registry,p.package_id,&found)==K_OK && !memcmp(&p,&found,sizeof(p)));
        assert(package_register(&registry,&p)==K_EINVAL);
    }
    package_identity_t p=identity(id,1,1);
    assert(package_unregister(&registry,p.package_id)==K_OK); return 0;
}
int main(void) {
    pthread_t threads[8]; parallel=true;
    for(unsigned i=0;i<PACKAGE_REGISTRY_CAPACITY;++i) {
        registry.entries[i].registered=true;
        memset(&registry.entries[i].identity,0x75,sizeof(package_identity_t));
    }
    for(unsigned i=0;i<8;++i)assert(!pthread_create(&threads[i],0,initializer,0));
    for(unsigned i=0;i<8;++i)assert(!pthread_join(threads[i],0));
    parallel=false; assert(atomic_load(&attempts)==8 && package_core_self_test());
    for(unsigned i=0;i<PACKAGE_REGISTRY_CAPACITY;++i) {
        package_identity_t p=identity(i,1,1);
        atomic_store(&visits,0); assert(package_register(&registry,&p)==K_OK);
        assert(atomic_load(&visits)==PACKAGE_REGISTRY_CAPACITY);
        assert(registry.entries[i].registered && !memcmp(&registry.entries[i].identity,&p,sizeof(p)));
    }
    package_identity_t p=identity(64,1,1);
    assert(package_register(&registry,&p)==K_ENOMEM);
    p=identity(63,2,2); assert(package_register(&registry,&p)==K_OK); /* Upgrade when full. */
    package_identity_t first=identity(0,1,1),found;
    assert(package_unregister(&registry,first.package_id)==K_OK);
    p=identity(63,3,3); assert(package_register(&registry,&p)==K_OK); /* Hole before existing identity. */
    assert(!registry.entries[0].registered && registry.entries[63].identity.version==3);
    p=identity(64,1,1); atomic_store(&visits,0);
    assert(package_register(&registry,&p)==K_OK && atomic_load(&visits)==PACKAGE_REGISTRY_CAPACITY);
    assert(registry.entries[0].identity.package_id[0]==65);
    for(unsigned i=0;i<PACKAGE_REGISTRY_CAPACITY;++i) {
        uint8_t id[PACKAGE_ID_SIZE]; memcpy(id,registry.entries[i].identity.package_id,sizeof(id));
        assert(package_unregister(&registry,id)==K_OK);
    }
    package_identity_t model[PACKAGE_REGISTRY_CAPACITY]={0}; bool occupied[PACKAGE_REGISTRY_CAPACITY]={0};
    unsigned seed=17,operations=20000;
    for(unsigned step=0;step<operations;++step) {
        seed=seed*1664525U+1013904223U;
        unsigned id=(seed>>8)%96,op=(seed>>24)%3;
        p=identity(id,1+(seed%17),1+((seed>>16)%19));
        int slot=-1,hole=-1;
        for(unsigned i=0;i<PACKAGE_REGISTRY_CAPACITY;++i) {
            if(!occupied[i]) { if(hole<0)hole=(int)i; }
            else if(model[i].package_id[0]==id+1)slot=(int)i;
        }
        if(op==0) {
            kstatus_t expected;
            if(slot>=0) {
                expected=p.version>model[slot].version && p.generation>model[slot].generation?K_OK:K_EINVAL;
                if(expected==K_OK)model[slot]=p;
            } else if(hole<0)expected=K_ENOMEM;
            else { expected=K_OK; occupied[hole]=true; model[hole]=p; }
            assert(package_register(&registry,&p)==expected);
        } else if(op==1) {
            memset(&found,0x39,sizeof(found)); package_identity_t before=found;
            assert(package_lookup(&registry,p.package_id,&found)==(slot<0?K_ENOENT:K_OK));
            assert(!memcmp(&found,slot<0?&before:&model[slot],sizeof(found)));
        } else {
            assert(package_unregister(&registry,p.package_id)==(slot<0?K_ENOENT:K_OK));
            if(slot>=0) { occupied[slot]=false; model[slot]=(package_identity_t){0}; }
        }
        for(unsigned i=0;i<PACKAGE_REGISTRY_CAPACITY;++i) {
            assert(registry.entries[i].registered==occupied[i]);
            assert(!memcmp(&registry.entries[i].identity,&model[i],sizeof(model[i])));
        }
    }
    for(unsigned i=0;i<PACKAGE_REGISTRY_CAPACITY;++i)
        if(occupied[i])assert(package_unregister(&registry,model[i].package_id)==K_OK);
    p=identity(0,1,1); package_identity_t invalid=p;
    invalid.version=0; assert(!package_identity_valid(&invalid));
    invalid=p; invalid.generation=0; assert(!package_identity_valid(&invalid));
    invalid=p; invalid.reserved=1; assert(!package_identity_valid(&invalid));
    invalid=p; memset(invalid.name,'x',sizeof(invalid.name)); assert(!package_identity_valid(&invalid));
    invalid=p; memset(invalid.package_id,0,sizeof(invalid.package_id)); assert(!package_identity_valid(&invalid));
    invalid=p; memset(invalid.payload_digest,0,sizeof(invalid.payload_digest)); assert(!package_identity_valid(&invalid));
    assert(!package_registry_init(0) && package_register(0,&p)==K_EINVAL);
    assert(package_register(&registry,0)==K_EINVAL && package_lookup(&registry,0,&found)==K_EINVAL);
    unsigned before=atomic_load(&attempts);
    for(unsigned i=0;i<10000;++i)assert(package_registry_init(&registry));
    assert(atomic_load(&attempts)==before);
    for(unsigned i=0;i<4;++i)assert(!pthread_create(&threads[i],0,updater,(void *)(uintptr_t)i));
    for(unsigned i=0;i<4;++i)assert(!pthread_join(threads[i],0));
    for(unsigned i=0;i<PACKAGE_REGISTRY_CAPACITY;++i)assert(!registry.entries[i].registered);
    assert(atomic_load(&attempts)==before && !atomic_load(&registry.lock.state));
    puts("Package registry: eight initializers, exact 64-slot insertion scan, full upgrades/early holes, 20000 model operations and 4000 concurrent upgrades with no repeat init CAS passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-package-') as directory:
    binary=str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-pthread',
                    '-I'+str(root/'include'),'-fsanitize=address,undefined',
                    '-fno-pie','-no-pie','-x','c','-','-o',binary],input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
