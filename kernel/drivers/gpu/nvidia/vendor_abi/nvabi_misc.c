#include "nvabi_internal.h"

NvU64 os_page_size = NVABI_PAGE_SIZE;
NvU64 os_max_page_size = NVABI_PAGE_SIZE;
NvU64 os_page_mask = NVABI_PAGE_MASK;
NvU8 os_page_shift = 12U;
NvBool os_cc_enabled = NV_FALSE;
NvBool os_cc_sev_snp_enabled = NV_FALSE;
NvBool os_cc_sme_enabled = NV_FALSE;
NvBool os_cc_snp_vtom_enabled = NV_FALSE;
NvBool os_cc_tdx_enabled = NV_FALSE;
NvBool os_dma_buf_enabled = NV_FALSE;
NvBool os_imex_channel_is_supported = NV_FALSE;

/* The capability filesystem is not part of the Phase 3 LiteOS surface.  A
 * null root is intentional: the capability entry points fail closed until a
 * later phase supplies their owning subsystem. */
nv_cap_t *nvidia_caps_root = NULL;

void NV_API_CALL out_string(const char *string) {
    liteos_serial_write(string == NULL ? "<null>" : string);
}

int NV_API_CALL nv_printf(NvU32 debuglevel, const char *format, ...) {
    char buffer[1024];
    va_list arguments;
    NvS32 result;
    (void)debuglevel;
    if (format == NULL) {
        nvabi_log_invalid("nv_printf", "vendor-logging");
        return -1;
    }
    va_start(arguments, format);
    result = os_vsnprintf(buffer, sizeof(buffer), format, arguments);
    va_end(arguments);
    if (result >= 0) liteos_serial_write(buffer);
    return result;
}
