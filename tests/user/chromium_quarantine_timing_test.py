"""Allocator diagnostics: no allocation/logging; phase, threshold and busy guard."""
from pathlib import Path as _TestPath
import sys as _test_sys
_test_sys.path.insert(0, str(_TestPath(__file__).resolve().parents[1]))
from chromium_source import chromium_source
from pathlib import Path
import subprocess
import tempfile

source = (chromium_source() / 'components/gwp_asan/client/extreme_lightweight_detector_quarantine.cc').read_text()
begin = source.index('static std::atomic_flag g_platform_quarantine_timing_guard')
end = source.index('\n};', begin) + 3
timer = source[begin:end]
shim_source = (chromium_source() / 'components/gwp_asan/client/extreme_lightweight_detector_malloc_shims.cc').read_text()
for text in (source, shim_source):
    assert 'liteos' not in text.replace('__NATIVE_OS__', '').lower()
begin = shim_source.index('static std::atomic_flag g_platform_sampled_free_timing_guard')
end = shim_source.index('\n};', begin) + 3
timer += '\n' + shim_source[begin:end]
assert 'LOG(' not in timer and 'new ' not in timer and 'write(' not in timer
program = r'''
#include <atomic>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
namespace base {
struct TimeDelta { uint64_t us; uint64_t InMicroseconds() const { return us; } };
struct TimeTicks {
    static inline uint64_t clock=0;
    uint64_t us;
    static TimeTicks Now() { return {clock}; }
    TimeDelta operator-(TimeTicks other) const { return {us-other.us}; }
};
}
''' + timer + r'''
int main() {
    { PlatformQuarantineTiming timing; base::TimeTicks::clock=9999; }
    assert(!g_platform_quarantine_slow_us[0]);
    base::TimeTicks::clock=0;
    {
        PlatformQuarantineTiming timing;
        base::TimeTicks::clock=1;timing.NextPhase();
        base::TimeTicks::clock=200001;timing.NextPhase();
        base::TimeTicks::clock=200003;timing.NextPhase();
        base::TimeTicks::clock=200006;timing.NextPhase();
        base::TimeTicks::clock=200010;timing.NextPhase();
        base::TimeTicks::clock=200015;
    }
    const uint64_t expected[]={200015,1,200000,2,3,4,5};
    for (size_t i=0;i<7;++i) assert(g_platform_quarantine_slow_us[i]==expected[i]);
    base::TimeTicks::clock=0;
    { PlatformQuarantineTiming timing; base::TimeTicks::clock=10000; }
    assert(g_platform_quarantine_slow_us[0]==200015);
    assert(!g_platform_quarantine_timing_guard.test_and_set(std::memory_order_acquire));
    base::TimeTicks::clock=0;
    { PlatformQuarantineTiming timing; base::TimeTicks::clock=300000; }
    assert(g_platform_quarantine_slow_us[0]==200015);
    assert(g_platform_quarantine_timing_guard.test_and_set(std::memory_order_acquire));
    g_platform_quarantine_timing_guard.clear(std::memory_order_release);
    base::TimeTicks::clock=0;
    { PlatformQuarantineTiming timing; base::TimeTicks::clock=300000; }
    assert(g_platform_quarantine_slow_us[0]==300000 && g_platform_quarantine_slow_us[1]==300000);
    for (size_t i=2;i<7;++i) assert(!g_platform_quarantine_slow_us[i]);
    base::TimeTicks::clock=0;
    {
        PlatformSampledFreeTiming timing;
        base::TimeTicks::clock=2;timing.NextPhase();
        base::TimeTicks::clock=5;timing.NextPhase();
        base::TimeTicks::clock=270005;timing.NextPhase();
        base::TimeTicks::clock=270010;
    }
    assert(g_platform_sampled_free_slow_us[0]==270010);
    assert(g_platform_sampled_free_slow_us[1]==2);
    assert(g_platform_sampled_free_slow_us[2]==3);
    assert(g_platform_sampled_free_slow_us[3]==270000);
    assert(g_platform_sampled_free_slow_us[4]==5);
    puts("quarantine timer: six phases, slow threshold, maximum and nonblocking guard passed");
}
'''
with tempfile.TemporaryDirectory(prefix='system-quarantine-timing-') as directory:
    path = Path(directory)
    (path / 'test.cc').write_text(program)
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    str(path / 'test.cc'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
