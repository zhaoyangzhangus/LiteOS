#ifndef LITEOS_KERNEL_RCU_H
#define LITEOS_KERNEL_RCU_H

#include "base.h"
#include "spinlock.h"

#define RCU_CALLBACK_CAPACITY 128U

typedef void (*rcu_callback_fn)(void *argument);

bool rcu_init(void);
void rcu_read_lock(void);
void rcu_read_unlock(void);
bool rcu_read_held(void);
kstatus_t rcu_call(rcu_callback_fn callback, void *argument);
uint32_t rcu_poll(uint32_t budget);
kstatus_t rcu_synchronize(void);
bool rcu_self_test(void);

#endif
