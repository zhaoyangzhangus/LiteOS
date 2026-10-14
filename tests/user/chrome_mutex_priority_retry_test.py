"""Execute the production Abseil priority-query interval on failure/recovery."""
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from chromium_source import chromium_source

source = (chromium_source() / 'third_party/abseil-cpp/absl/synchronization/mutex.cc').read_text()
start = source.index('    if (s->next_priority_read_cycles < now_cycles) {')
opening = source.index('{', start)
depth = 1
end = opening + 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
body = source[start:end]
program = r'''
#include <cassert>
#include <cstdint>
struct State {int64_t next_priority_read_cycles=0;int priority=7;};
struct sched_param {int sched_priority;};
static unsigned calls=0,logs=0;
static int error=1;
static int pthread_self(){return 1;}
static int pthread_getschedparam(int self,int* policy,sched_param* parameter){
 assert(self==1);++calls;if(error)return error;
 *policy=0;parameter->sched_priority=3;return 0;
}
struct CycleClock {static int64_t Frequency(){return 100;}};
#define ABSL_RAW_LOG(...) (++logs)
static void query(State* s,int64_t now_cycles){
''' + body + r'''
}
int main(){
 State s;
 for(unsigned i=0;i<10000;++i)query(&s,1);
 assert(calls==1&&logs==1&&s.priority==7);
 query(&s,101);assert(calls==1);
 query(&s,102);assert(calls==2&&logs==2&&s.priority==7);
 error=0;query(&s,203);
 assert(calls==3&&logs==2&&s.priority==3);
 for(unsigned i=0;i<10000;++i)query(&s,204);
 assert(calls==3&&logs==2);
 query(&s,304);assert(calls==4&&logs==2);
}
'''
with tempfile.TemporaryDirectory(prefix='chrome-priority-retry-') as directory:
    path = Path(directory)
    (path/'test.cc').write_text(program)
    subprocess.run(['c++','-std=c++20','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    str(path/'test.cc'),'-o',str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True, timeout=10)
print('Mutex priority queries: 20000 repeated enqueues, failure backoff, '
      'deadline boundary, preserved priority and successful recovery passed')
