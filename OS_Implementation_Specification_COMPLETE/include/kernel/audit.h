#pragma once

#include "base.h"

#define AUDIT_RING_CAPACITY 128U

enum audit_event_type {
    AUDIT_EVENT_ACCESS_DENIED = 1U,
    AUDIT_EVENT_ACCESS_ALLOWED = 2U,
    AUDIT_EVENT_SERVICE = 3U,
    AUDIT_EVENT_UPDATE = 4U,
};

typedef struct audit_record {
    uint64_t sequence;
    uint64_t timestamp_tsc;
    uint64_t object_id;
    uint32_t event_type;
    uint32_t cpu;
    uint32_t uid;
    uint32_t gid;
    uint32_t requested;
    int32_t result;
    uint32_t reserved;
} audit_record_t;

bool audit_init(void);
kstatus_t audit_emit(uint32_t event_type, uint64_t object_id,
                     uint32_t uid, uint32_t gid, uint32_t requested,
                     kstatus_t result);
size_t audit_snapshot(uint64_t after_sequence, audit_record_t *records,
                      size_t capacity, uint64_t *next_sequence);
bool audit_self_test(void);
