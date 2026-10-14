#!/usr/bin/env python3
"""Apply checked, repeatable query-result fixes to pinned FFmpeg sources."""
from pathlib import Path


def patch(source, old, new, previous=(), updates=None):
    text = updates.get(source, source.read_text()) if updates is not None else source.read_text()
    if new in text:
        return
    candidates = [candidate for candidate in (old, *previous)
                  if text.count(candidate) == 1]
    candidates.sort(key=len, reverse=True)
    if not candidates or any(candidate not in candidates[0]
                             for candidate in candidates[1:]):
        raise RuntimeError(f'Unrecognized Vulkan query implementation: {source}')
    text = text.replace(candidates[0], new, 1)
    if updates is None:
        source.write_text(text)
    else:
        updates[source] = text


def apply(root):
    updates = {}

    def stage(source, old, new, previous=()):
        patch(source, old, new, previous, updates)

    ffmpeg = root / 'build/nvidia/vulkan/ffmpeg-7.1.1'
    source = ffmpeg / 'libavutil/vulkan.c'
    stage(source, '        pool->nb_queries = nb_queries;',
          '        pool->query_64bit = !!query_64bit;\n'
          '        pool->nb_queries = nb_queries;')
    stage(source, '''    if (data)
        *data = e->query_data;

    return vk->GetQueryPoolResults(s->hwctx->act_dev, pool->query_pool,
                                   e->query_idx,
                                   pool->nb_queries,
                                   pool->qd_size, e->query_data,
                                   pool->qd_size, qf);''', '''    if (data)
        *data = NULL;

    VkResult ret = vk->GetQueryPoolResults(s->hwctx->act_dev, pool->query_pool,
                                          e->query_idx,
                                          pool->nb_queries,
                                          pool->qd_size, e->query_data,
                                          pool->qd_size / pool->nb_queries, qf);
    /* NOT_READY and errors must never expose stale results to callers. */
    if (ret == VK_SUCCESS && data)
        *data = e->query_data;
    return ret;''')
    source = ffmpeg / 'libavcodec/vulkan_decode.c'
    stage(source, '    sd_buf = (FFVkBuffer *)vp->slices_buf->data;',
          '''    sd_buf = (FFVkBuffer *)vp->slices_buf->data;
    /* The submitted range is aligned, and pooled storage retains old bytes.
     * Keep that entire range deterministic without modifying slice payload. */
    if (!sd_buf->mapped_mem || vp->slices_size > data_size ||
        data_size > sd_buf->size)
        return AVERROR(EINVAL);
    memset(sd_buf->mapped_mem + vp->slices_size, 0,
           data_size - vp->slices_size);''')
    stage(source, '        uint32_t *result;', '        int32_t *result;')
    original = '''        av_log(avctx,
               result[0] != VK_QUERY_RESULT_STATUS_COMPLETE_KHR ?
                   AV_LOG_ERROR : AV_LOG_DEBUG,
               "Result of previous frame decoding: %u\\n", result[0]);'''
    previous = original.replace('        av_log(avctx,',
                                '        if (ret == VK_SUCCESS)\n            av_log(avctx,')
    stage(source, original, '''        /* Vulkan permits implementation-defined positive success statuses;
         * status is signed: zero is unavailable, negative indicates failure. */
        if (ret == VK_SUCCESS)
            av_log(avctx,
                   result[0] <= 0 ? AV_LOG_ERROR : AV_LOG_DEBUG,
                   "Result of previous frame decoding: %d\\n", result[0]);''',
          previous=(previous,))
    # Verify every source revision before modifying any file. A mismatch must
    # not leave a partially patched build tree that a later retry could reuse.
    for source, text in updates.items():
        source.write_text(text)


if __name__ == '__main__':
    apply(Path(__file__).resolve().parents[2])
