set pagination off
set confirm off
python
import gdb
import os
import subprocess
import sys
from pathlib import Path
sys.path.insert(0, str(Path('tools/chromium').resolve()))
from trace_build import validate_chrome_symbols
binary = validate_chrome_symbols()
methods = {
    'content::RenderFrameImpl::DidCommitNavigation(': 'DOCUMENT_COMMIT',
    'blink::LocalFrameView::UpdateLifecyclePhases(': 'DOCUMENT_LIFECYCLE',
    'cc::LayerTreeHost::RequestNewLayerTreeFrameSink()': 'FRAME_SINK_REQUEST',
    'cc::mojo_embedder::AsyncLayerTreeFrameSink::SubmitCompositorFrame(': 'FRAME_SUBMIT',
}
if os.environ.get('OS_PAGE_TRACE_STAGE', 'paint') == 'startup':
    methods = {
        'content::RenderProcessHostImpl::OnProcessLaunched()': 'PROCESS_LAUNCHED',
        'content::RenderProcessHostImpl::OnChannelConnected(': 'CHANNEL_CONNECTED',
        'content::RenderFrameImpl::CreateMainFrame(': 'MAIN_FRAME_CREATE',
        'content::RenderFrameImpl::DidCommitNavigation(': 'DOCUMENT_COMMIT',
    }
elif os.environ.get('OS_PAGE_TRACE_STAGE', 'paint') == 'navigation':
    methods = {
        'content::NavigationRequest::BeginNavigation()': 'NAV_BEGIN',
        'content::NavigationRequest::OnResponseStarted(': 'NAV_RESPONSE',
        'content::NavigationRequest::ReadyToCommitNavigation(': 'NAV_READY',
        'content::RenderFrameHostImpl::CommitNavigation(': 'NAV_SEND_COMMIT',
    }
elif os.environ.get('OS_PAGE_TRACE_STAGE', 'paint') == 'receive':
    methods = {
        'content::mojom::NavigationClientStubDispatch::AcceptWithResponder(': 'NAV_RECEIVE',
        'content::RenderFrameImpl::PrepareFrameForCommit(': 'NAV_PREPARE',
        'blink::WebLocalFrameImpl::CommitNavigation(': 'BLINK_COMMIT',
        'content::RenderFrameImpl::DidCommitNavigation(': 'DOCUMENT_COMMIT',
    }
elif os.environ.get('OS_PAGE_TRACE_STAGE', 'paint') != 'paint':
    raise gdb.GdbError('Unknown page-pipeline trace stage')
entries = {method: set() for method in methods}
process = subprocess.Popen(['nm', '-C', str(binary)], stdout=subprocess.PIPE, text=True)
for row in process.stdout:
    fields = row.strip().split(None, 2)
    if len(fields) != 3 or fields[1] not in ('t', 'T') or '::$_' in fields[2]:
        continue
    for method in methods:
        if fields[2].startswith(method):
            entries[method].add(int(fields[0], 16))
if process.wait() != 0:
    raise gdb.GdbError('Cannot read page-pipeline symbols')
invalid = {method: len(addresses) for method, addresses in entries.items() if len(addresses) != 1}
if invalid:
    raise gdb.GdbError('Missing/ambiguous matching page-pipeline symbols: ' + str(invalid))
gdb.execute('target remote ' + os.environ.get('OS_PAGE_GDB_SOCKET', '/tmp/system-page.sock'))
gdb.execute('add-symbol-file build/current-inspection-types.o 0')
cpus = gdb.Value(int(gdb.parse_and_eval('&g_cpus'))).cast(gdb.lookup_type('scheduler_cpu_t').pointer())
class PageHit(gdb.Breakpoint):
    def __init__(self, address, label):
        super().__init__('*%#x' % address, type=gdb.BP_HARDWARE_BREAKPOINT)
        self.label, self.samples = label, {}
    def stop(self):
        current = cpus[gdb.selected_thread().num - 1]['queue']['current']
        if not int(current) or not int(current['process']): return False
        process = current['process'].dereference()
        if process['name'].string() != 'chrome': return False
        pid = int(process['pid'])
        count = self.samples.get(pid, 0)
        self.samples[pid] = count + 1
        if count < 8:
            print('PAGE_PIPELINE label=%s pid=%d tid=%d object=%#x' % (
                self.label, pid, int(current['tid']), int(gdb.parse_and_eval('$rdi'))), flush=True)
        if sum(self.samples.values()) >= 128: self.enabled = False
        return False
for method, addresses in entries.items():
    PageHit(next(iter(addresses)) + 0x40000000, methods[method])
print('PAGE_PIPELINE_TRACE_READY', flush=True)
end
continue
detach
quit
