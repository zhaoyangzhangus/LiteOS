"""Check pristine, repeated and rejected FFmpeg patch application."""
from pathlib import Path
import importlib.util
import tarfile
import tempfile

root = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('query_patch', root / 'tools/nvidia/patch-video-query.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
names = ['libavutil/vulkan.c', 'libavcodec/vulkan_decode.c']
with tarfile.open(root / 'build/nvidia/vulkan/ffmpeg-7.1.1.tar.xz') as archive:
    originals = {name: archive.extractfile('ffmpeg-7.1.1/' + name).read().decode()
                 for name in names}
with tempfile.TemporaryDirectory(prefix='video-query-patch-') as directory:
    base = Path(directory)
    ffmpeg = base / 'build/nvidia/vulkan/ffmpeg-7.1.1'
    for name, text in originals.items():
        target = ffmpeg / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text)
    module.apply(base)
    modified = {name: (ffmpeg / name).read_text() for name in names}
    assert 'memset(sd_buf->mapped_mem + vp->slices_size, 0,' in modified[names[1]]
    assert modified[names[1]].index('memset(sd_buf->mapped_mem + vp->slices_size, 0,') < modified[names[1]].index('    /* Flush if needed */')
    stamps = {name: (ffmpeg / name).stat().st_mtime_ns for name in names}
    module.apply(base)
    assert all((ffmpeg / name).read_text() == modified[name] and
               (ffmpeg / name).stat().st_mtime_ns == stamps[name] for name in names)
    broken = dict(originals)
    broken[names[1]] = broken[names[1]].replace('uint32_t *result;', 'uint64_t *result;')
    for name, text in broken.items():
        (ffmpeg / name).write_text(text)
    try:
        module.apply(base)
    except RuntimeError:
        pass
    else:
        raise AssertionError('Unrecognized source revision accepted')
    assert all((ffmpeg / name).read_text() == broken[name] for name in names)
print('Vulkan query source patches: pristine release, idempotence and mismatch rejection without partial writes passed')
