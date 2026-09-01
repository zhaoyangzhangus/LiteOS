#include <arch/x86_64/paging.h>
#include <kernel/audio.h>
#include "internal.h"

/* REFACTOR_P8_XHCI_AUDIO_OWNER: USB audio stream policy and UAPI state. */

static uint8_t g_xhci_audio_slot;

static xhci_device_context_t *
xhci_audio_context(
    xhci_state_t *state)
{
    if(state == 0 ||
       !state->initialized)
    {
        return 0;
    }


    /*
     * O(1) steady-state path.
     */
    if(g_xhci_audio_slot != 0U)
    {
        xhci_slot_device_t *slot_dev =
            xhci_topology_slot(g_xhci_audio_slot);


        if(slot_dev->used &&
           slot_dev->context.device_slot ==
               g_xhci_audio_slot &&
           slot_dev->context.audio_endpoint !=
               0U)
        {
            return
                &slot_dev->context;
        }


        /*
         * Device was unplugged or Slot was reused.
         */
        g_xhci_audio_slot =
            0U;
    }


    /*
     * Slow discovery path.
     */
    uint32_t limit =
        state->max_slots;


    if(limit >=
       XHCI_MAX_SLOT_TABLE)
    {
        limit =
            XHCI_MAX_SLOT_TABLE -
            1U;
    }


    for(uint32_t slot = 1U;
        slot <= limit;
        ++slot)
    {
        xhci_slot_device_t *slot_dev =
            xhci_topology_slot(slot);


        if(!slot_dev->used ||
           slot_dev->context.device_slot !=
               (uint8_t)slot ||
           slot_dev->context.audio_endpoint ==
               0U)
        {
            continue;
        }


        g_xhci_audio_slot =
            (uint8_t)slot;


        return
            &slot_dev->context;
    }


    return 0;
}



/*
 * V3.10.6B8E AUDIO CANONICAL UAPI
 *
 * xhci_audio_context_commit() removed.
 *
 * Audio UAPI mutates the canonical Slot.context directly;
 * there is no secondary context to commit or synchronize.
 */



/*
 * xhci_queue_audio_transfer() still consumes state->audio_*.
 *
 * Adapt an inactive canonical Slot context to that old ABI only
 * for the duration of the doorbell operation.
 */
/*
 * V3.10.4B DIRECT AUDIO OWNER
 *
 * Published Audio runtime state lives permanently in the supplied
 * device context.
 *
 * No Slot.context -> state->device -> Slot.context adaptation is
 * required for queue submission anymore.
 */
/*
 * V3.10.6B8F DIRECT AUDIO QUEUE
 *
 * xhci_queue_audio_owner() removed.
 *
 * Audio UAPI already owns the canonical Slot.context pointer and
 * submits directly through xhci_queue_audio_transfer_device().
 */



uint32_t
xhci_usb_audio_completed(void)
{
    if(!xhci_controller_state()->initialized ||
       !xhci_topology_audio_configured())
    {
        return 0U;
    }


    xhci_device_context_t *device =
        xhci_audio_context(
            xhci_controller_state());


    return
        device != 0
        ? device->audio_completed
        : 0U;
}


struct device *
xhci_audio_device(void)
{
    return
        xhci_controller_state()->initialized &&
        xhci_topology_audio_configured() &&
        xhci_controller_state()->pci != 0
        ? (struct device *)
            &xhci_controller_state()->pci->device
        : 0;
}


static void
xhci_audio_lock(void)
{
    xhci_event_lock(xhci_controller_state());
}


static void
xhci_audio_unlock(void)
{
    xhci_event_unlock(xhci_controller_state());
}


