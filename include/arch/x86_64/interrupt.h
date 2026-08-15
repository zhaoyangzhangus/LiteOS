#pragma once
#include "../../../OS_Implementation_Specification_COMPLETE/include/arch/x86_64/interrupt.h"

/* 触发真实 #BP/#UD，验证异常分类、陷阱帧和 IRET 恢复路径。 */
bool x86_exception_self_test(void);
