"""Exercise actual PHY restart RMW, retaining every unrelated BMCR bit."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/drivers/net/rtl8126.c').read_text()
code = source[source.index('static bool rtl8126_phy_page('):
              source.index('static bool rtl8126_phy_config(')]
program = r'''
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
#define RTL8126_PHY_OCP_BASE 0xa400U
typedef struct {unsigned unused;} e1000_state_t;
static uint16_t regvalue,written;
static bool read_ok=true,write_ok=true;
static unsigned reads,writes;
static bool rtl8126_gphy_ocp_read(e1000_state_t *s,uint32_t a,uint16_t *v){
 assert(s && a==0xa400);++reads;*v=regvalue;return read_ok;
}
static bool rtl8126_gphy_ocp_write(e1000_state_t *s,uint32_t a,uint16_t v){
 assert(s && a==0xa400);++writes;written=v;return write_ok;
}
''' + code + r'''
int main(void){
 e1000_state_t s={0};
 assert(!rtl8126_phy_start_autoneg(0) && !reads && !writes);
 for(unsigned v=0;v<65536;++v){
  regvalue=v;assert(rtl8126_phy_start_autoneg(&s));
  assert(written==((v & ~0x4c00U)|0x1200U));
 }
 read_ok=false;unsigned before=writes;
 assert(!rtl8126_phy_start_autoneg(&s) && writes==before);
 read_ok=true;write_ok=false;
 assert(!rtl8126_phy_start_autoneg(&s) && writes==before+1);
}
'''
assert 'return success && rtl8126_phy_start_autoneg(state);' in source
with tempfile.TemporaryDirectory(prefix='rtl-autoneg-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(path / 'test.c'),
                    '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('RTL8126 autoneg: 65536 BMCR states, exact OCP mapping, null/read/write failures PASS')
