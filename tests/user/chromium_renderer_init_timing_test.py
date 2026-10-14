"""Check the production low-volume renderer timer with a deterministic clock."""
from pathlib import Path as _TestPath
import sys as _test_sys
_test_sys.path.insert(0, str(_TestPath(__file__).resolve().parents[1]))
from chromium_source import chromium_source
from pathlib import Path
import subprocess
import tempfile

source = (chromium_source() / 'content/browser/renderer_host/render_process_host_impl.cc').read_text()
start = source.index('class PlatformRendererInitTiming {')
end = source.index('\n};', start) + 3
timer = source[start:end]
callback_loop = source[source.index('size_t observer_index = 0;'):]
assert callback_loop.index('std::memcpy(&observer_vtable') < callback_loop.index('observer->OnRenderProcessHostCreated(this)')
extension_source = (chromium_source() / 'extensions/browser/renderer_startup_helper.cc').read_text()
start = extension_source.index('class PlatformExtensionInitTiming {')
end = extension_source.index('\n};', start) + 3
extension_timer = extension_source[start:end]
world_read = extension_source.index('->GetAllUserScriptWorlds(ext->id());')
world_read_mark = extension_source.index('OS_EXTENSION_INIT_MARK("USER_SCRIPT_WORLDS_READ");', world_read)
world_send = extension_source.index('renderer->UpdateUserScriptWorlds(std::move(worlds_info));', world_read)
world_send_mark = extension_source.index('OS_EXTENSION_INIT_MARK("USER_SCRIPT_WORLDS_SEND");', world_send)
assert world_read < world_read_mark < world_send < world_send_mark
channel_source = (chromium_source() / 'mojo/core/channel_posix.cc').read_text()
start = channel_source.index('class PlatformChannelWriteTiming {')
end = channel_source.index('\n};', start) + 3
channel_timer = channel_source[start:end]
launch_source = (chromium_source() / 'base/process/launch_posix.cc').read_text()
for name, text in (('renderer', source), ('extension', extension_source),
                   ('channel', channel_source), ('fork', launch_source)):
    assert 'liteos' not in text.replace('__NATIVE_OS__', '').lower(), name
fd_marker = launch_source.index('"OS_CHILD_POST_SHUFFLE_FD6')
fd_guard = launch_source.rfind('#if ', 0, fd_marker)
fd_end = launch_source.index('#endif', fd_marker) + len('#endif')
fd_trace = launch_source[fd_guard:fd_end]
for flags, enabled in (([], False), (['-D__NATIVE_OS__'], False),
                       (['-D__NATIVE_OS__', '-DOS_STARTUP_TRACE'], True),
                       (['-DOS_STARTUP_TRACE'], False),
                       (['-D__NATIVE_OS__', '-DLITEOS_STARTUP_TRACE'], False)):
    result = subprocess.run(['c++', '-E', '-P', '-x', 'c++', *flags, '-'],
                            input=fd_trace, text=True, capture_output=True, check=True)
    assert ('OS_CHILD_POST_SHUFFLE_FD6' in result.stdout) == enabled
    assert ('fcntl(' in result.stdout) == enabled
    assert ('StringPrintf(' in result.stdout) == enabled
