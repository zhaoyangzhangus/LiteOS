"""Actual IOMMU map/unmap APIs with controlled domain/PTE/invalidation hooks."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/drivers/iommu/core.c').read_text()
wrappers = source[source.index('static void iommu_lock('):
                  source.index('static bool iommu_wait_register(')]
apis = source[source.index('kstatus_t iommu_map_pages('):
              source.index('bool iommu_self_test(')]
code = r'''
#include <assert.h>
#include <stdio.h>
#include <kernel/device.h>
#include <kernel/iommu.h>
#include <kernel/spinlock.h>
#include <kernel/sched.h>
#define VTD_MAX_IOVA (1ULL << 48)
static unsigned depth, caller_depth;
void sched_preempt_disable(void) {++depth;}
void sched_preempt_enable(void) {assert(depth);--depth;}
static spinlock_t g_iommu_lock;
static struct { bool enabled; } g_iommu;
typedef struct { unsigned unused; } iommu_domain_t;
static iommu_domain_t domain;
static device_t device;
static page_t page;
static bool domain_present, invalidate_ok;
static unsigned map_calls, rollback_calls, invalidations, fail_at;
static uint64_t ptes[4];
static void locked(void) {
    assert(depth == caller_depth+1 && atomic_load(&g_iommu_lock.state));
}
static iommu_domain_t *iommu_find_domain(device_t *d) {
    locked();assert(d==&device);return domain_present?&domain:0;
}
paddr_t page_to_phys(const page_t *p) {
    locked();assert(p==&page);return paddr_make(PAGE_SIZE);
}
static kstatus_t iommu_map_one(iommu_domain_t *d,uint64_t addr,paddr_t p,uint32_t access) {
    locked();assert(d==&domain && addr<4*PAGE_SIZE && p.value==PAGE_SIZE);
    assert(access==IOMMU_MAP_DEVICE_READ);
    ++map_calls;
    if(map_calls==fail_at)return K_EIO;
    ptes[addr/PAGE_SIZE]=p.value|access;return K_OK;
}
static uint64_t *iommu_pte_for(iommu_domain_t *d,uint64_t addr,bool create) {
    locked();assert(d==&domain && addr<4*PAGE_SIZE && !create);
    ++rollback_calls;return &ptes[addr/PAGE_SIZE];
}
static bool iommu_global_invalidate(void) {
    locked();++invalidations;return invalidate_ok;
}
''' + wrappers + apis + r'''
static void reset(void) {
    assert(depth==caller_depth && !atomic_load(&g_iommu_lock.state));
    g_iommu.enabled=true;domain_present=true;invalidate_ok=true;
    map_calls=rollback_calls=invalidations=fail_at=0;
    for(unsigned i=0;i<4;++i)ptes[i]=0;
}
static void balanced(kstatus_t actual,kstatus_t expected) {
    assert(actual==expected && depth==caller_depth && !atomic_load(&g_iommu_lock.state));
}
int main(void) {
    spinlock_init(&g_iommu_lock);
    page_t *pages[3]={&page,&page,&page};
    for(unsigned nesting=0;nesting<32;++nesting) {
        for(unsigned i=0;i<nesting;++i)sched_preempt_disable();
        caller_depth=nesting;reset();
        balanced(iommu_map_pages(0,iova_make(0),pages,3,IOMMU_MAP_DEVICE_READ),K_EINVAL);
        balanced(iommu_map_pages(&device,iova_make(1),pages,3,IOMMU_MAP_DEVICE_READ),K_EINVAL);
        balanced(iommu_map_pages(&device,iova_make(0),pages,0,IOMMU_MAP_DEVICE_READ),K_EINVAL);
        balanced(iommu_map_pages(&device,iova_make(VTD_MAX_IOVA-PAGE_SIZE),pages,3,IOMMU_MAP_DEVICE_READ),K_EINVAL);
        balanced(iommu_map_pages(&device,iova_make(0),pages,3,0),K_EINVAL);
        balanced(iommu_unmap_pages(&device,iova_make(0),0),K_EINVAL);
        balanced(iommu_unmap_pages(&device,iova_make(1),PAGE_SIZE),K_EINVAL);
        balanced(iommu_unmap_pages(&device,iova_make(VTD_MAX_IOVA-PAGE_SIZE),2*PAGE_SIZE),K_EINVAL);
        assert(!map_calls && !invalidations && !rollback_calls);
        g_iommu.enabled=false;
        balanced(iommu_map_pages(0,iova_make(1),0,0,0),K_OK);
        balanced(iommu_unmap_pages(0,iova_make(1),0),K_OK);
        reset();domain_present=false;
        balanced(iommu_map_pages(&device,iova_make(0),pages,3,IOMMU_MAP_DEVICE_READ),K_ENOENT);
        balanced(iommu_unmap_pages(&device,iova_make(0),PAGE_SIZE),K_ENOENT);
        assert(!map_calls && !invalidations && !rollback_calls);
        for(unsigned fail=1;fail<=3;++fail) {
            reset();fail_at=fail;
            balanced(iommu_map_pages(&device,iova_make(0),pages,3,IOMMU_MAP_DEVICE_READ),K_EIO);
            assert(map_calls==fail && rollback_calls==fail-1 && !invalidations);
            for(unsigned i=0;i<3;++i)assert(!ptes[i]);
        }
        for(unsigned null_at=0;null_at<3;++null_at) {
            reset();pages[null_at]=0;
            balanced(iommu_map_pages(&device,iova_make(0),pages,3,IOMMU_MAP_DEVICE_READ),K_EIO);
            assert(map_calls==null_at && rollback_calls==null_at && !invalidations);
            for(unsigned i=0;i<3;++i)assert(!ptes[i]);
            pages[null_at]=&page;
        }
        for(unsigned ok=0;ok<2;++ok) {
            reset();invalidate_ok=ok;
            balanced(iommu_map_pages(&device,iova_make(0),pages,3,IOMMU_MAP_DEVICE_READ),ok?K_OK:K_EIO);
            assert(map_calls==3 && invalidations==1 && !rollback_calls);
            for(unsigned i=0;i<3;++i)assert(ptes[i]);
            balanced(iommu_unmap_pages(&device,iova_make(0),3*PAGE_SIZE),ok?K_OK:K_EIO);
            assert(invalidations==2 && rollback_calls==3);
            for(unsigned i=0;i<3;++i)assert(!ptes[i]);
        }
        for(unsigned i=0;i<nesting;++i)sched_preempt_enable();
        assert(!depth);caller_depth=0;
    }
    puts("IOMMU map/unmap: 32 nested guard depths, disabled/invalid/missing-domain exits, each failed/null page rollback, success and invalidation-failure branches passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-iommu-mapping-') as directory:
    binary = str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-I'+str(root/'include'),'-fsanitize=address,undefined',
                    '-fno-pie','-no-pie','-x','c','-','-o',binary],
                   input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=20)
