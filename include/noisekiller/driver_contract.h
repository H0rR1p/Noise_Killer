#ifndef NOISEKILLER_DRIVER_CONTRACT_H
#define NOISEKILLER_DRIVER_CONTRACT_H

/* DriverContract v1. No pointers, variable-length fields or user addresses.
 * This header defines a proposed ABI, NOT a shipping WaveRT driver.
 * All operations require exact byte sizes, zero reserved fields and v1.
 * Driver control ACL: LocalSystem + installed TrustedBroker service SID only.
 * Broker grants a file-object/EPROCESS-bound handle to one admitted client.
 * Duplication or a matching SID never grants another process producer rights.
 */
#include <stdint.h>

#define NK_DRIVER_VERSION 1u
#define NK_DRIVER_RATE 16000u
#define NK_DRIVER_CHANNELS 1u
#define NK_DRIVER_BITS 16u
#define NK_DRIVER_HOP 160u
#define NK_DRIVER_QUEUE_HOPS 4u
#define NK_DRIVER_WATCHDOG_MS 100u
#define NK_DRIVER_DEVICE_TYPE 0x8000u
#define NK_DRIVER_IOCTL(function) ((NK_DRIVER_DEVICE_TYPE << 16u) | (3u << 14u) | ((function) << 2u))
#define NK_IOCTL_GRANT NK_DRIVER_IOCTL(0x800u)
#define NK_IOCTL_CAPABILITIES NK_DRIVER_IOCTL(0x801u)
#define NK_IOCTL_CONFIGURE NK_DRIVER_IOCTL(0x802u)
#define NK_IOCTL_SUBMIT NK_DRIVER_IOCTL(0x803u)
#define NK_IOCTL_STATS NK_DRIVER_IOCTL(0x804u)
#define NK_IOCTL_STOP NK_DRIVER_IOCTL(0x805u)

#define NK_CAP_CAPTURE_ONLY (UINT64_C(1) << 0u)
#define NK_CAP_BOUNDED_COPY (UINT64_C(1) << 1u)
#define NK_CAP_TRUSTED_BROKER (UINT64_C(1) << 2u)
#define NK_CAP_INDEPENDENT_CLOCK (UINT64_C(1) << 3u)
#define NK_CAP_PER_STREAM_CURSOR (UINT64_C(1) << 4u)
#define NK_CAP_FAIL_SILENT (UINT64_C(1) << 5u)
#define NK_REQUIRED_CAPABILITIES (NK_CAP_CAPTURE_ONLY | NK_CAP_BOUNDED_COPY | NK_CAP_TRUSTED_BROKER | NK_CAP_INDEPENDENT_CLOCK | NK_CAP_PER_STREAM_CURSOR | NK_CAP_FAIL_SILENT)

#define NK_BROKER_SERVICE_NAME L"NoiseKillerBroker"
#define NK_BROKER_PIPE_NAME L"\\\\.\\pipe\\NoiseKiller.TrustedBroker.v1"

enum NK_OPERATION {
    NK_OP_GRANT = 1,
    NK_OP_CAPABILITIES = 2,
    NK_OP_CONFIGURE = 3,
    NK_OP_SUBMIT = 4,
    NK_OP_STATS = 5,
    NK_OP_STOP = 6
};

#pragma pack(push, 8)
typedef struct NK_HEADER {
    uint32_t size;
    uint16_t version;
    uint16_t operation;
    uint64_t epoch;
} NK_HEADER;

/* Only the broker may issue grant to the kernel. All peer information MUST
 * be derived from OS-authenticated handles/token/process references, not from
 * a caller's claimed executable name, PID, SID or fields below.
 */
typedef struct NK_GRANT {
    NK_HEADER header;
    uint32_t process_id;
    uint32_t session_id;
    uint64_t process_creation_time;
} NK_GRANT;

typedef struct NK_BROKER_REQUEST {
    NK_HEADER header;
    uint32_t claimed_process_id; /* convenience only; never authority */
    uint32_t claimed_session_id;
} NK_BROKER_REQUEST;

typedef struct NK_BROKER_REPLY {
    NK_HEADER header;
    uint32_t status;
    uint32_t reserved;
    uint64_t duplicated_device_handle; /* value in client handle table */
} NK_BROKER_REPLY;

typedef struct NK_CAPABILITIES {
    NK_HEADER header;
    uint64_t flags;
    uint64_t driver_build;
    uint64_t qpc_frequency;
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t bits;
    uint32_t hop_samples;
    uint32_t max_queue_hops;
    uint32_t watchdog_ms;
} NK_CAPABILITIES;

typedef struct NK_CONFIGURE {
    NK_HEADER header;
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t bits;
    uint32_t hop_samples;
    uint32_t max_queue_hops;
    uint32_t reserved;
} NK_CONFIGURE;

typedef struct NK_SUBMIT {
    NK_HEADER header;
    uint64_t sequence;
    uint64_t first_sample_index;
    uint64_t capture_qpc; /* diagnostic input timestamp, never endpoint clock */
    uint32_t sample_count;
    uint32_t reserved;
    int16_t pcm[NK_DRIVER_HOP];
} NK_SUBMIT;

typedef struct NK_STATS {
    NK_HEADER header;
    uint64_t endpoint_sample_position;
    uint64_t endpoint_qpc;
    uint64_t accepted_blocks;
    uint64_t rejected_blocks;
    uint64_t zero_filled_samples;
    uint64_t watchdog_stops;
    uint64_t consumer_overruns;
    uint32_t queued_samples;
    uint32_t active_consumers;
    uint32_t owner_active;
    uint32_t reserved;
} NK_STATS;
#pragma pack(pop)

#ifdef __cplusplus
#include <type_traits>
static_assert(sizeof(NK_HEADER) == 16, "Driver ABI header size changed");
static_assert(sizeof(NK_GRANT) == 32, "Driver ABI grant size changed");
static_assert(sizeof(NK_BROKER_REQUEST) == 24, "Broker ABI request size changed");
static_assert(sizeof(NK_BROKER_REPLY) == 32, "Broker ABI reply size changed");
static_assert(sizeof(NK_CAPABILITIES) == 64, "Driver ABI capabilities size changed");
static_assert(sizeof(NK_CONFIGURE) == 40, "Driver ABI format size changed");
static_assert(sizeof(NK_SUBMIT) == 368, "Driver ABI block size changed");
static_assert(sizeof(NK_STATS) == 88, "Driver ABI statistics size changed");
static_assert(std::is_trivially_copyable<NK_SUBMIT>::value, "PCM message must be byte-copyable");
#endif
#endif