start = launch_source.index('void LogPlatformForkTiming(')
end = launch_source.index('\n}', start) + 2
fork_timer = launch_source[start:end]
program = r'''
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <sys/types.h>
#include <sstream>
#include <string>
#include <thread>
namespace base {
struct TimeDelta {
    double ms = 0;
    double InMillisecondsF() const { return ms; }
    bool operator>(TimeDelta other) const { return ms > other.ms; }
    bool operator>=(TimeDelta other) const { return ms >= other.ms; }
};
struct TimeTicks {
    static inline thread_local double clock = 0;
    double ms;
    static TimeTicks Now() { return {clock}; }
    TimeDelta operator-(TimeTicks other) const { return {ms-other.ms}; }
};
TimeDelta Milliseconds(double ms) { return {ms}; }
}
std::ostringstream log_output;
std::string startup_output;
ssize_t write(int fd, const void* buffer, size_t count) {
    assert(fd == 2);
    startup_output.append(static_cast<const char*>(buffer), count);
    return static_cast<ssize_t>(count);
}
template<class T> using raw_ptr = T*;
#define LOG(level) log_output
''' + timer + '\n' + extension_timer + '\n' + channel_timer + '\n' + fork_timer + r'''
int main() {
    { PlatformRendererInitTiming timer; base::TimeTicks::clock = 49; }
    assert(log_output.str().empty());
#ifdef OS_STARTUP_TRACE
    assert(startup_output == "OS_RENDER_PROCESS_RETURN\n");
#else
    assert(startup_output.empty());
#endif
    base::TimeTicks::clock = 0;
    {
        PlatformRendererInitTiming timer;
        timer.Mark("INIT_BEGIN");
        base::TimeTicks::clock = 10;
        timer.Mark("AFTER_LAUNCHER_CONSTRUCTION");
        auto started = base::TimeTicks::Now();
        int observer = 0;
        base::TimeTicks::clock = 15;
        timer.RecordObserver(0, &observer, 0x1000, started);
        started = base::TimeTicks::Now();
        base::TimeTicks::clock = 215;
        timer.RecordObserver(3, &observer, 0x2345, started);
        started = base::TimeTicks::Now();
        base::TimeTicks::clock = 220;
        timer.RecordObserver(4, &observer, 0x3456, started);
        timer.Mark("AFTER_CREATION_OBSERVERS");
        base::TimeTicks::clock = 221;
        timer.Mark("AFTER_BACKGROUND_TRACING");
        assert(log_output.str().empty()); // Never log per observer.
    }
    auto text = log_output.str();
    assert(text.find("total_ms=221") != std::string::npos);
    assert(text.find("slowest_phase=AFTER_CREATION_OBSERVERS phase_ms=210") != std::string::npos);
    assert(text.find("observer_index=3") != std::string::npos);
    assert(text.find("observer_ms=200") != std::string::npos);
    assert(text.find("observer_vtable=0x2345") != std::string::npos);
    log_output.str("");
    base::TimeTicks::clock = 0;
    { PlatformRendererInitTiming timer; base::TimeTicks::clock = 50; }
    assert(log_output.str().find("total_ms=50") != std::string::npos);
    assert(log_output.str().find("observer_ms=0") != std::string::npos);
    log_output.str("");
    base::TimeTicks::clock = 0;
    {
        PlatformRendererInitTiming timer;
        int* victim = new int(1);
        const auto identity = reinterpret_cast<uintptr_t>(victim);
        delete victim; // A callback may delete its own observer.
        base::TimeTicks::clock = 70;
        timer.RecordObserver(8, reinterpret_cast<const void*>(identity),
                             0x4567, {0});
        timer.Mark("AFTER_CREATION_OBSERVERS");
    }
    assert(log_output.str().find("observer_vtable=0x4567") != std::string::npos);
    log_output.str("");
    base::TimeTicks::clock = 0;
    {
        PlatformExtensionInitTiming timer;
        base::TimeTicks::clock = 10;
        timer.Mark("REGISTER_PROCESS");
        base::TimeTicks::clock = 240;
        timer.Mark("SYSTEM_FONT");
        base::TimeTicks::clock = 241;
        timer.Mark("LOAD_EXTENSIONS");
        assert(log_output.str().empty());
    }
    assert(log_output.str().find("total_ms=241") != std::string::npos);
    assert(log_output.str().find("slowest_phase=SYSTEM_FONT phase_ms=230") != std::string::npos);
    log_output.str("");
    for (bool slow_read : {false, true}) {
        base::TimeTicks::clock = 0;
        {
            PlatformExtensionInitTiming timer;
            base::TimeTicks::clock = slow_read ? 60 : 1;
            timer.Mark("USER_SCRIPT_WORLDS_READ");
            base::TimeTicks::clock += slow_read ? 1 : 60;
            timer.Mark("USER_SCRIPT_WORLDS_SEND");
        }
        assert(log_output.str().find(slow_read ?
            "slowest_phase=USER_SCRIPT_WORLDS_READ phase_ms=60" :
            "slowest_phase=USER_SCRIPT_WORLDS_SEND phase_ms=60") != std::string::npos);
        log_output.str("");
    }
    base::TimeTicks::clock = 0;
    { PlatformExtensionInitTiming timer; base::TimeTicks::clock = 49; }
    assert(log_output.str().empty());
    base::TimeTicks::clock = 0;
    PlatformExtensionInitTiming::MarkCurrent("OUTSIDE_SCOPE");
    {
        PlatformExtensionInitTiming outer;
        base::TimeTicks::clock = 5;
        PlatformExtensionInitTiming::MarkCurrent("OUTER_BEGIN");
        {
            PlatformExtensionInitTiming inner;
            base::TimeTicks::clock = 10;
            PlatformExtensionInitTiming::MarkCurrent("INNER_PHASE");
        }
        std::thread other([] {
            base::TimeTicks::clock = 1000;
            PlatformExtensionInitTiming::MarkCurrent("WRONG_THREAD");
            PlatformExtensionInitTiming own;
            base::TimeTicks::clock = 1001;
            PlatformExtensionInitTiming::MarkCurrent("OTHER_PHASE");
        });
        other.join();
        base::TimeTicks::clock = 65;
        PlatformExtensionInitTiming::MarkCurrent("OUTER_RESUMED");
        base::TimeTicks::clock = 66;
        assert(log_output.str().empty());
    }
    assert(log_output.str().find("total_ms=66") != std::string::npos);
    assert(log_output.str().find("slowest_phase=OUTER_RESUMED phase_ms=60") != std::string::npos);
    assert(log_output.str().find("WRONG_THREAD") == std::string::npos);
    log_output.str("");
    PlatformExtensionInitTiming::MarkCurrent("AFTER_SCOPE");
    assert(log_output.str().empty());
    base::TimeTicks::clock = 0;
    {
        PlatformChannelWriteTiming timer;
        base::TimeTicks::clock = 200;
        timer.Locked();
        base::TimeTicks::clock = 205;
        assert(log_output.str().empty());
    }
    assert(log_output.str().find("total_ms=205 lock_ms=200 send_ms=5") != std::string::npos);
    log_output.str("");
    base::TimeTicks::clock = 0;
    {
        PlatformChannelWriteTiming timer;
        base::TimeTicks::clock = 1;
        timer.Locked();
        base::TimeTicks::clock = 301;
    }
    assert(log_output.str().find("total_ms=301 lock_ms=1 send_ms=300") != std::string::npos);
    log_output.str("");
    base::TimeTicks::clock = 0;
    { PlatformChannelWriteTiming timer; timer.Locked(); base::TimeTicks::clock = 49; }
    assert(log_output.str().empty());
    base::TimeTicks::clock = 9;
    LogPlatformForkTiming({0}, 123);
    assert(log_output.str().empty());
    base::TimeTicks::clock = 300;
    LogPlatformForkTiming({0}, 0);
    assert(log_output.str().empty());
    LogPlatformForkTiming({0}, 123);
    assert(log_output.str().find("child=123 total_ms=300") != std::string::npos);
}
'''
with tempfile.TemporaryDirectory(prefix='system-renderer-timing-') as directory:
    path = Path(directory)
    (path / 'test.cc').write_text(program)
    for flags in ([], ['-DOS_STARTUP_TRACE'], ['-DLITEOS_STARTUP_TRACE']):
        subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread',
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie', *flags,
                        str(path / 'test.cc'), '-o', str(path / 'test')], check=True)
        subprocess.run([str(path / 'test')], check=True)
print('Neutral renderer/extension/channel/fork timers: slowest phase/observer, nested scopes, '
      'thread isolation, 50ms threshold; startup trace flag and child descriptor diagnostics gates passed')
