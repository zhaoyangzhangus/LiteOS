#include "address_space.h"

BOOLEAN liteos_page_fault_handle(UINT64 error_code, UINT64 fault_address) {
    return liteos_address_space_handle_page_fault(fault_address, error_code);
}