static bool
xhci_audio_format_supported(
    const xhci_device_context_t *device,
    const audio_format_t *format)
{
    uint32_t sample_bits;


    if(device == 0 ||
       format == 0 ||
       format->channels !=
           device->audio_channels ||
       format->sample_rate !=
           device->audio_sample_rate)
    {
        return false;
    }


    switch(format->sample_format)
    {
        case AUDIO_SAMPLE_S16_LE:
            sample_bits = 16U;
            break;

        case AUDIO_SAMPLE_S24_LE:
            sample_bits = 24U;
            break;

        case AUDIO_SAMPLE_S32_LE:
            sample_bits = 32U;
            break;

        default:
            return false;
    }


    return
        sample_bits ==
        device->
            audio_bit_resolution;
}


kstatus_t
xhci_audio_stream_configure(
    struct audio_stream *stream)
{
    audio_stream_t *audio =
        (audio_stream_t *)stream;

    audio_format_t format;


    if(audio == 0)
    {
        return K_EINVAL;
    }


    if(!xhci_controller_state()->initialized ||
       !xhci_topology_audio_configured() ||
       audio_stream_device(audio) !=
           xhci_audio_device())
    {
        return K_ENOENT;
    }


    audio_stream_get_format(
        audio,
        &format);


    xhci_audio_lock();


    xhci_device_context_t *device =
        xhci_audio_context(
            xhci_controller_state());


    if(device == 0)
    {
        xhci_audio_unlock();
        return K_ENOENT;
    }


    if(audio_stream_direction(audio) !=
           (device->audio_endpoint_in
                ? AUDIO_CAPTURE
                : AUDIO_PLAYBACK) ||
       !xhci_audio_format_supported(
           device,
           &format) ||
       audio_stream_period_bytes(audio) >
           PAGE_SIZE)
    {
        xhci_audio_unlock();
        return K_EINVAL;
    }


    if(device->audio_stream != 0 &&
       device->audio_stream != audio)
    {
        xhci_audio_unlock();
        return K_EBUSY;
    }


    device->audio_stream =
        audio;

    device->audio_stream_bound =
        true;

    device->audio_stream_running =
        false;

    device->audio_stream_queued =
        false;

    device->audio_period =
        0U;

    device->audio_frames =
        0U;




    xhci_audio_unlock();

    return K_OK;
}


kstatus_t
xhci_audio_stream_start(
    struct audio_stream *stream)
{
    audio_stream_t *audio =
        (audio_stream_t *)stream;

    bool success =
        true;


    if(audio == 0)
    {
        return K_EINVAL;
    }


    if(!xhci_controller_state()->initialized ||
       !xhci_topology_audio_configured())
    {
        return K_ENOENT;
    }


    xhci_audio_lock();


    xhci_device_context_t *device =
        xhci_audio_context(
            xhci_controller_state());


    if(device == 0 ||
       device->audio_stream != audio ||
       !device->audio_stream_bound)
    {
        xhci_audio_unlock();
        return K_ENOENT;
    }


    device->audio_stream_running =
        true;


    if(device->audio_stream_queued &&
       !device->audio_transfer_pending)
    {
        success =
            xhci_queue_audio_transfer_device(
                xhci_controller_state(),
                device);


        if(!success)
        {
            device->
                audio_stream_running =
                    false;
        }
    }




    xhci_audio_unlock();


    return
        success
        ? K_OK
        : K_EIO;
}


kstatus_t
xhci_audio_stream_stop(
    struct audio_stream *stream)
{
    audio_stream_t *audio =
        (audio_stream_t *)stream;


    if(audio == 0)
    {
        return K_EINVAL;
    }


    if(!xhci_controller_state()->initialized ||
       !xhci_topology_audio_configured())
    {
        return K_ENOENT;
    }


    xhci_audio_lock();


    xhci_device_context_t *device =
        xhci_audio_context(
            xhci_controller_state());


    if(device == 0 ||
       device->audio_stream != audio ||
       !device->audio_stream_bound)
    {
        xhci_audio_unlock();
        return K_ENOENT;
    }


    device->audio_stream_running =
        false;


