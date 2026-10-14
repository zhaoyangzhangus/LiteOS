"""Validate production VFIO preflight without touching host devices."""
import importlib.util
from pathlib import Path
import resource
import tempfile

root = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('vfio_check', root/'tools/qemu-vfio-preflight.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
assert module.memory_bytes('4096') == module.memory_bytes('4G') == 4 * 1024**3
assert module.memory_bytes('8192K') == 8 * 1024**2
with tempfile.TemporaryDirectory(prefix='system-vfio-preflight-') as directory:
    fixture = Path(directory)
    devices = fixture/'devices'
    device = devices/'0000:01:00.0'
    device.mkdir(parents=True)
    driver = fixture/'vfio-pci'
    driver.mkdir()
    group = fixture/'groups/12'
    group.mkdir(parents=True)
    (device/'driver').symlink_to(driver)
    (device/'iommu_group').symlink_to(group)
    devfs = fixture/'vfio'
    devfs.mkdir()
    for name in ('vfio', '12'):
        (devfs/name).touch()

    def check(**changes):
        values = dict(memory='4G', devices=['01:00.0'], sysfs=devices,
                      devfs=devfs, limit=resource.RLIM_INFINITY)
        values.update(changes)
        module.check(**values)

    def rejects(fragment, **changes):
        try:
            check(**changes)
        except ValueError as error:
            assert fragment in str(error), str(error)
        else:
            raise AssertionError('preflight accepted invalid configuration')

    check()
    check(limit=4 * 1024**3)
    rejects('锁页额度不足', limit=8 * 1024**2)
    rejects('无权读写', access=lambda path, mode: path.name != '12')
    rejects('无权读写', access=lambda path, mode: path.name != 'vfio')
    rejects('无效 PCI', devices=['../01:00.0'])
    rejects('内存大小', memory='0')
    (device/'driver').unlink()
    rejects('尚未绑定')
    (device/'driver').symlink_to(driver)
    (device/'iommu_group').unlink()
    rejects('IOMMU')
launcher = (root/'run-qemu.sh').read_text()
assert launcher.index('tools/qemu-vfio-preflight.py') < launcher.index('if ((no_build == 0))')
assert 'prepare_only == 0 && ${#vfio_devices[@]} > 0' in launcher
print('VFIO preflight: memory units, ACLs, binding/group, malformed input, '
      'finite/unlimited memlock and early launcher ordering passed')