    if(!device->
           audio_transfer_pending)
    {
        device->audio_stream_queued =
            false;
    }




    xhci_audio_unlock();

    return K_OK;
}


kstatus_t
xhci_audio_stream_queue(
    struct audio_stream *stream,
    uint32_t period,
    uint32_t frames)
{
    audio_stream_t *audio =
        (audio_stream_t *)stream;

    audio_format_t format;

    bool success =
        true;


    if(audio == 0 ||
       frames == 0U)
    {
        return K_EINVAL;
    }


    if(!xhci_controller_state()->initialized ||
       !xhci_topology_audio_configured())
    {
        return K_ENOENT;
    }


    audio_stream_get_format(
        audio,
        &format);


    xhci_audio_lock();


    xhci_device_context_t *device =
        xhci_audio_context(
            xhci_controller_state());


    if(device == 0 ||
       device->audio_stream != audio ||
       !device->audio_stream_bound)
    {
        xhci_audio_unlock();
        return K_ENOENT;
    }


    if(device->audio_stream_queued ||
       device->audio_transfer_pending)
    {
        xhci_audio_unlock();
        return K_EBUSY;
    }


    if(period >=
           format.period_count ||
       frames >
           format.period_frames)
    {
        xhci_audio_unlock();
        return K_EINVAL;
    }


    uint64_t bytes =
        audio_stream_bytes_for_frames(
            audio,
            frames);


    if(bytes == 0U ||
       bytes > PAGE_SIZE)
    {
        xhci_audio_unlock();
        return K_EINVAL;
    }


    device->audio_period =
        period;

    device->audio_frames =
        frames;

    device->audio_stream_queued =
        true;


    if(device->audio_stream_running)
    {
        success =
            xhci_queue_audio_transfer_device(
                xhci_controller_state(),
                device);


        if(!success)
        {
            device->
                audio_stream_queued =
                    false;
        }
    }




    xhci_audio_unlock();


    return
        success
        ? K_OK
        : K_EIO;
}


kstatus_t
xhci_audio_stream_reset(
    struct audio_stream *stream)
{
    audio_stream_t *audio =
        (audio_stream_t *)stream;


    if(audio == 0)
    {
        return K_EINVAL;
    }


    if(!xhci_controller_state()->initialized ||
       !xhci_topology_audio_configured())
    {
        return K_ENOENT;
    }


    xhci_audio_lock();


    xhci_device_context_t *device =
        xhci_audio_context(
            xhci_controller_state());


    if(device == 0 ||
       device->audio_stream != audio ||
       !device->audio_stream_bound)
    {
        xhci_audio_unlock();
        return K_ENOENT;
    }


    device->audio_stream_running =
        false;

    device->audio_stream_queued =
        false;

    device->audio_period =
        0U;

    device->audio_frames =
        0U;




    xhci_audio_unlock();

    return K_OK;
}


kstatus_t
xhci_audio_stream_disconnect(
    struct audio_stream *stream)
{
    audio_stream_t *audio =
        (audio_stream_t *)stream;


    if(audio == 0)
    {
        return K_EINVAL;
    }


    if(!xhci_controller_state()->initialized ||
       !xhci_topology_audio_configured())
    {
        return K_ENOENT;
    }


    xhci_audio_lock();


    xhci_device_context_t *device =
        xhci_audio_context(
            xhci_controller_state());


    if(device == 0 ||
       device->audio_stream != audio)
    {
        xhci_audio_unlock();
        return K_ENOENT;
    }


    device->audio_stream =
        0;

    device->audio_stream_running =
        false;

    device->audio_stream_queued =
        false;


    /*
     * Keep bound set until any already posted Transfer Event has
     * been consumed.  This preserves the previous behavior and
     * prevents the autonomous self-test queue from restarting.
     */
    device->audio_stream_bound =
        true;




    xhci_audio_unlock();

    return K_OK;
}
