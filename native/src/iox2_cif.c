/* iceoryx2 CIF wrapper — blackboard + pub/sub (Windows DLL / Linux .so). */

#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "iox2_cif.h"

#include "iox2/iceoryx2.h"

#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <excpt.h>
#define IOX2_CIF_ALIGN_OF(T) __alignof(T)
#else
#include <dlfcn.h>
#include <dirent.h>
#include <errno.h>
#include <stdalign.h>
#include <stdint.h>
#include <sys/stat.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#define IOX2_CIF_ALIGN_OF(T) alignof(T)
#endif

#define IOX2_CIF_MAX_SLOTS 256
#define IOX2_CIF_MAX_READERS 4096u
#define IOX2_CIF_LOCK_TIMEOUT_MS 5000u
#define IOX2_CIF_SHUTDOWN_LOCK_TIMEOUT_MS 10000u
#define IOX2_CIF_TYPE_NAME_CAP 128
#define IOX2_CIF_TOPIC_CAP 256
#define IOX2_CIF_NAME_CAP 256
#define IOX2_CIF_DIR_MAX_TOPICS 64
#define IOX2_CIF_DIR_TYPE_NAME "iox2_cif.directory"
#define IOX2_CIF_KEY_TYPE_NAME "u64"
#ifdef _WIN32
#define IOX2_CIF_META_ROOT "C:\\Temp\\iox2_cif\\meta"
#else
#define IOX2_CIF_META_ROOT "/tmp/iox2_cif/meta"
#endif
#define IOX2_CIF_META_MAGIC 0x314D5631u
#define IOX2_CIF_META_VERSION 1u

typedef enum iox2_cif_kind {
    IOX2_CIF_KIND_FREE = 0,
    IOX2_CIF_KIND_BB,
    IOX2_CIF_KIND_WRITER,
    IOX2_CIF_KIND_READER,
    IOX2_CIF_KIND_PS_WRITER,
    IOX2_CIF_KIND_PS_SUB,
    IOX2_CIF_KIND_PCAP_WRITER,
    IOX2_CIF_KIND_PCAP_SUB
} iox2_cif_kind;

typedef struct iox2_cif_writer iox2_cif_writer;
typedef struct iox2_cif_bb iox2_cif_bb;
typedef struct iox2_cif_reader iox2_cif_reader;

struct iox2_cif_writer {
    uint64_t handle;
    iox2_cif_bb* bb;
    char topic[IOX2_CIF_TOPIC_CAP];
    char type_name[IOX2_CIF_TYPE_NAME_CAP]; /* iceoryx2 value type_name */
    uint64_t key;
    uint32_t elem_size;
    uint32_t elem_align;
    uint32_t max_count;
    uint32_t value_size;
    uint32_t value_align;
    int is_array;
    int valid;
    void* initial;
    uint32_t initial_count;
    char* descriptor; /* heap, never NULL after create; may be "" */
    void* create_payload;
    iox2_entry_handle_mut_h entry;
    iox2_cif_writer* next;
};

struct iox2_cif_bb {
    uint64_t handle;
    char name[IOX2_CIF_NAME_CAP];
    uint32_t max_readers;
    int started;
    uint32_t staged_count;
    char staged_topics[IOX2_CIF_DIR_MAX_TOPICS][IOX2_CIF_TOPIC_CAP];
    iox2_cif_writer* writers;
    iox2_port_factory_blackboard_h service;
    iox2_writer_h writer;
    iox2_service_name_h service_name;
};

struct iox2_cif_reader {
    uint64_t handle;
    char type_name[IOX2_CIF_TYPE_NAME_CAP];
    uint64_t key;
    uint32_t elem_size;
    uint32_t elem_align;
    uint32_t max_count;
    uint32_t value_size;
    uint32_t value_align;
    int is_array;
    int is_raw;
    int is_field;
    int swap_endian;
    int src_kind; /* iox2_cif_scalar; 0 = exact copy of the published value */
    int dst_kind;
    uint32_t dst_elem_size;
    uint32_t field_offset;
    uint32_t field_size;
    iox2_port_factory_blackboard_h service;
    iox2_reader_h reader;
    iox2_entry_handle_h entry;
    iox2_service_name_h service_name;
};

typedef struct iox2_cif_handle_slot {
    uint32_t gen;
    iox2_cif_kind kind;
    void* ptr;
} iox2_cif_handle_slot;

typedef struct iox2_cif_state {
#ifdef _WIN32
    HANDLE mutex;
    HANDLE wait_thread;
#else
    pthread_rwlock_t lock;
    pthread_t wait_thread;
#endif
    int running;
    int wait_started;
    iox2_node_h node;
    iox2_cif_handle_slot slots[IOX2_CIF_MAX_SLOTS];
} iox2_cif_state;

static iox2_cif_state g_state;
#ifdef _WIN32
static volatile LONG g_pinned;
#else
static volatile int g_pinned;
#endif
static char g_last_detail[192];

static void dbg_log(const char* fmt, ...);

/*
 * LabVIEW FreeLibrary's this DLL when the last VI that referenced it closes.
 * A background node.wait() thread (and the statically linked Rust runtime) cannot
 * survive that unmap — LabVIEW.exe exits. Pin so unload is deferred until process exit.
 */
static void pin_library(void) {
#ifdef _WIN32
    HMODULE self = NULL;
    wchar_t path[MAX_PATH];
    if (InterlockedCompareExchange(&g_pinned, 1, 0) != 0) {
        return;
    }
    if (GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            (LPCWSTR)(const void*)&pin_library,
            &self)) {
        dbg_log("dll pinned");
        return;
    }
    g_pinned = 0;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            (LPCWSTR)(const void*)&pin_library,
            &self)
        || self == NULL) {
        return;
    }
    if (GetModuleFileNameW(self, path, MAX_PATH) == 0) {
        return;
    }
    if (LoadLibraryW(path) != NULL) {
        InterlockedExchange(&g_pinned, 1);
        dbg_log("dll refcount bumped (pin failed)");
    }
#else
    Dl_info info;
    if (!__sync_bool_compare_and_swap(&g_pinned, 0, 1)) {
        return;
    }
    if (dladdr((void*)&pin_library, &info) == 0 || info.dli_fname == NULL) {
        g_pinned = 0;
        return;
    }
    if (dlopen(info.dli_fname, RTLD_NOW | RTLD_NODELETE) == NULL) {
        g_pinned = 0;
        return;
    }
    dbg_log("so pinned path=%s", info.dli_fname);
#endif
}

static void dbg_log(const char* fmt, ...) {
    FILE* f = NULL;
    va_list ap;
#ifdef _WIN32
    f = fopen("C:\\Temp\\iox2_cif.log", "a");
#else
    f = fopen("/tmp/iox2_cif.log", "a");
#endif
    if (f == NULL) {
        return;
    }
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static void set_last_detail(const char* text) {
    if (text == NULL || text[0] == 0) {
        g_last_detail[0] = 0;
        return;
    }
    snprintf(g_last_detail, sizeof(g_last_detail), "%s", text);
    dbg_log("detail: %s", g_last_detail);
}

static uint32_t align_up_u32(uint32_t value, uint32_t align) {
    uint32_t a = align == 0 ? 1u : align;
    return (uint32_t)((value + (a - 1u)) & ~(a - 1u));
}

static uint32_t max_u32(uint32_t a, uint32_t b) {
    return a > b ? a : b;
}

static int starts_with(const char* s, const char* prefix) {
    size_t n = strlen(prefix);
    return strncmp(s, prefix, n) == 0;
}

#ifdef _WIN32
static iox2_cif_status ensure_lock(void) {
    HANDLE created = NULL;
    HANDLE previous = NULL;
    pin_library();
    if (g_state.mutex != NULL) {
        return IOX2_CIF_OK;
    }
    created = CreateMutexW(NULL, FALSE, NULL);
    if (created == NULL) {
        return IOX2_CIF_ERR_INTERNAL;
    }
    previous = (HANDLE)InterlockedCompareExchangePointer((PVOID*)&g_state.mutex, created, NULL);
    if (previous != NULL) {
        CloseHandle(created);
    }
    return g_state.mutex != NULL ? IOX2_CIF_OK : IOX2_CIF_ERR_INTERNAL;
}

static iox2_cif_status lock_exclusive_ms(uint32_t timeout_ms) {
    DWORD wr = 0;
    iox2_cif_status st = ensure_lock();
    if (st != IOX2_CIF_OK) {
        return st;
    }
    wr = WaitForSingleObject(g_state.mutex, timeout_ms);
    if (wr == WAIT_OBJECT_0 || wr == WAIT_ABANDONED) {
        return IOX2_CIF_OK;
    }
    if (wr == WAIT_TIMEOUT) {
        return IOX2_CIF_ERR_LOCK_TIMEOUT;
    }
    return IOX2_CIF_ERR_INTERNAL;
}
#else
static pthread_once_t g_lock_once = PTHREAD_ONCE_INIT;

static void init_lock_once(void) {
    pin_library();
    pthread_rwlock_init(&g_state.lock, NULL);
}

static iox2_cif_status ensure_lock(void) {
    pthread_once(&g_lock_once, init_lock_once);
    return IOX2_CIF_OK;
}

static void lock_deadline(uint32_t timeout_ms, struct timespec* ts) {
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += (time_t)(timeout_ms / 1000u);
    ts->tv_nsec += (long)(timeout_ms % 1000u) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec += 1;
        ts->tv_nsec -= 1000000000L;
    }
}

static iox2_cif_status lock_exclusive_ms(uint32_t timeout_ms) {
    struct timespec ts;
    int rc = 0;
    iox2_cif_status st = ensure_lock();
    if (st != IOX2_CIF_OK) {
        return st;
    }
    lock_deadline(timeout_ms, &ts);
    rc = pthread_rwlock_timedwrlock(&g_state.lock, &ts);
    if (rc == 0) {
        return IOX2_CIF_OK;
    }
    if (rc == ETIMEDOUT) {
        return IOX2_CIF_ERR_LOCK_TIMEOUT;
    }
    return IOX2_CIF_ERR_INTERNAL;
}

static iox2_cif_status lock_shared_ms(uint32_t timeout_ms) {
    struct timespec ts;
    int rc = 0;
    iox2_cif_status st = ensure_lock();
    if (st != IOX2_CIF_OK) {
        return st;
    }
    lock_deadline(timeout_ms, &ts);
    rc = pthread_rwlock_timedrdlock(&g_state.lock, &ts);
    if (rc == 0) {
        return IOX2_CIF_OK;
    }
    if (rc == ETIMEDOUT) {
        return IOX2_CIF_ERR_LOCK_TIMEOUT;
    }
    return IOX2_CIF_ERR_INTERNAL;
}
#endif

static iox2_cif_status lock_exclusive(void) {
    return lock_exclusive_ms(IOX2_CIF_LOCK_TIMEOUT_MS);
}

static iox2_cif_status lock_shared(void) {
#ifdef _WIN32
    return lock_exclusive_ms(IOX2_CIF_LOCK_TIMEOUT_MS);
#else
    return lock_shared_ms(IOX2_CIF_LOCK_TIMEOUT_MS);
#endif
}

static void unlock_exclusive(void) {
#ifdef _WIN32
    ReleaseMutex(g_state.mutex);
#else
    pthread_rwlock_unlock(&g_state.lock);
#endif
}

static void unlock_shared(void) {
#ifdef _WIN32
    ReleaseMutex(g_state.mutex);
#else
    pthread_rwlock_unlock(&g_state.lock);
#endif
}

static iox2_cif_status copy_cstr_bounded(char* dst, size_t dst_sz, const char* src) {
    size_t i = 0;
    if (dst == NULL || dst_sz == 0 || src == NULL) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
#ifdef _WIN32
    __try {
#endif
        for (i = 0; i + 1 < dst_sz; ++i) {
            dst[i] = src[i];
            if (src[i] == 0) {
                return IOX2_CIF_OK;
            }
        }
        dst[dst_sz - 1] = 0;
        return IOX2_CIF_OK;
#ifdef _WIN32
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (dst_sz > 0) {
            dst[0] = 0;
        }
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
#endif
}

static iox2_cif_status copy_bytes_bounded(void* dst, const void* src, size_t n) {
    if (n == 0) {
        return IOX2_CIF_OK;
    }
    if (dst == NULL || src == NULL) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
#ifdef _WIN32
    __try {
        memcpy(dst, src, n);
        return IOX2_CIF_OK;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
#else
    memcpy(dst, src, n);
    return IOX2_CIF_OK;
#endif
}

static iox2_cif_status store_u64(uint64_t* out, uint64_t value) {
    if (out == NULL) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
#ifdef _WIN32
    __try {
        *out = value;
        return IOX2_CIF_OK;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
#else
    *out = value;
    return IOX2_CIF_OK;
#endif
}

static iox2_cif_status store_u32(uint32_t* out, uint32_t value) {
    if (out == NULL) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
#ifdef _WIN32
    __try {
        *out = value;
        return IOX2_CIF_OK;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
#else
    *out = value;
    return IOX2_CIF_OK;
#endif
}

static iox2_cif_status store_i32(int32_t* out, int32_t value) {
    if (out == NULL) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
#ifdef _WIN32
    __try {
        *out = value;
        return IOX2_CIF_OK;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
#else
    *out = value;
    return IOX2_CIF_OK;
#endif
}

static iox2_cif_status store_u8(uint8_t* out, uint8_t value) {
    if (out == NULL) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
#ifdef _WIN32
    __try {
        *out = value;
        return IOX2_CIF_OK;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
#else
    *out = value;
    return IOX2_CIF_OK;
#endif
}

static iox2_cif_status measure_cstr(const char* src, size_t max_strlen, size_t* out_len) {
    size_t i = 0;
    if (out_len == NULL) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    if (src == NULL) {
        *out_len = 0;
        return IOX2_CIF_OK;
    }
#ifdef _WIN32
    __try {
#endif
        for (i = 0; i <= max_strlen; ++i) {
            if (src[i] == 0) {
                *out_len = i;
                return IOX2_CIF_OK;
            }
        }
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
#ifdef _WIN32
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
#endif
}

static iox2_cif_status dup_optional_cstr(const char* src, size_t max_strlen, char** out) {
    size_t n = 0;
    char* copy = NULL;
    iox2_cif_status st = IOX2_CIF_OK;
    if (out == NULL) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    st = measure_cstr(src, max_strlen, &n);
    if (st != IOX2_CIF_OK) {
        return st;
    }
    copy = (char*)malloc(n + 1u);
    if (copy == NULL) {
        return IOX2_CIF_ERR_NO_MEMORY;
    }
    if (n > 0) {
        st = copy_bytes_bounded(copy, src, n);
        if (st != IOX2_CIF_OK) {
            free(copy);
            return st;
        }
    }
    copy[n] = 0;
    *out = copy;
    return IOX2_CIF_OK;
}

static iox2_cif_status write_cstr_out(
    char* buf, uint32_t buf_len, uint32_t* out_len, const char* text, uint32_t text_len) {
    iox2_cif_status st = IOX2_CIF_OK;
    uint32_t n = text_len;
    if (text == NULL) {
        text = "";
        text_len = 0;
        n = 0;
    }
    if (out_len != NULL) {
        st = store_u32(out_len, text_len);
        if (st != IOX2_CIF_OK) {
            return st;
        }
    }
    if (buf == NULL || buf_len == 0) {
        return IOX2_CIF_OK;
    }
    if (n >= buf_len) {
        n = buf_len - 1u;
    }
    if (n > 0) {
        st = copy_bytes_bounded(buf, text, n);
        if (st != IOX2_CIF_OK) {
            return st;
        }
    }
    return store_u8((uint8_t*)buf + n, 0);
}

static bool iox2_cif_key_eq(const void* lhs, const void* rhs) {
    uint64_t a = 0;
    uint64_t b = 0;
    memcpy(&a, lhs, sizeof(a));
    memcpy(&b, rhs, sizeof(b));
    return a == b;
}

uint64_t iox2_cif_topic_key(const char* topic) {
    uint64_t hash = IOX2_CIF_FNV1A64_OFFSET;
    const unsigned char* p = NULL;
    if (topic == NULL) {
        return 0;
    }
    for (p = (const unsigned char*)topic; *p != 0; ++p) {
        hash ^= (uint64_t)(*p);
        hash *= IOX2_CIF_FNV1A64_PRIME;
    }
    return hash;
}

iox2_cif_status iox2_cif_status_text(
    iox2_cif_status status, char* buf, uint32_t buf_len, uint32_t* out_len) {
    const char* text = "unknown error";
    size_t n = 0;
    switch (status) {
    case IOX2_CIF_OK:
        text = "ok";
        break;
    case IOX2_CIF_ERR_INVALID_ARGUMENT:
        text = "invalid argument";
        break;
    case IOX2_CIF_ERR_INVALID_HANDLE:
        text = "invalid handle";
        break;
    case IOX2_CIF_ERR_ALREADY_STARTED:
        text = "blackboard already started";
        break;
    case IOX2_CIF_ERR_NOT_STARTED:
        text = "blackboard not started";
        break;
    case IOX2_CIF_ERR_DUPLICATE_TOPIC:
        text = "duplicate topic";
        break;
    case IOX2_CIF_ERR_WRITER_HELD:
        text = "writer still attached";
        break;
    case IOX2_CIF_ERR_SCHEMA_MISMATCH:
        text = "schema mismatch";
        break;
    case IOX2_CIF_ERR_TOPIC_NOT_FOUND:
        text = "topic not found";
        break;
    case IOX2_CIF_ERR_ARRAY_TOO_LARGE:
        text = "array larger than max";
        break;
    case IOX2_CIF_ERR_TIMEOUT:
        text = "timeout";
        break;
    case IOX2_CIF_ERR_TYPE_REJECTED:
        text = "type rejected";
        break;
    case IOX2_CIF_ERR_KEY_RESERVED:
        text = "topic hashes to reserved key 0";
        break;
    case IOX2_CIF_ERR_KEY_COLLISION:
        text = "topic key collision";
        break;
    case IOX2_CIF_ERR_NO_MEMORY:
        text = "out of memory";
        break;
    case IOX2_CIF_ERR_SERVICE_GONE:
        text = "service gone";
        break;
    case IOX2_CIF_ERR_UNSUPPORTED:
        text = "unsupported";
        break;
    case IOX2_CIF_ERR_INTERNAL:
        text = "internal error";
        break;
    case IOX2_CIF_ERR_LOCK_TIMEOUT:
        text = "lock timeout";
        break;
    default:
        break;
    }
    if (status == IOX2_CIF_ERR_INTERNAL && g_last_detail[0] != 0) {
        char composed[256];
        snprintf(composed, sizeof(composed), "internal error: %s", g_last_detail);
        n = strlen(composed);
        if (out_len) {
            *out_len = (uint32_t)n;
        }
        if (buf != NULL && buf_len > 0) {
            if (n >= buf_len) {
                n = buf_len - 1;
            }
            memcpy(buf, composed, n);
            buf[n] = 0;
        }
        return IOX2_CIF_OK;
    }
    n = strlen(text);
    if (out_len) {
        *out_len = (uint32_t)n;
    }
    if (buf != NULL && buf_len > 0) {
        if (n >= buf_len) {
            n = buf_len - 1;
        }
        memcpy(buf, text, n);
        buf[n] = 0;
    }
    return IOX2_CIF_OK;
}

static uint64_t alloc_handle(iox2_cif_kind kind, void* ptr) {
    uint32_t i = 0;
    for (i = 0; i < IOX2_CIF_MAX_SLOTS; ++i) {
        if (g_state.slots[i].kind == IOX2_CIF_KIND_FREE) {
            uint32_t gen = g_state.slots[i].gen + 1u;
            if (gen == 0) {
                gen = 1;
            }
            g_state.slots[i].gen = gen;
            g_state.slots[i].kind = kind;
            g_state.slots[i].ptr = ptr;
            return ((uint64_t)gen << 32) | (uint64_t)(i + 1u);
        }
    }
    return IOX2_CIF_INVALID_HANDLE;
}

static void free_handle(uint64_t handle) {
    uint32_t idx = 0;
    uint32_t gen = 0;
    if (handle == 0) {
        return;
    }
    idx = (uint32_t)(handle & 0xffffffffu);
    gen = (uint32_t)(handle >> 32);
    if (idx == 0 || idx > IOX2_CIF_MAX_SLOTS) {
        return;
    }
    idx -= 1;
    if (g_state.slots[idx].gen == gen) {
        g_state.slots[idx].kind = IOX2_CIF_KIND_FREE;
        g_state.slots[idx].ptr = NULL;
        g_state.slots[idx].gen = gen + 1u;
    }
}

static void* lookup_handle(uint64_t handle, iox2_cif_kind kind) {
    uint32_t idx = 0;
    uint32_t gen = 0;
    if (handle == 0) {
        return NULL;
    }
    idx = (uint32_t)(handle & 0xffffffffu);
    gen = (uint32_t)(handle >> 32);
    if (idx == 0 || idx > IOX2_CIF_MAX_SLOTS) {
        return NULL;
    }
    idx -= 1;
    if (g_state.slots[idx].gen != gen || g_state.slots[idx].kind != kind) {
        return NULL;
    }
    return g_state.slots[idx].ptr;
}

static void compute_value_layout(
    int is_array,
    uint32_t elem_size,
    uint32_t elem_align,
    uint32_t max_count,
    uint32_t* value_size,
    uint32_t* value_align) {
    if (!is_array) {
        *value_size = elem_size;
        *value_align = elem_align == 0 ? 1u : elem_align;
        return;
    }
    *value_align = max_u32(8u, elem_align == 0 ? 1u : elem_align);
    *value_size = align_up_u32(IOX2_CIF_SLOT_HEADER_SIZE + max_count * elem_size, *value_align);
}

static void fill_iox_type_name(char* dst, size_t dst_sz, const char* elem_type, int is_array) {
    if (is_array && !starts_with(elem_type, IOX2_CIF_ARRAY_TYPE_PREFIX)) {
        snprintf(dst, dst_sz, "%s%s", IOX2_CIF_ARRAY_TYPE_PREFIX, elem_type);
    } else {
        snprintf(dst, dst_sz, "%s", elem_type);
    }
}

static void* make_payload(const iox2_cif_writer* w, const void* elems, uint32_t count) {
    uint8_t* buf = (uint8_t*)calloc(1, w->value_size);
    iox2_cif_slot_t header;
    if (buf == NULL) {
        return NULL;
    }
    if (!w->is_array) {
        if (copy_bytes_bounded(buf, elems, w->elem_size) != IOX2_CIF_OK) {
            free(buf);
            return NULL;
        }
        return buf;
    }
    header.count = count;
    header.reserved = 0;
    memcpy(buf, &header, sizeof(header));
    if (count > 0 && elems != NULL) {
        if (copy_bytes_bounded(buf + IOX2_CIF_SLOT_HEADER_SIZE, elems, (size_t)count * w->elem_size)
            != IOX2_CIF_OK) {
            free(buf);
            return NULL;
        }
    }
    return buf;
}

#ifdef _WIN32
static DWORD WINAPI wait_thread_fn(LPVOID param) {
    (void)param;
    while (g_state.running && g_state.node != NULL) {
        iox2_node_wait(&g_state.node, 0, 50000000u);
    }
    return 0;
}
#else
static void* wait_thread_fn(void* param) {
    (void)param;
    while (g_state.running && g_state.node != NULL) {
        iox2_node_wait(&g_state.node, 0, 50000000u);
    }
    return NULL;
}
#endif

static int start_wait_thread(void) {
#ifdef _WIN32
    g_state.wait_thread = CreateThread(NULL, 0, wait_thread_fn, NULL, 0, NULL);
    g_state.wait_started = g_state.wait_thread != NULL;
#else
    g_state.wait_started = pthread_create(&g_state.wait_thread, NULL, wait_thread_fn, NULL) == 0;
#endif
    return g_state.wait_started;
}

static void meta_sweep_orphans(void);

static iox2_cif_status ensure_node_unlocked(const char* node_name) {
    iox2_node_builder_h builder = NULL;
    iox2_node_name_h name_h = NULL;
    iox2_config_h cfg = NULL;
    int rc = 0;

    if (g_state.node != NULL) {
        return IOX2_CIF_OK;
    }

    iox2_set_log_level_from_env_or(iox2_log_level_e_INFO);
    builder = iox2_node_builder_new(NULL);
    if (builder == NULL) {
        set_last_detail("node builder alloc failed");
        return IOX2_CIF_ERR_INTERNAL;
    }
    iox2_node_builder_set_signal_handling_mode(&builder, iox2_signal_handling_mode_e_DISABLED);
    if (iox2_config_default(NULL, &cfg) == IOX2_OK && cfg != NULL) {
        /* Default is 1s; leftover Windows SHM / dead-node cleanup often needs longer. */
        iox2_config_global_set_creation_timeout(&cfg, 10, 0);
        iox2_node_builder_set_config(&builder, &cfg);
    }
    if (node_name != NULL && node_name[0] != 0) {
        rc = iox2_node_name_new(NULL, node_name, strlen(node_name), &name_h);
        if (rc != IOX2_OK) {
            if (cfg != NULL) {
                iox2_config_drop(cfg);
            }
            return IOX2_CIF_ERR_INVALID_ARGUMENT;
        }
        iox2_node_builder_set_name(&builder, iox2_cast_node_name_ptr(name_h));
    }
    rc = iox2_node_builder_create(builder, NULL, iox2_service_type_e_IPC, &g_state.node);
    if (name_h != NULL) {
        iox2_node_name_drop(name_h);
    }
    if (cfg != NULL) {
        iox2_config_drop(cfg);
    }
    dbg_log("node_create rc=%d", rc);
    if (rc != IOX2_OK) {
        g_state.node = NULL;
        set_last_detail(iox2_node_creation_failure_string((enum iox2_node_creation_failure_e)rc));
        return IOX2_CIF_ERR_INTERNAL;
    }
    g_state.running = 1;
    start_wait_thread();
    meta_sweep_orphans();
    return IOX2_CIF_OK;
}

iox2_cif_status iox2_cif_init(const char* node_name) {
    iox2_cif_status st = lock_exclusive();
    if (st != IOX2_CIF_OK) {
        return st;
    }
    st = ensure_node_unlocked(node_name);
    unlock_exclusive();
    return st;
}

static void destroy_reader_unlocked(iox2_cif_reader* r, uint64_t handle);
static void destroy_bb_unlocked(iox2_cif_bb* b, uint32_t* readers_remaining);
static void destroy_ps_writer_unlocked(void* obj);
static void destroy_ps_sub_unlocked(void* obj);
static int ps_writer_exists_unlocked(const char* name, const char* topic);

iox2_cif_status iox2_cif_shutdown(void) {
    uint32_t i = 0;
    iox2_cif_status st = lock_exclusive_ms(IOX2_CIF_SHUTDOWN_LOCK_TIMEOUT_MS);
    if (st != IOX2_CIF_OK) {
        return st;
    }
    g_state.running = 0;
#ifdef _WIN32
    if (g_state.wait_thread != NULL) {
        HANDLE wait_thread = g_state.wait_thread;
        g_state.wait_thread = NULL;
        g_state.wait_started = 0;
        unlock_exclusive();
        WaitForSingleObject(wait_thread, 2000);
        CloseHandle(wait_thread);
        st = lock_exclusive_ms(IOX2_CIF_SHUTDOWN_LOCK_TIMEOUT_MS);
        if (st != IOX2_CIF_OK) {
            return st;
        }
    }
#else
    if (g_state.wait_started) {
        pthread_t wait_thread = g_state.wait_thread;
        struct timespec ts;
        g_state.wait_started = 0;
        unlock_exclusive();
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 2;
        pthread_timedjoin_np(wait_thread, NULL, &ts);
        st = lock_exclusive_ms(IOX2_CIF_SHUTDOWN_LOCK_TIMEOUT_MS);
        if (st != IOX2_CIF_OK) {
            return st;
        }
    }
#endif
    for (i = 0; i < IOX2_CIF_MAX_SLOTS; ++i) {
        if (g_state.slots[i].kind == IOX2_CIF_KIND_PS_SUB
            || g_state.slots[i].kind == IOX2_CIF_KIND_PCAP_SUB) {
            destroy_ps_sub_unlocked(g_state.slots[i].ptr);
        }
    }
    for (i = 0; i < IOX2_CIF_MAX_SLOTS; ++i) {
        if (g_state.slots[i].kind == IOX2_CIF_KIND_PS_WRITER
            || g_state.slots[i].kind == IOX2_CIF_KIND_PCAP_WRITER) {
            destroy_ps_writer_unlocked(g_state.slots[i].ptr);
        }
    }
    for (i = 0; i < IOX2_CIF_MAX_SLOTS; ++i) {
        if (g_state.slots[i].kind == IOX2_CIF_KIND_READER) {
            iox2_cif_reader* r = (iox2_cif_reader*)g_state.slots[i].ptr;
            uint64_t h = r->handle;
            destroy_reader_unlocked(r, h);
        }
    }
    for (i = 0; i < IOX2_CIF_MAX_SLOTS; ++i) {
        if (g_state.slots[i].kind == IOX2_CIF_KIND_BB) {
            destroy_bb_unlocked((iox2_cif_bb*)g_state.slots[i].ptr, NULL);
        }
    }
    if (g_state.node != NULL) {
        iox2_node_drop(g_state.node);
        g_state.node = NULL;
    }
    unlock_exclusive();
    return IOX2_CIF_OK;
}

static iox2_cif_status make_service_name(const char* name, iox2_service_name_h* out) {
    int rc = 0;
    *out = NULL;
    if (name == NULL || name[0] == 0) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    rc = iox2_service_name_new(NULL, name, strlen(name), out);
    if (rc != IOX2_OK) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    return IOX2_CIF_OK;
}

static void apply_key_type_creator(iox2_service_builder_blackboard_creator_h* creator) {
    const char* tn = IOX2_CIF_KEY_TYPE_NAME;
    iox2_service_builder_blackboard_creator_set_key_type_details(
        creator, tn, strlen(tn), sizeof(uint64_t), IOX2_CIF_ALIGN_OF(uint64_t));
    iox2_service_builder_blackboard_creator_set_key_eq_comparison_function(creator, iox2_cif_key_eq);
}

static void apply_key_type_opener(iox2_service_builder_blackboard_opener_h* opener) {
    const char* tn = IOX2_CIF_KEY_TYPE_NAME;
    iox2_service_builder_blackboard_opener_set_key_type_details(
        opener, tn, strlen(tn), sizeof(uint64_t), IOX2_CIF_ALIGN_OF(uint64_t));
    iox2_service_builder_blackboard_opener_set_key_eq_comparison_function(opener, iox2_cif_key_eq);
}

static int try_open_service(
    iox2_service_name_ptr name_ptr, iox2_port_factory_blackboard_h* service) {
    iox2_service_builder_h sb = iox2_node_service_builder(&g_state.node, NULL, name_ptr);
    iox2_service_builder_blackboard_opener_h opener = iox2_service_builder_blackboard_opener(sb);
    apply_key_type_opener(&opener);
    *service = NULL;
    return iox2_service_builder_blackboard_open(opener, NULL, service);
}

static uint32_t directory_bytes(void) {
    return (uint32_t)(sizeof(iox2_cif_dir_header_t) + IOX2_CIF_DIR_MAX_TOPICS * sizeof(iox2_cif_dir_entry_t));
}

static void* build_directory(const iox2_cif_bb* bb) {
    uint32_t bytes = directory_bytes();
    uint8_t* buf = (uint8_t*)calloc(1, bytes);
    iox2_cif_dir_header_t* hdr = NULL;
    iox2_cif_dir_entry_t* entries = NULL;
    iox2_cif_writer* w = NULL;
    uint32_t n = 0;
    if (buf == NULL) {
        return NULL;
    }
    hdr = (iox2_cif_dir_header_t*)buf;
    entries = (iox2_cif_dir_entry_t*)(buf + sizeof(iox2_cif_dir_header_t));
    hdr->abi = IOX2_CIF_ABI_VERSION;
    hdr->entry_capacity = IOX2_CIF_DIR_MAX_TOPICS;
    for (w = bb->writers; w != NULL && n < IOX2_CIF_DIR_MAX_TOPICS; w = w->next) {
        entries[n].key = w->key;
        entries[n].elem_size = w->elem_size;
        entries[n].elem_align = w->elem_align;
        entries[n].max_count = w->max_count;
        entries[n].flags = w->is_array ? 1u : 0u;
        snprintf(entries[n].type_name, sizeof(entries[n].type_name), "%s", w->type_name);
        snprintf(entries[n].topic, sizeof(entries[n].topic), "%s", w->topic);
        n += 1;
    }
    hdr->entry_count = n;
    return buf;
}

static const iox2_cif_dir_entry_t* find_dir_topic(const void* dir, const char* topic, uint64_t key) {
    const iox2_cif_dir_header_t* hdr = (const iox2_cif_dir_header_t*)dir;
    const iox2_cif_dir_entry_t* entries = (const iox2_cif_dir_entry_t*)((const uint8_t*)dir + sizeof(*hdr));
    uint32_t i = 0;
    if (hdr == NULL) {
        return NULL;
    }
    for (i = 0; i < hdr->entry_count && i < hdr->entry_capacity; ++i) {
        if (entries[i].key == key) {
            if (topic == NULL || topic[0] == 0 || strcmp(entries[i].topic, topic) == 0) {
                return &entries[i];
            }
        }
    }
    return NULL;
}

static int type_name_matches_dir(const char* caller, const iox2_cif_dir_entry_t* found) {
    const char* stored = NULL;
    if (caller == NULL || found == NULL) {
        return 0;
    }
    stored = found->type_name;
    if (strcmp(caller, stored) == 0) {
        return 1;
    }
    if ((found->flags & 1u) != 0 && starts_with(stored, IOX2_CIF_ARRAY_TYPE_PREFIX)) {
        if (strcmp(caller, stored + (sizeof(IOX2_CIF_ARRAY_TYPE_PREFIX) - 1u)) == 0) {
            return 1;
        }
    }
    return 0;
}

static void apply_dir_entry(iox2_cif_reader* r, const iox2_cif_dir_entry_t* found) {
    r->elem_size = found->elem_size;
    r->elem_align = found->elem_align == 0 ? 1u : found->elem_align;
    r->max_count = found->max_count;
    r->is_array = (found->flags & 1u) != 0;
    snprintf(r->type_name, sizeof(r->type_name), "%s", found->type_name);
    compute_value_layout(r->is_array, r->elem_size, r->elem_align, r->max_count, &r->value_size, &r->value_align);
}

/* Load the key-0 topic directory blob. Caller frees *out. */
static iox2_cif_status load_directory(iox2_port_factory_blackboard_h* service, void** out) {
    iox2_reader_h tmp_reader = NULL;
    iox2_entry_handle_h dir_entry = NULL;
    void* dir = NULL;
    uint64_t dir_key = IOX2_CIF_RESERVED_KEY;
    int rc = 0;

    if (out == NULL) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    *out = NULL;
    rc = iox2_port_factory_reader_builder_create(
        iox2_port_factory_blackboard_reader_builder(service, NULL), NULL, &tmp_reader);
    if (rc != IOX2_OK) {
        return IOX2_CIF_ERR_INTERNAL;
    }
    rc = iox2_reader_entry(
        &tmp_reader,
        NULL,
        &dir_entry,
        &dir_key,
        IOX2_CIF_DIR_TYPE_NAME,
        strlen(IOX2_CIF_DIR_TYPE_NAME),
        directory_bytes(),
        8);
    if (rc != IOX2_OK) {
        iox2_reader_drop(tmp_reader);
        return IOX2_CIF_ERR_TOPIC_NOT_FOUND;
    }
    dir = calloc(1, directory_bytes());
    if (dir == NULL) {
        iox2_entry_handle_drop(dir_entry);
        iox2_reader_drop(tmp_reader);
        return IOX2_CIF_ERR_NO_MEMORY;
    }
    iox2_entry_handle_get(&dir_entry, dir, directory_bytes(), 8, NULL);
    iox2_entry_handle_drop(dir_entry);
    iox2_reader_drop(tmp_reader);
    *out = dir;
    return IOX2_CIF_OK;
}

/* Load the key-0 topic directory. Returns OK and fills *out when the topic is listed.
 * TOPIC_NOT_FOUND = directory missing or topic not listed. INTERNAL = reader port failed. */
static iox2_cif_status load_dir_entry(
    iox2_port_factory_blackboard_h* service,
    const char* topic,
    uint64_t key,
    iox2_cif_dir_entry_t* out) {
    void* dir = NULL;
    const iox2_cif_dir_entry_t* found = NULL;
    iox2_cif_status st = load_directory(service, &dir);
    if (st != IOX2_CIF_OK) {
        return st;
    }
    found = find_dir_topic(dir, topic, key);
    if (found == NULL) {
        free(dir);
        return IOX2_CIF_ERR_TOPIC_NOT_FOUND;
    }
    *out = *found;
    free(dir);
    return IOX2_CIF_OK;
}

typedef struct iox2_cif_meta_entry {
    char topic[IOX2_CIF_TOPIC_CAP];
    char type_name[IOX2_CIF_TYPE_NAME_CAP];
    char* descriptor;
} iox2_cif_meta_entry;

typedef struct iox2_cif_meta_tab {
    char bb_name[IOX2_CIF_NAME_CAP];
    uint32_t count;
    iox2_cif_meta_entry entries[IOX2_CIF_DIR_MAX_TOPICS];
} iox2_cif_meta_tab;

static void meta_tab_clear(iox2_cif_meta_tab* tab) {
    uint32_t i = 0;
    if (tab == NULL) {
        return;
    }
    for (i = 0; i < tab->count; ++i) {
        free(tab->entries[i].descriptor);
        tab->entries[i].descriptor = NULL;
    }
    tab->count = 0;
    tab->bb_name[0] = 0;
}

static void ensure_meta_dir(void) {
#ifdef _WIN32
    CreateDirectoryA("C:\\Temp", NULL);
    CreateDirectoryA("C:\\Temp\\iox2_cif", NULL);
    CreateDirectoryA(IOX2_CIF_META_ROOT, NULL);
#else
    /* World-writable: LabVIEW runs as lvuser, and a root smoke test must not
     * leave a 0755 directory that lvuser cannot write. mkdir is masked by
     * umask, so chmod afterwards. */
    mkdir("/tmp/iox2_cif", 0777);
    mkdir(IOX2_CIF_META_ROOT, 0777);
    chmod("/tmp/iox2_cif", 0777);
    chmod(IOX2_CIF_META_ROOT, 0777);
#endif
}

static void meta_path(const char* bb_name, char* out, size_t out_sz) {
    uint64_t key = iox2_cif_topic_key(bb_name);
#ifdef _WIN32
    snprintf(out, out_sz, "%s\\%016llx.meta", IOX2_CIF_META_ROOT, (unsigned long long)key);
#else
    snprintf(out, out_sz, "%s/%016llx.meta", IOX2_CIF_META_ROOT, (unsigned long long)key);
#endif
}

static int meta_write_u32(FILE* f, uint32_t v) {
    return fwrite(&v, sizeof(v), 1, f) == 1;
}

static int meta_read_u32(FILE* f, uint32_t* v) {
    return fread(v, sizeof(*v), 1, f) == 1;
}

static int meta_write_blob(FILE* f, const void* p, uint32_t n) {
    return n == 0 || fwrite(p, 1, n, f) == n;
}

static int meta_read_blob(FILE* f, void* p, uint32_t n) {
    return n == 0 || fread(p, 1, n, f) == n;
}

static iox2_cif_status meta_save(const iox2_cif_meta_tab* tab) {
    char path[512];
    char tmp[520];
    FILE* f = NULL;
    uint32_t i = 0;
    uint32_t name_len = 0;
    int ok = 1;

    if (tab == NULL || tab->bb_name[0] == 0) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    ensure_meta_dir();
    meta_path(tab->bb_name, path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    f = fopen(tmp, "wb");
    if (f == NULL) {
#ifndef _WIN32
        {
            char detail[160];
            snprintf(detail, sizeof(detail), "cannot write %s (%s)", tmp, strerror(errno));
            set_last_detail(detail);
        }
#else
        set_last_detail("cannot write meta file");
#endif
        return IOX2_CIF_ERR_INTERNAL;
    }
    name_len = (uint32_t)strlen(tab->bb_name);
    ok = meta_write_u32(f, IOX2_CIF_META_MAGIC) && meta_write_u32(f, IOX2_CIF_META_VERSION)
        && meta_write_u32(f, name_len) && meta_write_blob(f, tab->bb_name, name_len)
        && meta_write_u32(f, tab->count);
    for (i = 0; ok && i < tab->count; ++i) {
        const char* desc = tab->entries[i].descriptor != NULL ? tab->entries[i].descriptor : "";
        uint32_t tlen = (uint32_t)strlen(tab->entries[i].topic);
        uint32_t ylen = (uint32_t)strlen(tab->entries[i].type_name);
        uint32_t dlen = (uint32_t)strlen(desc);
        ok = meta_write_u32(f, tlen) && meta_write_u32(f, ylen) && meta_write_u32(f, dlen)
            && meta_write_blob(f, tab->entries[i].topic, tlen)
            && meta_write_blob(f, tab->entries[i].type_name, ylen)
            && meta_write_blob(f, desc, dlen);
    }
    if (fclose(f) != 0) {
        ok = 0;
    }
    if (!ok) {
#ifdef _WIN32
        DeleteFileA(tmp);
#else
        remove(tmp);
#endif
        return IOX2_CIF_ERR_INTERNAL;
    }
#ifdef _WIN32
    if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileA(tmp);
        return IOX2_CIF_ERR_INTERNAL;
    }
#else
    if (rename(tmp, path) != 0) {
        char detail[160];
        snprintf(detail, sizeof(detail), "cannot replace %s (%s)", path, strerror(errno));
        set_last_detail(detail);
        remove(tmp);
        return IOX2_CIF_ERR_INTERNAL;
    }
    chmod(path, 0666);
#endif
    return IOX2_CIF_OK;
}

static iox2_cif_status meta_load(const char* bb_name, iox2_cif_meta_tab* tab) {
    char path[512];
    FILE* f = NULL;
    uint32_t magic = 0;
    uint32_t version = 0;
    uint32_t name_len = 0;
    uint32_t count = 0;
    uint32_t i = 0;
    char name_buf[IOX2_CIF_NAME_CAP];

    if (tab == NULL) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    memset(tab, 0, sizeof(*tab));
    meta_path(bb_name, path, sizeof(path));
    f = fopen(path, "rb");
    if (f == NULL) {
        return IOX2_CIF_ERR_TOPIC_NOT_FOUND;
    }
    if (!meta_read_u32(f, &magic) || magic != IOX2_CIF_META_MAGIC || !meta_read_u32(f, &version)
        || version != IOX2_CIF_META_VERSION || !meta_read_u32(f, &name_len)
        || name_len == 0 || name_len >= IOX2_CIF_NAME_CAP || !meta_read_blob(f, name_buf, name_len)) {
        fclose(f);
        return IOX2_CIF_ERR_INTERNAL;
    }
    name_buf[name_len] = 0;
    if (strcmp(name_buf, bb_name) != 0 || !meta_read_u32(f, &count) || count > IOX2_CIF_DIR_MAX_TOPICS) {
        fclose(f);
        return IOX2_CIF_ERR_INTERNAL;
    }
    snprintf(tab->bb_name, sizeof(tab->bb_name), "%s", name_buf);
    for (i = 0; i < count; ++i) {
        uint32_t tlen = 0;
        uint32_t ylen = 0;
        uint32_t dlen = 0;
        char* desc = NULL;
        if (!meta_read_u32(f, &tlen) || !meta_read_u32(f, &ylen) || !meta_read_u32(f, &dlen)
            || tlen == 0 || tlen >= IOX2_CIF_TOPIC_CAP || ylen == 0 || ylen >= IOX2_CIF_TYPE_NAME_CAP
            || dlen >= IOX2_CIF_DESC_MAX) {
            meta_tab_clear(tab);
            fclose(f);
            return IOX2_CIF_ERR_INTERNAL;
        }
        if (!meta_read_blob(f, tab->entries[i].topic, tlen)
            || !meta_read_blob(f, tab->entries[i].type_name, ylen)) {
            meta_tab_clear(tab);
            fclose(f);
            return IOX2_CIF_ERR_INTERNAL;
        }
        tab->entries[i].topic[tlen] = 0;
        tab->entries[i].type_name[ylen] = 0;
        desc = (char*)malloc(dlen + 1u);
        if (desc == NULL) {
            meta_tab_clear(tab);
            fclose(f);
            return IOX2_CIF_ERR_NO_MEMORY;
        }
        if (!meta_read_blob(f, desc, dlen)) {
            free(desc);
            meta_tab_clear(tab);
            fclose(f);
            return IOX2_CIF_ERR_INTERNAL;
        }
        desc[dlen] = 0;
        tab->entries[i].descriptor = desc;
        tab->count += 1;
    }
    fclose(f);
    return IOX2_CIF_OK;
}

static iox2_cif_status meta_upsert(
    const char* bb_name, const char* topic, const char* type_name, const char* descriptor) {
    iox2_cif_meta_tab tab;
    iox2_cif_status st = IOX2_CIF_OK;
    uint32_t i = 0;
    const char* desc = descriptor != NULL ? descriptor : "";
    char* copy = NULL;

    memset(&tab, 0, sizeof(tab));
    st = meta_load(bb_name, &tab);
    if (st == IOX2_CIF_ERR_TOPIC_NOT_FOUND) {
        snprintf(tab.bb_name, sizeof(tab.bb_name), "%s", bb_name);
        st = IOX2_CIF_OK;
    }
    if (st != IOX2_CIF_OK) {
        return st;
    }
    copy = (char*)malloc(strlen(desc) + 1u);
    if (copy == NULL) {
        meta_tab_clear(&tab);
        return IOX2_CIF_ERR_NO_MEMORY;
    }
    memcpy(copy, desc, strlen(desc) + 1u);
    for (i = 0; i < tab.count; ++i) {
        if (strcmp(tab.entries[i].topic, topic) == 0) {
            free(tab.entries[i].descriptor);
            snprintf(tab.entries[i].type_name, sizeof(tab.entries[i].type_name), "%s", type_name);
            tab.entries[i].descriptor = copy;
            st = meta_save(&tab);
            meta_tab_clear(&tab);
            return st;
        }
    }
    if (tab.count >= IOX2_CIF_DIR_MAX_TOPICS) {
        free(copy);
        meta_tab_clear(&tab);
        return IOX2_CIF_ERR_NO_MEMORY;
    }
    snprintf(tab.entries[tab.count].topic, sizeof(tab.entries[tab.count].topic), "%s", topic);
    snprintf(tab.entries[tab.count].type_name, sizeof(tab.entries[tab.count].type_name), "%s", type_name);
    tab.entries[tab.count].descriptor = copy;
    tab.count += 1;
    st = meta_save(&tab);
    meta_tab_clear(&tab);
    return st;
}

static iox2_cif_status meta_remove_topic(const char* bb_name, const char* topic) {
    iox2_cif_meta_tab tab;
    iox2_cif_status st = IOX2_CIF_OK;
    uint32_t i = 0;
    uint32_t dst = 0;

    memset(&tab, 0, sizeof(tab));
    st = meta_load(bb_name, &tab);
    if (st != IOX2_CIF_OK) {
        return st == IOX2_CIF_ERR_TOPIC_NOT_FOUND ? IOX2_CIF_OK : st;
    }
    for (i = 0; i < tab.count; ++i) {
        if (strcmp(tab.entries[i].topic, topic) == 0) {
            free(tab.entries[i].descriptor);
            tab.entries[i].descriptor = NULL;
            continue;
        }
        if (dst != i) {
            tab.entries[dst] = tab.entries[i];
            tab.entries[i].descriptor = NULL;
        }
        dst += 1;
    }
    tab.count = dst;
    if (tab.count == 0) {
        char path[512];
        meta_path(bb_name, path, sizeof(path));
#ifdef _WIN32
        DeleteFileA(path);
#else
        remove(path);
#endif
        meta_tab_clear(&tab);
        return IOX2_CIF_OK;
    }
    st = meta_save(&tab);
    meta_tab_clear(&tab);
    return st;
}

static iox2_cif_status meta_lookup(
    const char* bb_name,
    const char* topic,
    char* type_name,
    uint32_t type_cap,
    uint32_t* type_len,
    char* descriptor,
    uint32_t desc_cap,
    uint32_t* desc_len) {
    iox2_cif_meta_tab tab;
    iox2_cif_status st = IOX2_CIF_OK;
    uint32_t i = 0;

    memset(&tab, 0, sizeof(tab));
    st = meta_load(bb_name, &tab);
    if (st != IOX2_CIF_OK) {
        return st;
    }
    for (i = 0; i < tab.count; ++i) {
        if (strcmp(tab.entries[i].topic, topic) == 0) {
            const char* desc = tab.entries[i].descriptor != NULL ? tab.entries[i].descriptor : "";
            uint32_t type_n = (uint32_t)strlen(tab.entries[i].type_name);
            uint32_t desc_n = (uint32_t)strlen(desc);
            st = write_cstr_out(type_name, type_cap, type_len, tab.entries[i].type_name, type_n);
            if (st == IOX2_CIF_OK) {
                st = write_cstr_out(descriptor, desc_cap, desc_len, desc, desc_n);
            }
            meta_tab_clear(&tab);
            return st;
        }
    }
    meta_tab_clear(&tab);
    return IOX2_CIF_ERR_TOPIC_NOT_FOUND;
}

static iox2_cif_writer* find_local_writer(const char* bb_name, const char* topic) {
    uint32_t i = 0;
    for (i = 0; i < IOX2_CIF_MAX_SLOTS; ++i) {
        iox2_cif_bb* b = NULL;
        iox2_cif_writer* w = NULL;
        if (g_state.slots[i].kind != IOX2_CIF_KIND_BB) {
            continue;
        }
        b = (iox2_cif_bb*)g_state.slots[i].ptr;
        if (b == NULL || strcmp(b->name, bb_name) != 0) {
            continue;
        }
        for (w = b->writers; w != NULL; w = w->next) {
            if (strcmp(w->topic, topic) == 0) {
                return w;
            }
        }
    }
    return NULL;
}

iox2_cif_status iox2_cif_bb_create(const char* name, uint32_t max_readers, uint64_t* bb) {
    iox2_cif_bb* obj = NULL;
    char name_copy[IOX2_CIF_NAME_CAP];
    uint64_t handle = 0;
    iox2_cif_status st = IOX2_CIF_OK;

    dbg_log("bb_create name_ptr=%p max_readers=%u bb_out=%p", (const void*)name, max_readers, (void*)bb);
    if (bb == NULL) {
        set_last_detail("bb out-handle is NULL (Pass Pointer to Value)");
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    if (max_readers < 1) {
        set_last_detail("max_readers < 1 (U32 Pass Value, e.g. 8)");
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    if (max_readers > IOX2_CIF_MAX_READERS) {
        dbg_log("clamping max_readers %u -> %u (CLFN likely Pass Pointer instead of Value)",
            max_readers, IOX2_CIF_MAX_READERS);
        max_readers = IOX2_CIF_MAX_READERS;
    }
    st = copy_cstr_bounded(name_copy, sizeof(name_copy), name);
    if (st != IOX2_CIF_OK || name_copy[0] == 0) {
        set_last_detail("blackboard name empty or not a C string pointer");
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    dbg_log("bb_create name='%s' max_readers=%u", name_copy, max_readers);

    obj = (iox2_cif_bb*)calloc(1, sizeof(*obj));
    if (obj == NULL) {
        return IOX2_CIF_ERR_NO_MEMORY;
    }
    memcpy(obj->name, name_copy, sizeof(name_copy));
    obj->max_readers = max_readers;

    st = lock_exclusive();
    if (st != IOX2_CIF_OK) {
        free(obj);
        return st;
    }
    obj->handle = alloc_handle(IOX2_CIF_KIND_BB, obj);
    handle = obj->handle;
    if (handle == IOX2_CIF_INVALID_HANDLE) {
        unlock_exclusive();
        free(obj);
        return IOX2_CIF_ERR_NO_MEMORY;
    }
    unlock_exclusive();

    st = store_u64(bb, handle);
    if (st != IOX2_CIF_OK) {
        iox2_cif_status lk = lock_exclusive();
        if (lk == IOX2_CIF_OK) {
            free_handle(handle);
            unlock_exclusive();
        }
        free(obj);
        return st;
    }
    return IOX2_CIF_OK;
}

iox2_cif_status iox2_cif_bb_writer_create(
    uint64_t bb,
    const char* topic,
    const char* type_name,
    uint32_t elem_size,
    uint32_t elem_align,
    uint32_t max_count,
    const void* initial,
    uint32_t initial_count,
    const char* descriptor,
    uint64_t* writer) {
    iox2_cif_bb* b = NULL;
    iox2_cif_writer* w = NULL;
    iox2_cif_writer* it = NULL;
    uint64_t key = 0;
    uint64_t handle = 0;
    int is_array = 0;
    size_t initial_bytes = 0;
    char topic_copy[IOX2_CIF_TOPIC_CAP];
    char type_copy[IOX2_CIF_TYPE_NAME_CAP];
    void* initial_copy = NULL;
    char* desc_copy = NULL;
    iox2_cif_status st = IOX2_CIF_OK;

    if (writer == NULL || elem_size == 0 || initial == NULL) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    st = copy_cstr_bounded(topic_copy, sizeof(topic_copy), topic);
    if (st != IOX2_CIF_OK || topic_copy[0] == 0) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    st = copy_cstr_bounded(type_copy, sizeof(type_copy), type_name);
    if (st != IOX2_CIF_OK || type_copy[0] == 0) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    st = dup_optional_cstr(descriptor, IOX2_CIF_DESC_MAX - 1u, &desc_copy);
    if (st != IOX2_CIF_OK) {
        return st;
    }

    is_array = starts_with(type_copy, IOX2_CIF_ARRAY_TYPE_PREFIX) || max_count > 1u;
    if (!is_array) {
        max_count = 1;
        initial_count = 1;
    } else if (max_count < 1u || initial_count > max_count) {
        free(desc_copy);
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    if (elem_align == 0) {
        elem_align = 1;
    }
    key = iox2_cif_topic_key(topic_copy);
    if (key == IOX2_CIF_RESERVED_KEY) {
        free(desc_copy);
        return IOX2_CIF_ERR_KEY_RESERVED;
    }

    initial_bytes = is_array ? (size_t)initial_count * (size_t)elem_size : (size_t)elem_size;
    initial_copy = malloc(initial_bytes == 0 ? 1 : initial_bytes);
    if (initial_copy == NULL) {
        free(desc_copy);
        return IOX2_CIF_ERR_NO_MEMORY;
    }
    st = copy_bytes_bounded(initial_copy, initial, initial_bytes);
    if (st != IOX2_CIF_OK) {
        free(initial_copy);
        free(desc_copy);
        return st;
    }

    w = (iox2_cif_writer*)calloc(1, sizeof(*w));
    if (w == NULL) {
        free(initial_copy);
        free(desc_copy);
        return IOX2_CIF_ERR_NO_MEMORY;
    }
    memcpy(w->topic, topic_copy, sizeof(topic_copy));
    fill_iox_type_name(w->type_name, sizeof(w->type_name), type_copy, is_array);
    w->key = key;
    w->elem_size = elem_size;
    w->elem_align = elem_align;
    w->max_count = max_count;
    w->is_array = is_array;
    w->valid = 1;
    w->initial_count = initial_count;
    w->initial = initial_copy;
    w->descriptor = desc_copy;
    compute_value_layout(is_array, elem_size, elem_align, max_count, &w->value_size, &w->value_align);

    st = lock_exclusive();
    if (st != IOX2_CIF_OK) {
        free(w->initial);
        free(w->descriptor);
        free(w);
        return st;
    }
    b = (iox2_cif_bb*)lookup_handle(bb, IOX2_CIF_KIND_BB);
    if (b == NULL) {
        unlock_exclusive();
        free(w->initial);
        free(w->descriptor);
        free(w);
        return IOX2_CIF_ERR_INVALID_HANDLE;
    }
    if (b->started) {
        unlock_exclusive();
        free(w->initial);
        free(w->descriptor);
        free(w);
        return IOX2_CIF_ERR_ALREADY_STARTED;
    }
    if (ps_writer_exists_unlocked(b->name, w->topic)) {
        unlock_exclusive();
        free(w->initial);
        free(w->descriptor);
        free(w);
        return IOX2_CIF_ERR_SCHEMA_MISMATCH;
    }
    for (it = b->writers; it != NULL; it = it->next) {
        if (strcmp(it->topic, w->topic) == 0) {
            unlock_exclusive();
            free(w->initial);
            free(w->descriptor);
            free(w);
            return IOX2_CIF_ERR_DUPLICATE_TOPIC;
        }
        if (it->key == w->key) {
            unlock_exclusive();
            free(w->initial);
            free(w->descriptor);
            free(w);
            return IOX2_CIF_ERR_KEY_COLLISION;
        }
    }
    w->bb = b;
    w->handle = alloc_handle(IOX2_CIF_KIND_WRITER, w);
    handle = w->handle;
    if (handle == IOX2_CIF_INVALID_HANDLE) {
        unlock_exclusive();
        free(w->initial);
        free(w->descriptor);
        free(w);
        return IOX2_CIF_ERR_NO_MEMORY;
    }
    w->next = b->writers;
    b->writers = w;
    st = meta_upsert(b->name, w->topic, w->type_name, w->descriptor);
    if (st == IOX2_CIF_OK && b->staged_count < IOX2_CIF_DIR_MAX_TOPICS) {
        memcpy(b->staged_topics[b->staged_count], w->topic, IOX2_CIF_TOPIC_CAP);
        b->staged_count += 1u;
    }
    unlock_exclusive();
    if (st != IOX2_CIF_OK) {
        iox2_cif_bb_destroy_writer(handle);
        return st;
    }

    st = store_u64(writer, handle);
    if (st != IOX2_CIF_OK) {
        iox2_cif_bb_destroy_writer(handle);
        return st;
    }
    return IOX2_CIF_OK;
}

static void drop_writer_entry(iox2_cif_writer* w) {
    if (w->entry != NULL) {
        iox2_entry_handle_mut_drop(w->entry);
        w->entry = NULL;
    }
}

static void drop_bb_ports(iox2_cif_bb* b) {
    iox2_cif_writer* w = NULL;
    for (w = b->writers; w != NULL; w = w->next) {
        drop_writer_entry(w);
        w->valid = 0;
    }
    if (b->writer != NULL) {
        iox2_writer_drop(b->writer);
        b->writer = NULL;
    }
    if (b->service != NULL) {
        iox2_port_factory_blackboard_drop(b->service);
        b->service = NULL;
    }
    if (b->service_name != NULL) {
        iox2_service_name_drop(b->service_name);
        b->service_name = NULL;
    }
}

static int bind_writer_entries(iox2_cif_bb* b) {
    iox2_cif_writer* w = NULL;
    for (w = b->writers; w != NULL; w = w->next) {
        int rc = iox2_writer_entry(
            &b->writer,
            NULL,
            &w->entry,
            &w->key,
            w->type_name,
            strlen(w->type_name),
            w->value_size,
            w->value_align);
        if (rc != IOX2_OK) {
            return rc;
        }
    }
    return IOX2_OK;
}

static iox2_cif_status start_open(iox2_cif_bb* b, iox2_port_factory_blackboard_h service);

static iox2_cif_status start_create(iox2_cif_bb* b) {
    iox2_service_builder_h sb = NULL;
    iox2_service_builder_blackboard_creator_h creator = NULL;
    iox2_cif_writer* w = NULL;
    void* dir = NULL;
    uint64_t dir_key = IOX2_CIF_RESERVED_KEY;
    int rc = 0;
    const char* dir_tn = IOX2_CIF_DIR_TYPE_NAME;

    if (b->writers == NULL) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    dir = build_directory(b);
    if (dir == NULL) {
        return IOX2_CIF_ERR_NO_MEMORY;
    }

    sb = iox2_node_service_builder(&g_state.node, NULL, iox2_cast_service_name_ptr(b->service_name));
    creator = iox2_service_builder_blackboard_creator(sb);
    apply_key_type_creator(&creator);
    {
        uint32_t max_readers = b->max_readers;
        if (max_readers > IOX2_CIF_MAX_READERS) {
            max_readers = IOX2_CIF_MAX_READERS;
        }
        iox2_service_builder_blackboard_creator_set_max_readers(&creator, (size_t)max_readers);
    }
    dbg_log("start_create name='%s' max_readers=%u writers=%s",
        b->name,
        b->max_readers,
        b->writers ? b->writers->topic : "(none)");
    for (w = b->writers; w != NULL; w = w->next) {
        dbg_log("  writer topic='%s' type='%s' elem=%u/%u max=%u value=%u/%u",
            w->topic,
            w->type_name,
            w->elem_size,
            w->elem_align,
            w->max_count,
            w->value_size,
            w->value_align);
    }

    iox2_service_builder_blackboard_creator_add(
        &creator,
        &dir_key,
        dir,
        NULL,
        dir_tn,
        strlen(dir_tn),
        directory_bytes(),
        8);

    for (w = b->writers; w != NULL; w = w->next) {
        w->create_payload = make_payload(w, w->initial, w->initial_count);
        if (w->create_payload == NULL) {
            iox2_cif_writer* x = NULL;
            free(dir);
            for (x = b->writers; x != NULL; x = x->next) {
                free(x->create_payload);
                x->create_payload = NULL;
            }
            return IOX2_CIF_ERR_NO_MEMORY;
        }
        iox2_service_builder_blackboard_creator_add(
            &creator,
            &w->key,
            w->create_payload,
            NULL,
            w->type_name,
            strlen(w->type_name),
            w->value_size,
            w->value_align);
    }

    rc = iox2_service_builder_blackboard_create(creator, NULL, &b->service);
    dbg_log("blackboard_create rc=%d (%s)",
        rc,
        rc == IOX2_OK ? "ok" : iox2_blackboard_create_error_string((enum iox2_blackboard_create_error_e)rc));
    free(dir);
    for (w = b->writers; w != NULL; w = w->next) {
        free(w->create_payload);
        w->create_payload = NULL;
    }
    if (rc != IOX2_OK) {
        b->service = NULL;
        set_last_detail(iox2_blackboard_create_error_string((enum iox2_blackboard_create_error_e)rc));
        if (rc == (int)iox2_blackboard_create_error_e_C_ALREADY_EXISTS) {
            iox2_port_factory_blackboard_h existing = NULL;
            int open_rc = try_open_service(iox2_cast_service_name_ptr(b->service_name), &existing);
            if (open_rc == IOX2_OK) {
                return start_open(b, existing);
            }
            set_last_detail(iox2_blackboard_open_error_string((enum iox2_blackboard_open_error_e)open_rc));
        }
        return IOX2_CIF_ERR_INTERNAL;
    }

    rc = iox2_port_factory_writer_builder_create(
        iox2_port_factory_blackboard_writer_builder(&b->service, NULL), NULL, &b->writer);
    if (rc != IOX2_OK) {
        drop_bb_ports(b);
        if (rc == (int)iox2_writer_create_error_e_EXCEEDS_MAX_SUPPORTED_WRITERS) {
            return IOX2_CIF_ERR_WRITER_HELD;
        }
        set_last_detail("writer create failed");
        return IOX2_CIF_ERR_INTERNAL;
    }
    rc = bind_writer_entries(b);
    if (rc != IOX2_OK) {
        drop_bb_ports(b);
        set_last_detail("bind writer entries failed");
        return IOX2_CIF_ERR_INTERNAL;
    }
    b->started = 1;
    return IOX2_CIF_OK;
}

static iox2_cif_status start_open(iox2_cif_bb* b, iox2_port_factory_blackboard_h service) {
    int rc = 0;
    b->service = service;
    rc = iox2_port_factory_writer_builder_create(
        iox2_port_factory_blackboard_writer_builder(&b->service, NULL), NULL, &b->writer);
    if (rc != IOX2_OK) {
        iox2_port_factory_blackboard_drop(b->service);
        b->service = NULL;
        return rc == (int)iox2_writer_create_error_e_EXCEEDS_MAX_SUPPORTED_WRITERS
            ? IOX2_CIF_ERR_WRITER_HELD
            : IOX2_CIF_ERR_INTERNAL;
    }
    rc = bind_writer_entries(b);
    if (rc != IOX2_OK) {
        drop_bb_ports(b);
        if (rc == (int)iox2_entry_handle_mut_error_e_ENTRY_DOES_NOT_EXIST) {
            return IOX2_CIF_ERR_SCHEMA_MISMATCH;
        }
        return IOX2_CIF_ERR_SCHEMA_MISMATCH;
    }
    b->started = 1;
    return IOX2_CIF_OK;
}

iox2_cif_status iox2_cif_bb_start(uint64_t bb) {
    iox2_cif_bb* b = NULL;
    iox2_cif_status st = IOX2_CIF_OK;
    iox2_port_factory_blackboard_h service = NULL;
    int rc = 0;

    st = lock_exclusive();
    if (st != IOX2_CIF_OK) {
        return st;
    }
    b = (iox2_cif_bb*)lookup_handle(bb, IOX2_CIF_KIND_BB);
    if (b == NULL) {
        unlock_exclusive();
        return IOX2_CIF_ERR_INVALID_HANDLE;
    }
    if (b->started) {
        unlock_exclusive();
        return IOX2_CIF_ERR_ALREADY_STARTED;
    }
    st = ensure_node_unlocked(NULL);
    if (st != IOX2_CIF_OK) {
        unlock_exclusive();
        return st;
    }
    if (b->service_name != NULL) {
        iox2_service_name_drop(b->service_name);
        b->service_name = NULL;
    }
    st = make_service_name(b->name, &b->service_name);
    if (st != IOX2_CIF_OK) {
        unlock_exclusive();
        return st;
    }
    rc = try_open_service(iox2_cast_service_name_ptr(b->service_name), &service);
    dbg_log("start open name='%s' rc=%d", b->name, rc);
    if (rc == IOX2_OK) {
        st = start_open(b, service);
        unlock_exclusive();
        return st;
    }
    if (rc == (int)iox2_blackboard_open_error_e_O_DOES_NOT_EXIST) {
        st = start_create(b);
        unlock_exclusive();
        return st;
    }
    set_last_detail(iox2_blackboard_open_error_string((enum iox2_blackboard_open_error_e)rc));
    iox2_service_name_drop(b->service_name);
    b->service_name = NULL;
    unlock_exclusive();
    return IOX2_CIF_ERR_INTERNAL;
}

static iox2_cif_status open_reader_service(
    const char* bb_name,
    uint32_t timeout_ms,
    iox2_service_name_h* name_h,
    iox2_port_factory_blackboard_h* service) {
    iox2_cif_status st = make_service_name(bb_name, name_h);
#ifdef _WIN32
    ULONGLONG start = GetTickCount64();
#else
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
#endif
    int rc = 0;
    if (st != IOX2_CIF_OK) {
        return st;
    }
    for (;;) {
        rc = try_open_service(iox2_cast_service_name_ptr(*name_h), service);
        if (rc == IOX2_OK) {
            return IOX2_CIF_OK;
        }
        if (rc != (int)iox2_blackboard_open_error_e_O_DOES_NOT_EXIST) {
            iox2_service_name_drop(*name_h);
            *name_h = NULL;
            return IOX2_CIF_ERR_INTERNAL;
        }
#ifdef _WIN32
        if (timeout_ms == 0 || (GetTickCount64() - start) >= timeout_ms) {
            iox2_service_name_drop(*name_h);
            *name_h = NULL;
            return IOX2_CIF_ERR_TIMEOUT;
        }
        Sleep(10);
#else
        {
            struct timespec now;
            struct timespec delay;
            int64_t elapsed_ms = 0;
            clock_gettime(CLOCK_MONOTONIC, &now);
            elapsed_ms = (int64_t)(now.tv_sec - start.tv_sec) * 1000;
            elapsed_ms += (now.tv_nsec - start.tv_nsec) / 1000000;
            if (timeout_ms == 0 || elapsed_ms >= (int64_t)timeout_ms) {
                iox2_service_name_drop(*name_h);
                *name_h = NULL;
                return IOX2_CIF_ERR_TIMEOUT;
            }
            delay.tv_sec = 0;
            delay.tv_nsec = 10 * 1000000L;
            nanosleep(&delay, NULL);
        }
#endif
    }
}

typedef enum iox2_cif_scalar {
    IOX2_CIF_SCALAR_NONE = 0,
    IOX2_CIF_SCALAR_BOOL,
    IOX2_CIF_SCALAR_I8,
    IOX2_CIF_SCALAR_U8,
    IOX2_CIF_SCALAR_I16,
    IOX2_CIF_SCALAR_U16,
    IOX2_CIF_SCALAR_I32,
    IOX2_CIF_SCALAR_U32,
    IOX2_CIF_SCALAR_I64,
    IOX2_CIF_SCALAR_U64,
    IOX2_CIF_SCALAR_F32,
    IOX2_CIF_SCALAR_F64
} iox2_cif_scalar;

#define IOX2_CIF_MAX_FIELDS 64

typedef struct iox2_cif_field_desc {
    char name[128];
    iox2_cif_scalar type;
    uint32_t size;
    uint32_t offset;
    int type_ok;
} iox2_cif_field_desc;

typedef struct iox2_cif_idl {
    int is_array;
    int big_endian;
    int field_count;
    iox2_cif_field_desc fields[IOX2_CIF_MAX_FIELDS];
} iox2_cif_idl;

static iox2_cif_status finish_reader(iox2_cif_reader* r, const char* type_name);

static iox2_cif_scalar scalar_from_name(const char* name) {
    if (name == NULL) {
        return IOX2_CIF_SCALAR_NONE;
    }
    if (strcmp(name, "bool") == 0) {
        return IOX2_CIF_SCALAR_BOOL;
    }
    if (strcmp(name, "i8") == 0) {
        return IOX2_CIF_SCALAR_I8;
    }
    if (strcmp(name, "u8") == 0) {
        return IOX2_CIF_SCALAR_U8;
    }
    if (strcmp(name, "i16") == 0) {
        return IOX2_CIF_SCALAR_I16;
    }
    if (strcmp(name, "u16") == 0) {
        return IOX2_CIF_SCALAR_U16;
    }
    if (strcmp(name, "i32") == 0) {
        return IOX2_CIF_SCALAR_I32;
    }
    if (strcmp(name, "u32") == 0) {
        return IOX2_CIF_SCALAR_U32;
    }
    if (strcmp(name, "i64") == 0) {
        return IOX2_CIF_SCALAR_I64;
    }
    if (strcmp(name, "u64") == 0) {
        return IOX2_CIF_SCALAR_U64;
    }
    if (strcmp(name, "f32") == 0) {
        return IOX2_CIF_SCALAR_F32;
    }
    if (strcmp(name, "f64") == 0) {
        return IOX2_CIF_SCALAR_F64;
    }
    return IOX2_CIF_SCALAR_NONE;
}

static uint32_t scalar_size(iox2_cif_scalar kind) {
    switch (kind) {
    case IOX2_CIF_SCALAR_BOOL:
    case IOX2_CIF_SCALAR_I8:
    case IOX2_CIF_SCALAR_U8:
        return 1u;
    case IOX2_CIF_SCALAR_I16:
    case IOX2_CIF_SCALAR_U16:
        return 2u;
    case IOX2_CIF_SCALAR_I32:
    case IOX2_CIF_SCALAR_U32:
    case IOX2_CIF_SCALAR_F32:
        return 4u;
    case IOX2_CIF_SCALAR_I64:
    case IOX2_CIF_SCALAR_U64:
    case IOX2_CIF_SCALAR_F64:
        return 8u;
    default:
        return 0u;
    }
}

static int scalar_is_signed(iox2_cif_scalar kind) {
    return kind == IOX2_CIF_SCALAR_I8 || kind == IOX2_CIF_SCALAR_I16 || kind == IOX2_CIF_SCALAR_I32
        || kind == IOX2_CIF_SCALAR_I64;
}

static int scalar_is_unsigned(iox2_cif_scalar kind) {
    return kind == IOX2_CIF_SCALAR_BOOL || kind == IOX2_CIF_SCALAR_U8 || kind == IOX2_CIF_SCALAR_U16
        || kind == IOX2_CIF_SCALAR_U32 || kind == IOX2_CIF_SCALAR_U64;
}

static int64_t scalar_signed_min(iox2_cif_scalar kind) {
    switch (kind) {
    case IOX2_CIF_SCALAR_I8:
        return INT8_MIN;
    case IOX2_CIF_SCALAR_I16:
        return INT16_MIN;
    case IOX2_CIF_SCALAR_I32:
        return INT32_MIN;
    case IOX2_CIF_SCALAR_I64:
        return INT64_MIN;
    default:
        return 0;
    }
}

static int64_t scalar_signed_max(iox2_cif_scalar kind) {
    switch (kind) {
    case IOX2_CIF_SCALAR_I8:
        return INT8_MAX;
    case IOX2_CIF_SCALAR_I16:
        return INT16_MAX;
    case IOX2_CIF_SCALAR_I32:
        return INT32_MAX;
    case IOX2_CIF_SCALAR_I64:
        return INT64_MAX;
    default:
        return 0;
    }
}

static uint64_t scalar_unsigned_max(iox2_cif_scalar kind) {
    switch (kind) {
    case IOX2_CIF_SCALAR_BOOL:
        return 1u;
    case IOX2_CIF_SCALAR_U8:
        return UINT8_MAX;
    case IOX2_CIF_SCALAR_U16:
        return UINT16_MAX;
    case IOX2_CIF_SCALAR_U32:
        return UINT32_MAX;
    case IOX2_CIF_SCALAR_U64:
        return UINT64_MAX;
    default:
        return 0u;
    }
}

static void store_int_bytes(iox2_cif_scalar kind, int64_t value, void* dst) {
    uint32_t n = scalar_size(kind);
    uint64_t bits = (uint64_t)value;
    memcpy(dst, &bits, n);
}

static void store_uint_bytes(iox2_cif_scalar kind, uint64_t value, void* dst) {
    uint32_t n = scalar_size(kind);
    memcpy(dst, &value, n);
}

static int64_t load_signed(iox2_cif_scalar kind, const void* src) {
    uint64_t bits = 0;
    uint32_t n = scalar_size(kind);
    memcpy(&bits, src, n);
    if (n < 8u) {
        uint64_t sign = 1ull << ((n * 8u) - 1u);
        if ((bits & sign) != 0) {
            bits |= ~((sign << 1) - 1ull);
        }
    }
    return (int64_t)bits;
}

static uint64_t load_unsigned(iox2_cif_scalar kind, const void* src) {
    uint64_t bits = 0;
    memcpy(&bits, src, scalar_size(kind));
    if (kind == IOX2_CIF_SCALAR_BOOL) {
        return bits == 0 ? 0u : 1u;
    }
    return bits;
}

static double round_ties_even(double x) {
    double ip = 0.0;
    double frac = 0.0;
    double ax = fabs(x);
    frac = modf(ax, &ip);
    if (frac > 0.5) {
        ip += 1.0;
    } else if (frac == 0.5 && ip < 9223372036854775808.0) {
        int64_t n = (int64_t)ip;
        if ((n & 1) != 0) {
            ip += 1.0;
        }
    }
    return x < 0.0 ? -ip : ip;
}

static float round_ties_even_f(float x) {
    float ip = 0.0f;
    float frac = 0.0f;
    float ax = fabsf(x);
    frac = modff(ax, &ip);
    if (frac > 0.5f) {
        ip += 1.0f;
    } else if (frac == 0.5f && ip < 16777216.0f) {
        int32_t n = (int32_t)ip;
        if ((n & 1) != 0) {
            ip += 1.0f;
        }
    }
    return x < 0.0f ? -ip : ip;
}

static int64_t sat_i64_from_double(double x, int64_t lo, int64_t hi) {
    double rounded = 0.0;
    if (isnan(x)) {
        return 0;
    }
    if (isinf(x)) {
        return x > 0.0 ? hi : lo;
    }
    rounded = round_ties_even(x);
    if (rounded >= 9223372036854775808.0) {
        return hi;
    }
    if (rounded < -9223372036854775808.0) {
        return lo;
    }
    if ((int64_t)rounded > hi) {
        return hi;
    }
    if ((int64_t)rounded < lo) {
        return lo;
    }
    return (int64_t)rounded;
}

static int64_t sat_i64_from_float(float x, int64_t lo, int64_t hi) {
    float rounded = 0.0f;
    if (isnan(x)) {
        return 0;
    }
    if (isinf(x)) {
        return x > 0.0f ? hi : lo;
    }
    rounded = round_ties_even_f(x);
    if (rounded >= 9223372036854775808.0f) {
        return hi;
    }
    if (rounded < -9223372036854775808.0f) {
        return lo;
    }
    if ((int64_t)rounded > hi) {
        return hi;
    }
    if ((int64_t)rounded < lo) {
        return lo;
    }
    return (int64_t)rounded;
}

static uint64_t sat_u64_from_double(double x, uint64_t hi) {
    double rounded = 0.0;
    if (isnan(x) || x <= 0.0) {
        return 0u;
    }
    if (isinf(x)) {
        return hi;
    }
    rounded = round_ties_even(x);
    if (rounded >= 18446744073709551616.0) {
        return hi;
    }
    if ((uint64_t)rounded > hi) {
        return hi;
    }
    return (uint64_t)rounded;
}

static uint64_t sat_u64_from_float(float x, uint64_t hi) {
    float rounded = 0.0f;
    if (isnan(x) || x <= 0.0f) {
        return 0u;
    }
    if (isinf(x)) {
        return hi;
    }
    rounded = round_ties_even_f(x);
    if (rounded >= 18446744073709551616.0f) {
        return hi;
    }
    if ((uint64_t)rounded > hi) {
        return hi;
    }
    return (uint64_t)rounded;
}

static int bool_from_signed(int64_t v) {
    return v != 0;
}

static int bool_from_unsigned(uint64_t v) {
    return v != 0u;
}

static int bool_from_double(double x) {
    if (isnan(x) || x == 0.0) {
        return 0;
    }
    return 1;
}

static int bool_from_float(float x) {
    if (isnan(x) || x == 0.0f) {
        return 0;
    }
    return 1;
}

static void store_f32_sat_from_double(double x, void* dst) {
    float f = 0.0f;
    uint32_t nan_bits = 0x7fc00000u;
    if (isnan(x)) {
        memcpy(dst, &nan_bits, sizeof(nan_bits));
        return;
    }
    if (x > (double)FLT_MAX) {
        f = FLT_MAX;
    } else if (x < -(double)FLT_MAX) {
        f = -FLT_MAX;
    } else {
        f = (float)x;
    }
    memcpy(dst, &f, sizeof(f));
}

static void convert_scalar(iox2_cif_scalar src_kind, const void* src, iox2_cif_scalar dst_kind, void* dst) {
    int64_t s = 0;
    uint64_t u = 0;
    float f = 0.0f;
    double d = 0.0;
    int src_float32 = src_kind == IOX2_CIF_SCALAR_F32;
    int src_float64 = src_kind == IOX2_CIF_SCALAR_F64;
    int dst_bool = dst_kind == IOX2_CIF_SCALAR_BOOL;

    if (src_kind == dst_kind) {
        memcpy(dst, src, scalar_size(dst_kind));
        return;
    }
    if (scalar_is_signed(src_kind)) {
        s = load_signed(src_kind, src);
    } else if (scalar_is_unsigned(src_kind)) {
        u = load_unsigned(src_kind, src);
    } else if (src_float32) {
        memcpy(&f, src, sizeof(f));
    } else if (src_float64) {
        memcpy(&d, src, sizeof(d));
    }

    if (dst_bool) {
        int b = 0;
        if (scalar_is_signed(src_kind)) {
            b = bool_from_signed(s);
        } else if (scalar_is_unsigned(src_kind)) {
            b = bool_from_unsigned(u);
        } else if (src_float32) {
            b = bool_from_float(f);
        } else if (src_float64) {
            b = bool_from_double(d);
        }
        store_uint_bytes(IOX2_CIF_SCALAR_BOOL, b ? 1u : 0u, dst);
        return;
    }
    if (dst_kind == IOX2_CIF_SCALAR_F32) {
        float out = 0.0f;
        if (scalar_is_signed(src_kind)) {
            out = (float)s;
        } else if (scalar_is_unsigned(src_kind)) {
            out = (float)u;
        } else if (src_float32) {
            out = f;
        } else if (src_float64) {
            store_f32_sat_from_double(d, dst);
            return;
        }
        memcpy(dst, &out, sizeof(out));
        return;
    }
    if (dst_kind == IOX2_CIF_SCALAR_F64) {
        double out = 0.0;
        if (scalar_is_signed(src_kind)) {
            out = (double)s;
        } else if (scalar_is_unsigned(src_kind)) {
            out = (double)u;
        } else if (src_float32) {
            out = (double)f;
        } else if (src_float64) {
            out = d;
        }
        memcpy(dst, &out, sizeof(out));
        return;
    }
    if (scalar_is_signed(dst_kind)) {
        int64_t lo = scalar_signed_min(dst_kind);
        int64_t hi = scalar_signed_max(dst_kind);
        int64_t out = 0;
        if (scalar_is_signed(src_kind)) {
            out = s > hi ? hi : (s < lo ? lo : s);
        } else if (scalar_is_unsigned(src_kind)) {
            out = u > (uint64_t)hi ? hi : (int64_t)u;
        } else if (src_float32) {
            out = sat_i64_from_float(f, lo, hi);
        } else if (src_float64) {
            out = sat_i64_from_double(d, lo, hi);
        }
        store_int_bytes(dst_kind, out, dst);
        return;
    }
    if (scalar_is_unsigned(dst_kind)) {
        uint64_t hi = scalar_unsigned_max(dst_kind);
        uint64_t out = 0;
        if (scalar_is_signed(src_kind)) {
            out = s < 0 ? 0u : ((uint64_t)s > hi ? hi : (uint64_t)s);
        } else if (scalar_is_unsigned(src_kind)) {
            out = u > hi ? hi : u;
        } else if (src_float32) {
            out = sat_u64_from_float(f, hi);
        } else if (src_float64) {
            out = sat_u64_from_double(d, hi);
        }
        store_uint_bytes(dst_kind, out, dst);
    }
}

static const char* json_skip_ws(const char* p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        p += 1;
    }
    return p;
}

static int json_skip_string(const char** pp) {
    const char* p = *pp;
    if (*p != '"') {
        return 0;
    }
    p += 1;
    while (*p != 0 && *p != '"') {
        if (*p == '\\') {
            p += 1;
            if (*p == 0) {
                return 0;
            }
        }
        p += 1;
    }
    if (*p != '"') {
        return 0;
    }
    *pp = p + 1;
    return 1;
}

static int json_parse_string(const char** pp, char* dst, size_t dst_sz) {
    const char* p = *pp;
    size_t n = 0;
    if (*p != '"') {
        return 0;
    }
    p += 1;
    while (*p != 0 && *p != '"') {
        char c = *p;
        p += 1;
        if (c == '\\') {
            if (*p == 0) {
                return 0;
            }
            c = *p;
            p += 1;
            if (c == 'n') {
                c = '\n';
            } else if (c == 't') {
                c = '\t';
            } else if (c == 'r') {
                c = '\r';
            }
        }
        if (n + 1u >= dst_sz) {
            return 0;
        }
        dst[n] = c;
        n += 1;
    }
    if (*p != '"') {
        return 0;
    }
    dst[n] = 0;
    *pp = p + 1;
    return 1;
}

static int json_skip_value(const char** pp);

static int json_skip_object(const char** pp) {
    const char* p = json_skip_ws(*pp);
    if (*p != '{') {
        return 0;
    }
    p += 1;
    p = json_skip_ws(p);
    if (*p == '}') {
        *pp = p + 1;
        return 1;
    }
    for (;;) {
        char key[8];
        p = json_skip_ws(p);
        if (!json_parse_string(&p, key, sizeof(key)) && !json_skip_string(&p)) {
            return 0;
        }
        p = json_skip_ws(p);
        if (*p != ':') {
            return 0;
        }
        p += 1;
        if (!json_skip_value(&p)) {
            return 0;
        }
        p = json_skip_ws(p);
        if (*p == ',') {
            p += 1;
            continue;
        }
        if (*p == '}') {
            *pp = p + 1;
            return 1;
        }
        return 0;
    }
}

static int json_skip_array(const char** pp) {
    const char* p = json_skip_ws(*pp);
    if (*p != '[') {
        return 0;
    }
    p += 1;
    p = json_skip_ws(p);
    if (*p == ']') {
        *pp = p + 1;
        return 1;
    }
    for (;;) {
        if (!json_skip_value(&p)) {
            return 0;
        }
        p = json_skip_ws(p);
        if (*p == ',') {
            p += 1;
            continue;
        }
        if (*p == ']') {
            *pp = p + 1;
            return 1;
        }
        return 0;
    }
}

static int json_skip_value(const char** pp) {
    const char* p = json_skip_ws(*pp);
    if (*p == '"') {
        if (!json_skip_string(&p)) {
            return 0;
        }
        *pp = p;
        return 1;
    }
    if (*p == '{') {
        return json_skip_object(pp);
    }
    if (*p == '[') {
        return json_skip_array(pp);
    }
    if (strncmp(p, "true", 4) == 0) {
        *pp = p + 4;
        return 1;
    }
    if (strncmp(p, "false", 5) == 0) {
        *pp = p + 5;
        return 1;
    }
    if (strncmp(p, "null", 4) == 0) {
        *pp = p + 4;
        return 1;
    }
    if (*p == '-' || (*p >= '0' && *p <= '9')) {
        if (*p == '-') {
            p += 1;
        }
        while (*p >= '0' && *p <= '9') {
            p += 1;
        }
        if (*p == '.') {
            p += 1;
            while (*p >= '0' && *p <= '9') {
                p += 1;
            }
        }
        if (*p == 'e' || *p == 'E') {
            p += 1;
            if (*p == '+' || *p == '-') {
                p += 1;
            }
            while (*p >= '0' && *p <= '9') {
                p += 1;
            }
        }
        *pp = p;
        return 1;
    }
    return 0;
}

static int json_parse_u32(const char** pp, uint32_t* out) {
    const char* p = json_skip_ws(*pp);
    char* end = NULL;
    unsigned long v = 0;
    if (*p == '-') {
        return 0;
    }
    v = strtoul(p, &end, 10);
    if (end == p || v > 0xfffffffful) {
        return 0;
    }
    *out = (uint32_t)v;
    *pp = end;
    return 1;
}

static int json_parse_field_object(const char** pp, iox2_cif_field_desc* field) {
    const char* p = json_skip_ws(*pp);
    int saw_name = 0;
    int saw_type = 0;
    int saw_size = 0;
    int saw_offset = 0;
    memset(field, 0, sizeof(*field));
    if (*p != '{') {
        return 0;
    }
    p += 1;
    p = json_skip_ws(p);
    if (*p == '}') {
        *pp = p + 1;
        return 1;
    }
    for (;;) {
        char key[32];
        p = json_skip_ws(p);
        if (!json_parse_string(&p, key, sizeof(key))) {
            return 0;
        }
        p = json_skip_ws(p);
        if (*p != ':') {
            return 0;
        }
        p += 1;
        p = json_skip_ws(p);
        if (strcmp(key, "name") == 0) {
            if (!json_parse_string(&p, field->name, sizeof(field->name))) {
                return 0;
            }
            saw_name = 1;
        } else if (strcmp(key, "type") == 0) {
            char type_name[32];
            if (!json_parse_string(&p, type_name, sizeof(type_name))) {
                return 0;
            }
            field->type = scalar_from_name(type_name);
            field->type_ok = field->type != IOX2_CIF_SCALAR_NONE;
            saw_type = 1;
        } else if (strcmp(key, "size") == 0) {
            if (!json_parse_u32(&p, &field->size)) {
                return 0;
            }
            saw_size = 1;
        } else if (strcmp(key, "offset") == 0) {
            if (!json_parse_u32(&p, &field->offset)) {
                return 0;
            }
            saw_offset = 1;
        } else if (!json_skip_value(&p)) {
            return 0;
        }
        p = json_skip_ws(p);
        if (*p == ',') {
            p += 1;
            continue;
        }
        if (*p == '}') {
            *pp = p + 1;
            return saw_name && saw_type && saw_size && saw_offset;
        }
        return 0;
    }
}

static int json_parse_struct_elems(const char** pp, iox2_cif_idl* idl) {
    const char* p = json_skip_ws(*pp);
    if (*p != '[') {
        return 0;
    }
    p += 1;
    p = json_skip_ws(p);
    if (*p == ']') {
        *pp = p + 1;
        return 1;
    }
    for (;;) {
        iox2_cif_field_desc field;
        p = json_skip_ws(p);
        if (!json_parse_field_object(&p, &field)) {
            return 0;
        }
        if (idl->field_count < IOX2_CIF_MAX_FIELDS) {
            idl->fields[idl->field_count] = field;
            idl->field_count += 1;
        } else {
            return 0;
        }
        p = json_skip_ws(p);
        if (*p == ',') {
            p += 1;
            continue;
        }
        if (*p == ']') {
            *pp = p + 1;
            return 1;
        }
        return 0;
    }
}

/* Returns 1 when `json` is an object. Field readers require struct_elems. */
static int json_parse_idl(const char* json, iox2_cif_idl* idl) {
    const char* p = NULL;
    if (json == NULL || json[0] == 0) {
        return 0;
    }
    memset(idl, 0, sizeof(*idl));
    p = json_skip_ws(json);
    if (*p != '{') {
        return 0;
    }
    p += 1;
    p = json_skip_ws(p);
    if (*p == '}') {
        return 1;
    }
    for (;;) {
        char key[32];
        p = json_skip_ws(p);
        if (!json_parse_string(&p, key, sizeof(key))) {
            return 0;
        }
        p = json_skip_ws(p);
        if (*p != ':') {
            return 0;
        }
        p += 1;
        p = json_skip_ws(p);
        if (strcmp(key, "Endian") == 0) {
            char endian[16];
            if (!json_parse_string(&p, endian, sizeof(endian))) {
                return 0;
            }
            idl->big_endian = strcmp(endian, "big") == 0;
        } else if (strcmp(key, "Array") == 0) {
            if (strncmp(p, "true", 4) == 0) {
                idl->is_array = 1;
                p += 4;
            } else if (strncmp(p, "false", 5) == 0) {
                idl->is_array = 0;
                p += 5;
            } else {
                return 0;
            }
        } else if (strcmp(key, "struct_elems") == 0) {
            if (!json_parse_struct_elems(&p, idl)) {
                return 0;
            }
        } else if (!json_skip_value(&p)) {
            return 0;
        }
        p = json_skip_ws(p);
        if (*p == ',') {
            p += 1;
            continue;
        }
        if (*p == '}') {
            return 1;
        }
        return 0;
    }
}

static void swap_bytes(uint8_t* p, uint32_t n) {
    uint32_t i = 0;
    for (i = 0; i < n / 2u; ++i) {
        uint8_t tmp = p[i];
        p[i] = p[n - 1u - i];
        p[n - 1u - i] = tmp;
    }
}

static iox2_cif_status finish_reader(iox2_cif_reader* r, const char* type_name) {
    int rc = iox2_port_factory_reader_builder_create(
        iox2_port_factory_blackboard_reader_builder(&r->service, NULL), NULL, &r->reader);
    if (rc != IOX2_OK) {
        return IOX2_CIF_ERR_INTERNAL;
    }
    rc = iox2_reader_entry(
        &r->reader,
        NULL,
        &r->entry,
        &r->key,
        type_name,
        strlen(type_name),
        r->value_size,
        r->value_align);
    if (rc != IOX2_OK) {
        return IOX2_CIF_ERR_SCHEMA_MISMATCH;
    }
    r->handle = alloc_handle(IOX2_CIF_KIND_READER, r);
    if (r->handle == IOX2_CIF_INVALID_HANDLE) {
        return IOX2_CIF_ERR_NO_MEMORY;
    }
    return IOX2_CIF_OK;
}

static void drop_reader_obj(iox2_cif_reader* r) {
    if (r->entry != NULL) {
        iox2_entry_handle_drop(r->entry);
        r->entry = NULL;
    }
    if (r->reader != NULL) {
        iox2_reader_drop(r->reader);
        r->reader = NULL;
    }
    if (r->service != NULL) {
        iox2_port_factory_blackboard_drop(r->service);
        r->service = NULL;
    }
    if (r->service_name != NULL) {
        iox2_service_name_drop(r->service_name);
        r->service_name = NULL;
    }
    free(r);
}

static void reset_reader_port(iox2_cif_reader* r) {
    if (r->entry != NULL) {
        iox2_entry_handle_drop(r->entry);
        r->entry = NULL;
    }
    if (r->reader != NULL) {
        iox2_reader_drop(r->reader);
        r->reader = NULL;
    }
}

static iox2_cif_status bind_field_reader(
    iox2_cif_reader* r, const char* bb_name, const char* topic, iox2_cif_scalar dst) {
    iox2_cif_meta_tab tab;
    iox2_cif_status st = IOX2_CIF_OK;
    uint32_t i = 0;
    int any_prefix = 0;
    int best_len = -1;
    char best_topic[IOX2_CIF_TOPIC_CAP];
    iox2_cif_field_desc best_field;
    int best_array = 0;
    int best_be = 0;
    iox2_cif_dir_entry_t found;

    if (strchr(topic, '.') == NULL) {
        set_last_detail("published type does not match subscriber");
        return IOX2_CIF_ERR_SCHEMA_MISMATCH;
    }
    memset(&tab, 0, sizeof(tab));
    memset(&best_field, 0, sizeof(best_field));
    best_topic[0] = 0;
    st = meta_load(bb_name, &tab);
    if (st == IOX2_CIF_ERR_TOPIC_NOT_FOUND) {
        set_last_detail("no topic matches field name");
        return IOX2_CIF_ERR_TOPIC_NOT_FOUND;
    }
    if (st != IOX2_CIF_OK) {
        return st;
    }
    for (i = 0; i < tab.count; ++i) {
        const char* stored = tab.entries[i].topic;
        size_t tlen = strlen(stored);
        size_t rlen = strlen(topic);
        iox2_cif_idl idl;
        uint32_t f = 0;
        if (tlen == 0 || rlen <= tlen + 1u || strncmp(topic, stored, tlen) != 0 || topic[tlen] != '.') {
            continue;
        }
        any_prefix = 1;
        if (tab.entries[i].descriptor == NULL || !json_parse_idl(tab.entries[i].descriptor, &idl)) {
            continue;
        }
        for (f = 0; f < (uint32_t)idl.field_count; ++f) {
            if (strcmp(idl.fields[f].name, topic + tlen + 1u) == 0 && (int)tlen > best_len) {
                best_len = (int)tlen;
                memcpy(best_topic, stored, tlen + 1u);
                best_field = idl.fields[f];
                best_array = idl.is_array;
                best_be = idl.big_endian;
            }
        }
    }
    meta_tab_clear(&tab);
    if (best_len < 0) {
        if (any_prefix) {
            set_last_detail("cluster field missing or descriptor empty");
            return IOX2_CIF_ERR_SCHEMA_MISMATCH;
        }
        set_last_detail("no topic matches field name");
        return IOX2_CIF_ERR_TOPIC_NOT_FOUND;
    }
    if (best_array) {
        set_last_detail("cannot read a field of an array");
        return IOX2_CIF_ERR_SCHEMA_MISMATCH;
    }
    if (!best_field.type_ok || best_field.size != scalar_size(best_field.type)) {
        set_last_detail("cluster field type is not a standard scalar");
        return IOX2_CIF_ERR_SCHEMA_MISMATCH;
    }
    memset(&found, 0, sizeof(found));
    st = load_dir_entry(&r->service, best_topic, iox2_cif_topic_key(best_topic), &found);
    if (st == IOX2_CIF_ERR_INTERNAL || st == IOX2_CIF_ERR_NO_MEMORY) {
        return st;
    }
    if (st != IOX2_CIF_OK) {
        set_last_detail("cluster field topic is not in the directory");
        return IOX2_CIF_ERR_SCHEMA_MISMATCH;
    }
    if ((found.flags & 1u) != 0) {
        set_last_detail("cannot read a field of an array");
        return IOX2_CIF_ERR_SCHEMA_MISMATCH;
    }
    if (best_field.size > found.elem_size || best_field.offset > found.elem_size - best_field.size) {
        set_last_detail("cluster field offset is outside the value");
        return IOX2_CIF_ERR_SCHEMA_MISMATCH;
    }
    r->key = iox2_cif_topic_key(best_topic);
    apply_dir_entry(r, &found);
    r->is_field = 1;
    r->swap_endian = best_be;
    r->src_kind = (int)best_field.type;
    r->dst_kind = (int)dst;
    r->dst_elem_size = scalar_size(dst);
    r->field_offset = best_field.offset;
    r->field_size = best_field.size;
    return finish_reader(r, r->type_name);
}

static iox2_cif_status bind_scalar_or_field(
    iox2_cif_reader* r, const char* bb_name, const char* topic, iox2_cif_scalar dst) {
    iox2_cif_dir_entry_t found;
    iox2_cif_status st = IOX2_CIF_OK;
    iox2_cif_scalar src = IOX2_CIF_SCALAR_NONE;
    memset(&found, 0, sizeof(found));
    st = load_dir_entry(&r->service, topic, r->key, &found);
    if (st == IOX2_CIF_ERR_INTERNAL || st == IOX2_CIF_ERR_NO_MEMORY) {
        return st;
    }
    if (st == IOX2_CIF_OK) {
        uint32_t align = found.elem_align == 0 ? 1u : found.elem_align;
        if ((found.flags & 1u) != 0) {
            set_last_detail("scalar reader cannot convert an array topic");
            return IOX2_CIF_ERR_SCHEMA_MISMATCH;
        }
        src = scalar_from_name(found.type_name);
        if (src != IOX2_CIF_SCALAR_NONE && src != dst && found.elem_size == scalar_size(src) && align == scalar_size(src)) {
            apply_dir_entry(r, &found);
            r->src_kind = (int)src;
            r->dst_kind = (int)dst;
            r->dst_elem_size = scalar_size(dst);
            r->is_field = 0;
            return finish_reader(r, r->type_name);
        }
        set_last_detail("reader type does not match published value");
        return IOX2_CIF_ERR_SCHEMA_MISMATCH;
    }
    return bind_field_reader(r, bb_name, topic, dst);
}

iox2_cif_status iox2_cif_bb_reader_create(
    const char* bb_name,
    const char* topic,
    const char* type_name,
    uint32_t elem_size,
    uint32_t elem_align,
    int32_t is_iox2_cif_array,
    uint32_t timeout_ms,
    uint64_t* reader) {
    iox2_cif_reader* r = NULL;
    iox2_cif_status st = IOX2_CIF_OK;
    int is_array = is_iox2_cif_array != 0;
    uint64_t key = 0;
    uint64_t handle = 0;

    if (reader == NULL || bb_name == NULL || topic == NULL || type_name == NULL || type_name[0] == 0) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    if (elem_size == 0) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    key = iox2_cif_topic_key(topic);
    if (key == IOX2_CIF_RESERVED_KEY) {
        return IOX2_CIF_ERR_KEY_RESERVED;
    }
    r = (iox2_cif_reader*)calloc(1, sizeof(*r));
    if (r == NULL) {
        return IOX2_CIF_ERR_NO_MEMORY;
    }
    fill_iox_type_name(r->type_name, sizeof(r->type_name), type_name, is_array);
    r->key = key;
    r->elem_size = elem_size;
    r->elem_align = elem_align == 0 ? 1u : elem_align;
    r->max_count = is_array ? 0 : 1;
    r->is_array = is_array;
    compute_value_layout(0, elem_size, r->elem_align, 1, &r->value_size, &r->value_align);
    if (!is_array) {
        iox2_cif_scalar dst = scalar_from_name(type_name);
        if (dst != IOX2_CIF_SCALAR_NONE) {
            uint32_t need = scalar_size(dst);
            if (elem_size != need || r->elem_align != need) {
                set_last_detail("standard scalar elem_size/elem_align must match the type");
                free(r);
                return IOX2_CIF_ERR_INVALID_ARGUMENT;
            }
        }
    }

    st = lock_exclusive();
    if (st != IOX2_CIF_OK) {
        free(r);
        return st;
    }
    st = ensure_node_unlocked(NULL);
    unlock_exclusive();
    if (st != IOX2_CIF_OK) {
        free(r);
        return st;
    }
    st = open_reader_service(bb_name, timeout_ms, &r->service_name, &r->service);
    if (st != IOX2_CIF_OK) {
        free(r);
        return st;
    }
    st = lock_exclusive();
    if (st != IOX2_CIF_OK) {
        drop_reader_obj(r);
        return st;
    }

    if (is_array) {
        iox2_cif_dir_entry_t found;
        memset(&found, 0, sizeof(found));
        st = load_dir_entry(&r->service, topic, key, &found);
        if (st != IOX2_CIF_OK) {
            drop_reader_obj(r);
            unlock_exclusive();
            return st == IOX2_CIF_ERR_INTERNAL || st == IOX2_CIF_ERR_NO_MEMORY
                ? st
                : IOX2_CIF_ERR_TOPIC_NOT_FOUND;
        }
        if (!type_name_matches_dir(r->type_name, &found)
            && !type_name_matches_dir(type_name, &found)) {
            set_last_detail("reader type_name does not match stored iox2_cif.array type");
            drop_reader_obj(r);
            unlock_exclusive();
            return IOX2_CIF_ERR_SCHEMA_MISMATCH;
        }
        if (elem_size != found.elem_size || r->elem_align != found.elem_align) {
            set_last_detail("reader elem_size/elem_align do not match writer");
            drop_reader_obj(r);
            unlock_exclusive();
            return IOX2_CIF_ERR_SCHEMA_MISMATCH;
        }
        apply_dir_entry(r, &found);
    }
    st = finish_reader(r, r->type_name);
    if (st == IOX2_CIF_ERR_SCHEMA_MISMATCH && !is_array) {
        iox2_cif_scalar dst = scalar_from_name(type_name);
        if (dst != IOX2_CIF_SCALAR_NONE) {
            reset_reader_port(r);
            st = bind_scalar_or_field(r, bb_name, topic, dst);
        }
    }
    if (st != IOX2_CIF_OK) {
        drop_reader_obj(r);
        unlock_exclusive();
        return st == IOX2_CIF_ERR_SCHEMA_MISMATCH ? IOX2_CIF_ERR_SCHEMA_MISMATCH : st;
    }
    handle = r->handle;
    unlock_exclusive();
    st = store_u64(reader, handle);
    if (st != IOX2_CIF_OK) {
        iox2_cif_bb_destroy_reader(handle);
        return st;
    }
    return IOX2_CIF_OK;
}

iox2_cif_status iox2_cif_bb_reader_create_raw(
    const char* bb_name, const char* topic, uint32_t timeout_ms, uint64_t* reader) {
    iox2_cif_reader* r = NULL;
    iox2_cif_status st = IOX2_CIF_OK;
    uint64_t key = 0;
    uint64_t handle = 0;

    if (reader == NULL || bb_name == NULL || topic == NULL) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    key = iox2_cif_topic_key(topic);
    if (key == IOX2_CIF_RESERVED_KEY) {
        return IOX2_CIF_ERR_KEY_RESERVED;
    }
    r = (iox2_cif_reader*)calloc(1, sizeof(*r));
    if (r == NULL) {
        return IOX2_CIF_ERR_NO_MEMORY;
    }
    r->key = key;
    r->is_raw = 1;

    st = lock_exclusive();
    if (st != IOX2_CIF_OK) {
        free(r);
        return st;
    }
    st = ensure_node_unlocked(NULL);
    unlock_exclusive();
    if (st != IOX2_CIF_OK) {
        free(r);
        return st;
    }
    st = open_reader_service(bb_name, timeout_ms, &r->service_name, &r->service);
    if (st != IOX2_CIF_OK) {
        free(r);
        return st;
    }
    st = lock_exclusive();
    if (st != IOX2_CIF_OK) {
        drop_reader_obj(r);
        return st;
    }
    {
        iox2_cif_dir_entry_t found_ent;
        memset(&found_ent, 0, sizeof(found_ent));
        st = load_dir_entry(&r->service, topic, key, &found_ent);
        if (st != IOX2_CIF_OK) {
            drop_reader_obj(r);
            unlock_exclusive();
            return st == IOX2_CIF_ERR_INTERNAL ? IOX2_CIF_ERR_INTERNAL : IOX2_CIF_ERR_TOPIC_NOT_FOUND;
        }
        apply_dir_entry(r, &found_ent);
    }

    st = finish_reader(r, r->type_name);
    if (st != IOX2_CIF_OK) {
        drop_reader_obj(r);
        unlock_exclusive();
        return st;
    }
    handle = r->handle;
    unlock_exclusive();
    st = store_u64(reader, handle);
    if (st != IOX2_CIF_OK) {
        iox2_cif_bb_destroy_reader(handle);
        return st;
    }
    return IOX2_CIF_OK;
}

iox2_cif_status iox2_cif_bb_topic_info(
    const char* bb_name,
    const char* topic,
    uint32_t timeout_ms,
    char* type_name,
    uint32_t type_cap,
    uint32_t* type_len,
    char* descriptor,
    uint32_t desc_cap,
    uint32_t* desc_len) {
    char name_copy[IOX2_CIF_NAME_CAP];
    char topic_copy[IOX2_CIF_TOPIC_CAP];
    iox2_cif_status st = IOX2_CIF_OK;
#ifdef _WIN32
    ULONGLONG start = GetTickCount64();
#else
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
#endif

    st = copy_cstr_bounded(name_copy, sizeof(name_copy), bb_name);
    if (st != IOX2_CIF_OK || name_copy[0] == 0) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    st = copy_cstr_bounded(topic_copy, sizeof(topic_copy), topic);
    if (st != IOX2_CIF_OK || topic_copy[0] == 0) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }

    for (;;) {
        iox2_cif_writer* w = NULL;
        st = meta_lookup(
            name_copy, topic_copy, type_name, type_cap, type_len, descriptor, desc_cap, desc_len);
        if (st == IOX2_CIF_OK) {
            return st;
        }
        st = lock_shared();
        if (st != IOX2_CIF_OK) {
            return st;
        }
        w = find_local_writer(name_copy, topic_copy);
        if (w != NULL) {
            const char* desc = w->descriptor != NULL ? w->descriptor : "";
            uint32_t type_n = (uint32_t)strlen(w->type_name);
            uint32_t desc_n = (uint32_t)strlen(desc);
            st = write_cstr_out(type_name, type_cap, type_len, w->type_name, type_n);
            if (st == IOX2_CIF_OK) {
                st = write_cstr_out(descriptor, desc_cap, desc_len, desc, desc_n);
            }
            unlock_shared();
            return st;
        }
        unlock_shared();
#ifdef _WIN32
        if (timeout_ms == 0 || (GetTickCount64() - start) >= timeout_ms) {
            return IOX2_CIF_ERR_TOPIC_NOT_FOUND;
        }
        Sleep(10);
#else
        {
            struct timespec now;
            struct timespec delay;
            int64_t elapsed_ms = 0;
            clock_gettime(CLOCK_MONOTONIC, &now);
            elapsed_ms = (int64_t)(now.tv_sec - start.tv_sec) * 1000;
            elapsed_ms += (now.tv_nsec - start.tv_nsec) / 1000000;
            if (timeout_ms == 0 || elapsed_ms >= (int64_t)timeout_ms) {
                return IOX2_CIF_ERR_TOPIC_NOT_FOUND;
            }
            delay.tv_sec = 0;
            delay.tv_nsec = 10 * 1000000L;
            nanosleep(&delay, NULL);
        }
#endif
    }
}

iox2_cif_status iox2_cif_bb_write(uint64_t writer, const void* elems, uint32_t count) {
    iox2_cif_writer* w = NULL;
    void* payload = NULL;

    if (elems == NULL) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    {
        iox2_cif_status st = lock_shared();
        if (st != IOX2_CIF_OK) {
            return st;
        }
    }
    w = (iox2_cif_writer*)lookup_handle(writer, IOX2_CIF_KIND_WRITER);
    if (w == NULL || !w->valid) {
        unlock_shared();
        return IOX2_CIF_ERR_INVALID_HANDLE;
    }
    if (w->entry == NULL) {
        unlock_shared();
        return IOX2_CIF_ERR_NOT_STARTED;
    }
    if (count > w->max_count) {
        unlock_shared();
        return IOX2_CIF_ERR_ARRAY_TOO_LARGE;
    }
    if (!w->is_array && count != 1) {
        unlock_shared();
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    payload = make_payload(w, elems, count);
    if (payload == NULL) {
        unlock_shared();
        return IOX2_CIF_ERR_NO_MEMORY;
    }
    iox2_entry_handle_mut_update_with_copy(&w->entry, payload, w->value_size, w->value_align);
    free(payload);
    unlock_shared();
    return IOX2_CIF_OK;
}

static iox2_cif_status copy_converted(
    const iox2_cif_reader* r, const void* buf, void* elems, uint32_t cap, uint32_t* actual, uint32_t* max) {
    uint8_t raw[8];
    uint8_t out[8];
    const uint8_t* src_bytes = NULL;
    uint32_t src_n = 0;
    uint32_t dst_n = 0;
    iox2_cif_status st = IOX2_CIF_OK;

    if (actual != NULL) {
        st = store_u32(actual, 1);
        if (st != IOX2_CIF_OK) {
            return st;
        }
    }
    if (max != NULL) {
        st = store_u32(max, 1);
        if (st != IOX2_CIF_OK) {
            return st;
        }
    }
    if (cap == 0 || elems == NULL) {
        return IOX2_CIF_OK;
    }
    if (cap < 1u) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    dst_n = r->dst_elem_size != 0 ? r->dst_elem_size : scalar_size((iox2_cif_scalar)r->dst_kind);
    if (dst_n == 0 || dst_n > sizeof(out)) {
        return IOX2_CIF_ERR_INTERNAL;
    }
    if (r->is_field) {
        if (r->field_size == 0 || r->field_size > sizeof(raw) || r->field_offset > r->value_size
            || r->field_size > r->value_size - r->field_offset) {
            return IOX2_CIF_ERR_INTERNAL;
        }
        memcpy(raw, (const uint8_t*)buf + r->field_offset, r->field_size);
        if (r->swap_endian) {
            swap_bytes(raw, r->field_size);
        }
        src_bytes = raw;
        src_n = r->field_size;
    } else {
        src_n = r->elem_size;
        if (src_n == 0 || src_n > sizeof(raw) || src_n > r->value_size) {
            return IOX2_CIF_ERR_INTERNAL;
        }
        memcpy(raw, buf, src_n);
        src_bytes = raw;
    }
    if (r->src_kind == r->dst_kind) {
        if (src_n != dst_n) {
            return IOX2_CIF_ERR_INTERNAL;
        }
        memcpy(out, src_bytes, dst_n);
    } else {
        convert_scalar((iox2_cif_scalar)r->src_kind, src_bytes, (iox2_cif_scalar)r->dst_kind, out);
    }
    return copy_bytes_bounded(elems, out, dst_n);
}

static iox2_cif_status read_into(
    iox2_cif_reader* r, void* elems, uint32_t cap, uint32_t* actual, uint32_t* max) {
    void* buf = NULL;
    uint32_t live = 1;
    uint32_t maxc = 1;
    const void* src = NULL;
    size_t nbytes = 0;
    iox2_cif_status st = IOX2_CIF_OK;

    if (r->value_size == 0) {
        return IOX2_CIF_ERR_INTERNAL;
    }
    buf = malloc(r->value_size);
    if (buf == NULL) {
        return IOX2_CIF_ERR_NO_MEMORY;
    }
    iox2_entry_handle_get(&r->entry, buf, r->value_size, r->value_align, NULL);
    if (r->is_field || r->src_kind != r->dst_kind) {
        st = copy_converted(r, buf, elems, cap, actual, max);
        free(buf);
        return st;
    }
    if (r->is_array) {
        iox2_cif_slot_t header;
        memcpy(&header, buf, sizeof(header));
        live = header.count;
        if (r->max_count > 0 && live > r->max_count) {
            live = r->max_count;
        }
        maxc = r->max_count;
        src = (uint8_t*)buf + IOX2_CIF_SLOT_HEADER_SIZE;
        nbytes = (size_t)live * (size_t)r->elem_size;
        if (nbytes + IOX2_CIF_SLOT_HEADER_SIZE > r->value_size) {
            nbytes = r->value_size > IOX2_CIF_SLOT_HEADER_SIZE
                ? (size_t)r->value_size - IOX2_CIF_SLOT_HEADER_SIZE
                : 0;
        }
    } else {
        live = 1;
        maxc = 1;
        src = buf;
        nbytes = r->elem_size;
    }
    if (actual != NULL) {
        st = store_u32(actual, live);
        if (st != IOX2_CIF_OK) {
            free(buf);
            return st;
        }
    }
    if (max != NULL) {
        st = store_u32(max, maxc);
        if (st != IOX2_CIF_OK) {
            free(buf);
            return st;
        }
    }
    if (cap == 0 || elems == NULL) {
        free(buf);
        return IOX2_CIF_OK;
    }
    if (cap < live) {
        free(buf);
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    st = copy_bytes_bounded(elems, src, nbytes);
    free(buf);
    return st;
}

iox2_cif_status iox2_cif_bb_read(
    uint64_t reader, void* elems, uint32_t cap, uint32_t* actual, uint32_t* max) {
    iox2_cif_reader* r = NULL;
    iox2_cif_status st = lock_shared();
    if (st != IOX2_CIF_OK) {
        return st;
    }
    r = (iox2_cif_reader*)lookup_handle(reader, IOX2_CIF_KIND_READER);
    if (r == NULL || r->entry == NULL) {
        unlock_shared();
        return IOX2_CIF_ERR_INVALID_HANDLE;
    }
    st = read_into(r, elems, cap, actual, max);
    unlock_shared();
    return st;
}

static void raw_payload_sizes(
    const iox2_cif_reader* r, const void* buf, uint32_t* used, uint32_t* maxb) {
    if (r->is_array) {
        iox2_cif_slot_t header;
        uint32_t live = 0;
        uint64_t used64 = 0;
        memcpy(&header, buf, sizeof(header));
        live = header.count;
        if (r->max_count > 0 && live > r->max_count) {
            live = r->max_count;
        }
        used64 = (uint64_t)IOX2_CIF_SLOT_HEADER_SIZE + (uint64_t)live * (uint64_t)r->elem_size;
        if (used64 > r->value_size) {
            used64 = r->value_size;
        }
        *used = (uint32_t)used64;
        *maxb = r->value_size;
    } else {
        *used = r->elem_size;
        *maxb = r->elem_size;
    }
}

static iox2_cif_status raw_get(
    iox2_cif_reader* r, void** buf_out, uint32_t* used, uint32_t* maxb) {
    void* buf = NULL;
    if (r->value_size == 0) {
        return IOX2_CIF_ERR_INTERNAL;
    }
    buf = malloc(r->value_size);
    if (buf == NULL) {
        return IOX2_CIF_ERR_NO_MEMORY;
    }
    iox2_entry_handle_get(&r->entry, buf, r->value_size, r->value_align, NULL);
    raw_payload_sizes(r, buf, used, maxb);
    *buf_out = buf;
    return IOX2_CIF_OK;
}

iox2_cif_status iox2_cif_bb_read_raw(
    uint64_t reader, void* bytes, uint32_t cap_bytes, uint32_t* actual_bytes, uint32_t* max_bytes) {
    iox2_cif_reader* r = NULL;
    void* buf = NULL;
    uint32_t used = 0;
    uint32_t maxb = 0;
    iox2_cif_status st = lock_shared();
    if (st != IOX2_CIF_OK) {
        return st;
    }
    r = (iox2_cif_reader*)lookup_handle(reader, IOX2_CIF_KIND_READER);
    if (r == NULL || r->entry == NULL) {
        unlock_shared();
        return IOX2_CIF_ERR_INVALID_HANDLE;
    }
    st = raw_get(r, &buf, &used, &maxb);
    if (st != IOX2_CIF_OK) {
        unlock_shared();
        return st;
    }
    if (actual_bytes != NULL) {
        st = store_u32(actual_bytes, used);
        if (st != IOX2_CIF_OK) {
            free(buf);
            unlock_shared();
            return st;
        }
    }
    if (max_bytes != NULL) {
        st = store_u32(max_bytes, maxb);
        if (st != IOX2_CIF_OK) {
            free(buf);
            unlock_shared();
            return st;
        }
    }
    /* cap_bytes == 0 is a size probe. LabVIEW Adapt to Type still passes a
     * non-NULL bytes pointer, so do not require bytes == NULL. */
    if (cap_bytes == 0 || bytes == NULL) {
        free(buf);
        unlock_shared();
        return IOX2_CIF_OK;
    }
    if (cap_bytes < used) {
        free(buf);
        unlock_shared();
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    st = copy_bytes_bounded(bytes, buf, used);
    free(buf);
    unlock_shared();
    return st;
}

iox2_cif_status iox2_cif_bb_read_raw_at(
    uint64_t reader,
    void* bytes,
    uint32_t offset_bytes,
    uint32_t cap_bytes,
    uint32_t* actual_bytes,
    uint32_t* max_bytes) {
    iox2_cif_reader* r = NULL;
    void* buf = NULL;
    uint32_t used = 0;
    uint32_t maxb = 0;
    uint32_t n = 0;
    iox2_cif_status st = lock_shared();
    if (st != IOX2_CIF_OK) {
        return st;
    }
    r = (iox2_cif_reader*)lookup_handle(reader, IOX2_CIF_KIND_READER);
    if (r == NULL || r->entry == NULL) {
        unlock_shared();
        return IOX2_CIF_ERR_INVALID_HANDLE;
    }
    st = raw_get(r, &buf, &used, &maxb);
    if (st != IOX2_CIF_OK) {
        unlock_shared();
        return st;
    }
    if (offset_bytes < used) {
        uint32_t avail = used - offset_bytes;
        n = cap_bytes < avail ? cap_bytes : avail;
    }
    if (actual_bytes != NULL) {
        st = store_u32(actual_bytes, n);
        if (st != IOX2_CIF_OK) {
            free(buf);
            unlock_shared();
            return st;
        }
    }
    if (max_bytes != NULL) {
        st = store_u32(max_bytes, maxb);
        if (st != IOX2_CIF_OK) {
            free(buf);
            unlock_shared();
            return st;
        }
    }
    if (n == 0 || bytes == NULL) {
        free(buf);
        unlock_shared();
        return IOX2_CIF_OK;
    }
    st = copy_bytes_bounded(bytes, (uint8_t*)buf + offset_bytes, n);
    free(buf);
    unlock_shared();
    return st;
}

static void destroy_reader_unlocked(iox2_cif_reader* r, uint64_t handle) {
    free_handle(handle);
    drop_reader_obj(r);
}

static void destroy_bb_unlocked(iox2_cif_bb* b, uint32_t* readers_remaining) {
    iox2_cif_writer* w = NULL;
    uint32_t remaining = 0;
    if (b->service != NULL) {
        remaining =
            (uint32_t)iox2_port_factory_blackboard_dynamic_config_number_of_readers(&b->service);
    }
    drop_bb_ports(b);
    if (remaining == 0) {
        uint32_t ti = 0;
        for (ti = 0; ti < b->staged_count; ++ti) {
            meta_remove_topic(b->name, b->staged_topics[ti]);
        }
    }
    w = b->writers;
    while (w != NULL) {
        iox2_cif_writer* next = w->next;
        w->valid = 0;
        w->bb = NULL;
        free_handle(w->handle);
        free(w->initial);
        free(w->descriptor);
        free(w->create_payload);
        free(w);
        w = next;
    }
    b->writers = NULL;
    free_handle(b->handle);
    free(b);
    if (readers_remaining != NULL) {
        *readers_remaining = remaining;
    }
}

iox2_cif_status iox2_cif_bb_destroy_writer(uint64_t writer) {
    iox2_cif_writer* w = NULL;
    iox2_cif_bb* b = NULL;
    iox2_cif_writer** pp = NULL;
    iox2_cif_status st = lock_exclusive();
    if (st != IOX2_CIF_OK) {
        return st;
    }
    w = (iox2_cif_writer*)lookup_handle(writer, IOX2_CIF_KIND_WRITER);
    if (w == NULL) {
        unlock_exclusive();
        return IOX2_CIF_ERR_INVALID_HANDLE;
    }
    b = w->bb;
    drop_writer_entry(w);
    if (b != NULL) {
        for (pp = &b->writers; *pp != NULL; pp = &(*pp)->next) {
            if (*pp == w) {
                *pp = w->next;
                break;
            }
        }
        if (!b->started) {
            meta_remove_topic(b->name, w->topic);
        }
    }
    free_handle(writer);
    free(w->initial);
    free(w->descriptor);
    free(w->create_payload);
    free(w);
    unlock_exclusive();
    return IOX2_CIF_OK;
}

iox2_cif_status iox2_cif_bb_destroy_reader(uint64_t reader) {
    iox2_cif_reader* r = NULL;
    iox2_cif_status st = lock_exclusive();
    if (st != IOX2_CIF_OK) {
        return st;
    }
    r = (iox2_cif_reader*)lookup_handle(reader, IOX2_CIF_KIND_READER);
    if (r == NULL) {
        unlock_exclusive();
        return IOX2_CIF_ERR_INVALID_HANDLE;
    }
    destroy_reader_unlocked(r, reader);
    unlock_exclusive();
    return IOX2_CIF_OK;
}

iox2_cif_status iox2_cif_bb_destroy(uint64_t bb, uint32_t* readers_remaining) {
    iox2_cif_bb* b = NULL;
    uint32_t remaining = 0;
    iox2_cif_status st = lock_exclusive();
    if (st != IOX2_CIF_OK) {
        return st;
    }
    b = (iox2_cif_bb*)lookup_handle(bb, IOX2_CIF_KIND_BB);
    if (b == NULL) {
        unlock_exclusive();
        return IOX2_CIF_ERR_INVALID_HANDLE;
    }
    destroy_bb_unlocked(b, &remaining);
    unlock_exclusive();
    if (readers_remaining != NULL) {
        return store_u32(readers_remaining, remaining);
    }
    return IOX2_CIF_OK;
}

/* -------------------------------------------------------------------------- */
/* Publish-subscribe                                                            */
/* -------------------------------------------------------------------------- */

#define IOX2_CIF_PS_BE_SUFFIX ".iox2_cif.ps.be"
#define IOX2_CIF_PS_BP_SUFFIX ".iox2_cif.ps.bp"
#define IOX2_CIF_PS_MAX_SUBS IOX2_CIF_MAX_READERS
typedef char iox2_cif_pcap_hdr_size_ok[(sizeof(iox2_cif_pcap_hdr_t) == IOX2_CIF_PCAP_HDR_SIZE) ? 1 : -1];

typedef struct iox2_cif_ps_chan {
    iox2_service_name_h name;
    iox2_port_factory_pub_sub_h pubsub;
    iox2_publisher_h publisher;
    iox2_port_factory_event_h event;
    iox2_notifier_h notifier;
} iox2_cif_ps_chan;

typedef struct iox2_cif_ps_writer {
    uint64_t handle;
    char name[IOX2_CIF_NAME_CAP];
    char topic[IOX2_CIF_TOPIC_CAP];
    char type_name[IOX2_CIF_TYPE_NAME_CAP];
    uint32_t elem_size;
    uint32_t elem_align;
    uint32_t max_count;
    uint32_t value_size;
    uint32_t value_align;
    int is_array;
    int supports_backpressure;
    int is_pcap;
    int dying;
    uint32_t inflight;
    uint32_t linktype;
    uint32_t be_subscriber_buffer;
    uint32_t bp_subscriber_buffer;
    uint64_t pcap_ctr;
    uint64_t stat_samples;
    uint64_t stat_timeouts;
    uint64_t stat_last_ctr;
    char* descriptor;
    iox2_cif_ps_chan be;
    iox2_cif_ps_chan bp;
} iox2_cif_ps_writer;

typedef struct iox2_cif_ps_sub {
    uint64_t handle;
    char type_name[IOX2_CIF_TYPE_NAME_CAP];
    uint32_t elem_size;
    uint32_t elem_align;
    uint32_t max_count;
    uint32_t value_size;
    uint32_t value_align;
    int is_array;
    int use_backpressure;
    int is_pcap;
    int dying;
    int stat_have_last;
    uint32_t inflight;
    uint32_t linktype;
    uint32_t subscriber_buffer;
    uint64_t stat_samples;
    uint64_t stat_dropped;
    uint64_t stat_timeouts;
    uint64_t stat_last_ctr;
    iox2_service_name_h name;
    iox2_port_factory_pub_sub_h pubsub;
    iox2_subscriber_h subscriber;
    iox2_port_factory_event_h event;
    iox2_listener_h listener;
    iox2_waitset_h waitset;
    iox2_waitset_guard_h guard;
} iox2_cif_ps_sub;

#ifdef _WIN32
static __declspec(thread) ULONGLONG g_ps_bp_deadline;
static __declspec(thread) int g_ps_bp_one_try;
#else
static __thread uint64_t g_ps_bp_deadline_ms;
static __thread int g_ps_bp_one_try;
#endif

static iox2_backpressure_action_e ps_bp_handler(iox2_backpressure_info_h_ref info, iox2_callback_context ctx);

static uint64_t now_ms(void) {
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
#endif
}

static void sleep_ms(uint32_t ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec delay;
    delay.tv_sec = (time_t)(ms / 1000u);
    delay.tv_nsec = (long)(ms % 1000u) * 1000000L;
    nanosleep(&delay, NULL);
#endif
}

static iox2_cif_status ps_compose_name(
    const char* name, const char* topic, const char* suffix, char* out, size_t out_sz) {
    int n = 0;
    if (name == NULL || topic == NULL || suffix == NULL || out == NULL || out_sz == 0) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    n = snprintf(out, out_sz, "%s/%s%s", name, topic, suffix);
    if (n <= 0 || (size_t)n >= out_sz || (size_t)n >= IOX2_SERVICE_NAME_LENGTH) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    return IOX2_CIF_OK;
}

static void ps_chan_clear(iox2_cif_ps_chan* c) {
    memset(c, 0, sizeof(*c));
}

static void ps_chan_drop(iox2_cif_ps_chan* c) {
    if (c == NULL) {
        return;
    }
    if (c->notifier != NULL) {
        iox2_notifier_drop(c->notifier);
        c->notifier = NULL;
    }
    if (c->publisher != NULL) {
        iox2_publisher_drop(c->publisher);
        c->publisher = NULL;
    }
    if (c->event != NULL) {
        iox2_port_factory_event_drop(c->event);
        c->event = NULL;
    }
    if (c->pubsub != NULL) {
        iox2_port_factory_pub_sub_drop(c->pubsub);
        c->pubsub = NULL;
    }
    if (c->name != NULL) {
        iox2_service_name_drop(c->name);
        c->name = NULL;
    }
}

static void ps_writer_free(iox2_cif_ps_writer* w) {
    if (w == NULL) {
        return;
    }
    ps_chan_drop(&w->bp);
    ps_chan_drop(&w->be);
    free(w->descriptor);
    free(w);
}

static void ps_sub_free(iox2_cif_ps_sub* s) {
    if (s == NULL) {
        return;
    }
    if (s->guard != NULL) {
        iox2_waitset_guard_drop(s->guard);
        s->guard = NULL;
    }
    if (s->waitset != NULL) {
        iox2_waitset_drop(s->waitset);
        s->waitset = NULL;
    }
    if (s->listener != NULL) {
        iox2_listener_drop(s->listener);
        s->listener = NULL;
    }
    if (s->event != NULL) {
        iox2_port_factory_event_drop(s->event);
        s->event = NULL;
    }
    if (s->subscriber != NULL) {
        iox2_subscriber_drop(s->subscriber);
        s->subscriber = NULL;
    }
    if (s->pubsub != NULL) {
        iox2_port_factory_pub_sub_drop(s->pubsub);
        s->pubsub = NULL;
    }
    if (s->name != NULL) {
        iox2_service_name_drop(s->name);
        s->name = NULL;
    }
    free(s);
}

static void destroy_ps_writer_unlocked(void* obj) {
    iox2_cif_ps_writer* w = (iox2_cif_ps_writer*)obj;
    uint64_t be_subs = 0;
    uint64_t bp_subs = 0;
    if (w == NULL) {
        return;
    }
    if (w->be.pubsub != NULL) {
        be_subs = iox2_port_factory_pub_sub_dynamic_config_number_of_subscribers(&w->be.pubsub);
    }
    if (w->bp.pubsub != NULL) {
        bp_subs = iox2_port_factory_pub_sub_dynamic_config_number_of_subscribers(&w->bp.pubsub);
    }
    if (be_subs == 0 && bp_subs == 0) {
        meta_remove_topic(w->name, w->topic);
    }
    free_handle(w->handle);
    w->dying = 1;
    if (w->inflight == 0) {
        ps_writer_free(w);
    }
}

static void destroy_ps_sub_unlocked(void* obj) {
    iox2_cif_ps_sub* s = (iox2_cif_ps_sub*)obj;
    if (s == NULL) {
        return;
    }
    free_handle(s->handle);
    s->dying = 1;
    if (s->inflight == 0) {
        ps_sub_free(s);
    }
}

static iox2_cif_status map_pubsub_open_error(int rc) {
    if (rc == IOX2_OK) {
        return IOX2_CIF_OK;
    }
    if (rc == (int)iox2_pub_sub_open_or_create_error_e_O_DOES_NOT_EXIST) {
        return IOX2_CIF_ERR_TIMEOUT;
    }
    if (rc == (int)iox2_pub_sub_open_or_create_error_e_O_INCOMPATIBLE_TYPES
        || rc == (int)iox2_pub_sub_open_or_create_error_e_O_INCOMPATIBLE_OVERFLOW_BEHAVIOR
        || rc == (int)iox2_pub_sub_open_or_create_error_e_O_INCOMPATIBLE_MESSAGING_PATTERN) {
        return IOX2_CIF_ERR_SCHEMA_MISMATCH;
    }
    set_last_detail(iox2_pub_sub_open_or_create_error_string((enum iox2_pub_sub_open_or_create_error_e)rc));
    return IOX2_CIF_ERR_INTERNAL;
}

static iox2_cif_status map_event_open_error(int rc) {
    if (rc == IOX2_OK) {
        return IOX2_CIF_OK;
    }
    if (rc == (int)iox2_event_open_or_create_error_e_O_DOES_NOT_EXIST) {
        return IOX2_CIF_ERR_TIMEOUT;
    }
    set_last_detail(iox2_event_open_or_create_error_string((enum iox2_event_open_or_create_error_e)rc));
    return IOX2_CIF_ERR_INTERNAL;
}

static void drop_static_config(iox2_static_config_t* cfg) {
    if (cfg != NULL && cfg->attributes != NULL) {
        iox2_attribute_set_drop(cfg->attributes);
        cfg->attributes = NULL;
    }
}

static int try_open_pubsub(
    const char* svc_name,
    const char* type_name,
    uint32_t value_size,
    uint32_t value_align,
    int safe_overflow,
    iox2_service_name_h* name_h,
    iox2_port_factory_pub_sub_h* service) {
    iox2_service_builder_h sb = NULL;
    iox2_service_builder_pub_sub_h psb;
    int rc = 0;

    *name_h = NULL;
    *service = NULL;
    rc = iox2_service_name_new(NULL, svc_name, strlen(svc_name), name_h);
    if (rc != IOX2_OK) {
        return rc;
    }
    sb = iox2_node_service_builder(&g_state.node, NULL, iox2_cast_service_name_ptr(*name_h));
    psb = iox2_service_builder_pub_sub(sb);
    rc = iox2_service_builder_pub_sub_set_payload_type_details(
        &psb,
        iox2_type_variant_e_FIXED_SIZE,
        type_name,
        strlen(type_name),
        value_size,
        value_align);
    if (rc != IOX2_OK) {
        iox2_service_name_drop(*name_h);
        *name_h = NULL;
        return rc;
    }
    iox2_service_builder_pub_sub_set_enable_safe_overflow(&psb, safe_overflow ? true : false);
    rc = iox2_service_builder_pub_sub_open(psb, NULL, service);
    if (rc != IOX2_OK) {
        iox2_service_name_drop(*name_h);
        *name_h = NULL;
        *service = NULL;
    }
    return rc;
}

static iox2_cif_status ps_probe_pubsub(
    const char* svc_name,
    const char* type_name,
    uint32_t elem_size,
    uint32_t elem_align,
    int is_array,
    int* exists,
    uint32_t* value_size,
    uint32_t* value_align,
    uint32_t* subscriber_buffer) {
    iox2_service_name_h name_h = NULL;
    iox2_static_config_t details;
    bool does_exist = false;
    int rc = 0;
    const iox2_type_detail_t* payload = NULL;

    *exists = 0;
    rc = iox2_service_name_new(NULL, svc_name, strlen(svc_name), &name_h);
    if (rc != IOX2_OK) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    memset(&details, 0, sizeof(details));
    rc = iox2_service_details(
        iox2_service_type_e_IPC,
        iox2_cast_service_name_ptr(name_h),
        iox2_node_config(&g_state.node),
        iox2_messaging_pattern_e_PUBLISH_SUBSCRIBE,
        &details,
        &does_exist);
    iox2_service_name_drop(name_h);
    if (rc != IOX2_OK) {
        set_last_detail(iox2_service_details_error_string((enum iox2_service_details_error_e)rc));
        return IOX2_CIF_ERR_INTERNAL;
    }
    if (!does_exist) {
        return IOX2_CIF_OK;
    }
    *exists = 1;
    if (details.messaging_pattern != iox2_messaging_pattern_e_PUBLISH_SUBSCRIBE) {
        drop_static_config(&details);
        return IOX2_CIF_ERR_SCHEMA_MISMATCH;
    }
    payload = &details.details.publish_subscribe.message_type_details.payload;
    if (strcmp(payload->type_name, type_name) != 0) {
        drop_static_config(&details);
        return IOX2_CIF_ERR_SCHEMA_MISMATCH;
    }
    if (is_array) {
        if (payload->alignment < max_u32(8u, elem_align)
            || payload->size < IOX2_CIF_SLOT_HEADER_SIZE + elem_size) {
            drop_static_config(&details);
            return IOX2_CIF_ERR_SCHEMA_MISMATCH;
        }
    } else if (payload->size != elem_size || payload->alignment != elem_align) {
        drop_static_config(&details);
        return IOX2_CIF_ERR_SCHEMA_MISMATCH;
    }
    *value_size = (uint32_t)payload->size;
    *value_align = (uint32_t)payload->alignment;
    if (subscriber_buffer != NULL) {
        *subscriber_buffer = (uint32_t)details.details.publish_subscribe.subscriber_max_buffer_size;
    }
    drop_static_config(&details);
    return IOX2_CIF_OK;
}

static size_t ps_sample_prealloc(size_t worst_case, iox2_callback_context ctx) {
    const size_t* want = (const size_t*)ctx;
    size_t n = 1;
    if (want != NULL) {
        n = *want;
    }
    if (n < 1u) {
        n = 1u;
    }
    if (n > worst_case) {
        n = worst_case;
    }
    return n;
}

/* Best-effort: queue_depth + max_subscribers + 1.
 * Backpressure: queue_depth + 2. Subscribers share one sequence. */
static size_t ps_sample_pool_size(int safe_overflow, uint32_t max_subscribers, uint32_t subscriber_buffer) {
    size_t depth = (size_t)subscriber_buffer;
    size_t extra = safe_overflow ? (size_t)max_subscribers : (size_t)1u;
    if (depth > (size_t)-1 - extra - 1u) {
        return (size_t)-1;
    }
    return depth + extra + 1u;
}

static iox2_cif_status ps_chan_create(
    iox2_cif_ps_chan* c,
    const char* svc_name,
    const char* type_name,
    uint32_t value_size,
    uint32_t value_align,
    int safe_overflow,
    int with_bp_handler,
    uint32_t max_publishers,
    uint32_t max_subscribers,
    uint32_t subscriber_buffer) {
    iox2_service_builder_h sb = NULL;
    iox2_service_builder_pub_sub_h psb;
    iox2_service_builder_event_h evb;
    iox2_port_factory_publisher_builder_h pubb;
    iox2_port_factory_notifier_builder_h ntb;
    int rc = 0;

    ps_chan_clear(c);
    rc = iox2_service_name_new(NULL, svc_name, strlen(svc_name), &c->name);
    if (rc != IOX2_OK) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }

    sb = iox2_node_service_builder(&g_state.node, NULL, iox2_cast_service_name_ptr(c->name));
    psb = iox2_service_builder_pub_sub(sb);
    rc = iox2_service_builder_pub_sub_set_payload_type_details(
        &psb,
        iox2_type_variant_e_FIXED_SIZE,
        type_name,
        strlen(type_name),
        value_size,
        value_align);
    if (rc != IOX2_OK) {
        set_last_detail("pub/sub type details");
        ps_chan_drop(c);
        return IOX2_CIF_ERR_INTERNAL;
    }
    iox2_service_builder_pub_sub_set_enable_safe_overflow(&psb, safe_overflow ? true : false);
    iox2_service_builder_pub_sub_set_history_size(&psb, 0);
    iox2_service_builder_pub_sub_set_max_publishers(&psb, max_publishers < 1u ? 1u : max_publishers);
    iox2_service_builder_pub_sub_set_max_subscribers(&psb, max_subscribers);
    iox2_service_builder_pub_sub_set_subscriber_max_buffer_size(&psb, subscriber_buffer);
    /* ps_receive holds one sample for the copy out. */
    iox2_service_builder_pub_sub_set_subscriber_max_borrowed_samples(&psb, 1);
    rc = iox2_service_builder_pub_sub_open_or_create(psb, NULL, &c->pubsub);
    if (rc != IOX2_OK) {
        iox2_cif_status st = map_pubsub_open_error(rc);
        if (rc == (int)iox2_pub_sub_open_or_create_error_e_O_DOES_NOT_SUPPORT_REQUESTED_AMOUNT_OF_PUBLISHERS) {
            st = IOX2_CIF_ERR_WRITER_HELD;
        }
        ps_chan_drop(c);
        return st;
    }

    sb = iox2_node_service_builder(&g_state.node, NULL, iox2_cast_service_name_ptr(c->name));
    evb = iox2_service_builder_event(sb);
    iox2_service_builder_event_set_max_notifiers(&evb, 2);
    iox2_service_builder_event_set_max_listeners(&evb, max_subscribers);
    rc = iox2_service_builder_event_open_or_create(evb, NULL, &c->event);
    if (rc != IOX2_OK) {
        ps_chan_drop(c);
        return map_event_open_error(rc);
    }

    pubb = iox2_port_factory_pub_sub_publisher_builder(&c->pubsub, NULL);
    if (with_bp_handler) {
        iox2_port_factory_publisher_builder_set_backpressure_handler(&pubb, ps_bp_handler, NULL);
    }
    {
        /* One in-flight publish. Best-effort: shared queue window plus one
         * pinned receive per subscriber, plus the loan that happens before
         * overflow frees a chunk. Backpressure: one shared window, one receive
         * still copying, and the loan held while a full queue waits. */
        size_t pool = ps_sample_pool_size(safe_overflow, max_subscribers, subscriber_buffer);
        iox2_port_factory_publisher_builder_set_max_loaned_samples(&pubb, 1);
        iox2_port_factory_publisher_builder_override_samples_preallocation(
            &pubb, ps_sample_prealloc, &pool);
        rc = iox2_port_factory_publisher_builder_create(pubb, NULL, &c->publisher);
    }
    if (rc != IOX2_OK) {
        if (rc == (int)iox2_publisher_create_error_e_EXCEEDS_MAX_SUPPORTED_PUBLISHERS) {
            ps_chan_drop(c);
            return IOX2_CIF_ERR_WRITER_HELD;
        }
        set_last_detail(iox2_publisher_create_error_string((enum iox2_publisher_create_error_e)rc));
        ps_chan_drop(c);
        return IOX2_CIF_ERR_INTERNAL;
    }

    ntb = iox2_port_factory_event_notifier_builder(&c->event, NULL);
    rc = iox2_port_factory_notifier_builder_create(ntb, NULL, &c->notifier);
    if (rc != IOX2_OK) {
        set_last_detail("notifier create");
        ps_chan_drop(c);
        return IOX2_CIF_ERR_INTERNAL;
    }
    return IOX2_CIF_OK;
}

static iox2_backpressure_action_e ps_bp_handler(iox2_backpressure_info_h_ref info, iox2_callback_context ctx) {
    (void)info;
    (void)ctx;
    if (g_ps_bp_one_try) {
        return iox2_backpressure_action_e_DISCARD_DATA_AND_FAIL;
    }
#ifdef _WIN32
    if (GetTickCount64() >= g_ps_bp_deadline) {
        return iox2_backpressure_action_e_DISCARD_DATA_AND_FAIL;
    }
    Sleep(0);
#else
    if (now_ms() >= g_ps_bp_deadline_ms) {
        return iox2_backpressure_action_e_DISCARD_DATA_AND_FAIL;
    }
    sched_yield();
#endif
    return iox2_backpressure_action_e_RETRY;
}

static int ps_writer_exists_unlocked(const char* name, const char* topic) {
    uint32_t i = 0;
    for (i = 0; i < IOX2_CIF_MAX_SLOTS; ++i) {
        if (g_state.slots[i].kind == IOX2_CIF_KIND_PS_WRITER
            || g_state.slots[i].kind == IOX2_CIF_KIND_PCAP_WRITER) {
            iox2_cif_ps_writer* w = (iox2_cif_ps_writer*)g_state.slots[i].ptr;
            if (w != NULL && !w->dying && strcmp(w->name, name) == 0 && strcmp(w->topic, topic) == 0) {
                return 1;
            }
        }
    }
    return 0;
}

static void* make_ps_payload(
    int is_array,
    uint32_t elem_size,
    uint32_t value_size,
    const void* elems,
    uint32_t count) {
    uint8_t* buf = (uint8_t*)calloc(1, value_size);
    iox2_cif_slot_t header;
    if (buf == NULL) {
        return NULL;
    }
    if (!is_array) {
        if (copy_bytes_bounded(buf, elems, elem_size) != IOX2_CIF_OK) {
            free(buf);
            return NULL;
        }
        return buf;
    }
    header.count = count;
    header.reserved = 0;
    memcpy(buf, &header, sizeof(header));
    if (count > 0 && elems != NULL) {
        if (copy_bytes_bounded(buf + IOX2_CIF_SLOT_HEADER_SIZE, elems, (size_t)count * elem_size)
            != IOX2_CIF_OK) {
            free(buf);
            return NULL;
        }
    }
    return buf;
}

static iox2_cif_status ps_copy_sample(
    iox2_cif_ps_sub* s,
    const void* buf,
    uint32_t nbytes,
    void* elems,
    uint32_t cap,
    uint32_t* actual,
    uint32_t* max) {
    uint32_t live = 1;
    uint32_t maxc = 1;
    const void* src = buf;
    size_t copy_n = 0;
    iox2_cif_status st = IOX2_CIF_OK;

    if (s->is_array) {
        iox2_cif_slot_t header;
        if (nbytes < IOX2_CIF_SLOT_HEADER_SIZE) {
            return IOX2_CIF_ERR_INTERNAL;
        }
        memcpy(&header, buf, sizeof(header));
        live = header.count;
        if (s->max_count > 0 && live > s->max_count) {
            live = s->max_count;
        }
        maxc = s->max_count;
        src = (const uint8_t*)buf + IOX2_CIF_SLOT_HEADER_SIZE;
        copy_n = (size_t)live * (size_t)s->elem_size;
        if (copy_n + IOX2_CIF_SLOT_HEADER_SIZE > nbytes) {
            copy_n = nbytes > IOX2_CIF_SLOT_HEADER_SIZE ? (size_t)nbytes - IOX2_CIF_SLOT_HEADER_SIZE : 0;
        }
    } else {
        live = 1;
        maxc = 1;
        src = buf;
        copy_n = s->elem_size;
        if (copy_n > nbytes) {
            copy_n = nbytes;
        }
    }
    if (actual != NULL) {
        st = store_u32(actual, live);
        if (st != IOX2_CIF_OK) {
            return st;
        }
    }
    if (max != NULL) {
        st = store_u32(max, maxc);
        if (st != IOX2_CIF_OK) {
            return st;
        }
    }
    if (cap == 0 || elems == NULL) {
        return IOX2_CIF_OK;
    }
    if (cap < live) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    return copy_bytes_bounded(elems, src, copy_n);
}

static void ignore_event(const iox2_event_id_t* id, uint64_t count, iox2_callback_context ctx) {
    (void)id;
    (void)count;
    (void)ctx;
}

static iox2_callback_progression_e ps_on_wait(iox2_waitset_attachment_id_h id, void* ctx) {
    iox2_cif_ps_sub* s = (iox2_cif_ps_sub*)ctx;
    uint64_t n = 0;
    if (s != NULL && s->listener != NULL) {
        iox2_listener_try_wait(&s->listener, &n, ignore_event, NULL);
    }
    iox2_waitset_attachment_id_drop(id);
    return iox2_callback_progression_e_STOP;
}

/* be_max_subscribers == 0 skips best-effort. bp_max_subscribers == 0 skips
 * backpressure. 8100 only when neither publisher would be created. */
static iox2_cif_status ps_select_sides(
    uint32_t be_max_subscribers,
    uint32_t bp_max_subscribers,
    uint32_t be_subscriber_buffer,
    uint32_t bp_subscriber_buffer,
    int* create_be,
    int* create_bp) {
    int want_be = be_max_subscribers > 0u;
    int want_bp = bp_max_subscribers > 0u;
    if (!want_be && !want_bp) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    if (want_be
        && (be_max_subscribers > IOX2_CIF_PS_MAX_SUBS || be_subscriber_buffer < 1u)) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    if (want_bp
        && (bp_max_subscribers > IOX2_CIF_PS_MAX_SUBS || bp_subscriber_buffer < 1u)) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    *create_be = want_be;
    *create_bp = want_bp;
    return IOX2_CIF_OK;
}

iox2_cif_status iox2_cif_ps_writer_create(
    const char* name,
    const char* topic,
    const char* type_name,
    uint32_t elem_size,
    uint32_t elem_align,
    uint32_t max_count,
    const char* descriptor,
    uint32_t be_max_subscribers,
    uint32_t bp_max_subscribers,
    uint32_t be_subscriber_buffer,
    uint32_t bp_subscriber_buffer,
    uint64_t* writer) {
    iox2_cif_ps_writer* w = NULL;
    char name_copy[IOX2_CIF_NAME_CAP];
    char topic_copy[IOX2_CIF_TOPIC_CAP];
    char type_copy[IOX2_CIF_TYPE_NAME_CAP];
    char be_name[IOX2_SERVICE_NAME_LENGTH];
    char bp_name[IOX2_SERVICE_NAME_LENGTH];
    char* desc_copy = NULL;
    int is_array = 0;
    int create_be = 0;
    int create_bp = 0;
    iox2_cif_status st = IOX2_CIF_OK;

    if (writer == NULL || elem_size == 0) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    st = ps_select_sides(
        be_max_subscribers,
        bp_max_subscribers,
        be_subscriber_buffer,
        bp_subscriber_buffer,
        &create_be,
        &create_bp);
    if (st != IOX2_CIF_OK) {
        return st;
    }
    st = copy_cstr_bounded(name_copy, sizeof(name_copy), name);
    if (st != IOX2_CIF_OK || name_copy[0] == 0) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    st = copy_cstr_bounded(topic_copy, sizeof(topic_copy), topic);
    if (st != IOX2_CIF_OK || topic_copy[0] == 0) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    st = copy_cstr_bounded(type_copy, sizeof(type_copy), type_name);
    if (st != IOX2_CIF_OK || type_copy[0] == 0) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    if (strcmp(type_copy, IOX2_CIF_PCAP_TYPE) == 0 || starts_with(type_copy, IOX2_CIF_PCAP_TYPE_PREFIX)) {
        return IOX2_CIF_ERR_TYPE_REJECTED;
    }
    if (iox2_cif_topic_key(topic_copy) == IOX2_CIF_RESERVED_KEY) {
        return IOX2_CIF_ERR_KEY_RESERVED;
    }
    st = dup_optional_cstr(descriptor, IOX2_CIF_DESC_MAX - 1u, &desc_copy);
    if (st != IOX2_CIF_OK) {
        return st;
    }
    is_array = starts_with(type_copy, IOX2_CIF_ARRAY_TYPE_PREFIX) || max_count > 1u;
    if (!is_array) {
        max_count = 1;
    } else if (max_count < 1u) {
        free(desc_copy);
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    if (elem_align == 0) {
        elem_align = 1;
    }
    if (create_be) {
        st = ps_compose_name(name_copy, topic_copy, IOX2_CIF_PS_BE_SUFFIX, be_name, sizeof(be_name));
        if (st != IOX2_CIF_OK) {
            free(desc_copy);
            return st;
        }
    }
    if (create_bp) {
        st = ps_compose_name(name_copy, topic_copy, IOX2_CIF_PS_BP_SUFFIX, bp_name, sizeof(bp_name));
        if (st != IOX2_CIF_OK) {
            free(desc_copy);
            return st;
        }
    }

    w = (iox2_cif_ps_writer*)calloc(1, sizeof(*w));
    if (w == NULL) {
        free(desc_copy);
        return IOX2_CIF_ERR_NO_MEMORY;
    }
    memcpy(w->name, name_copy, sizeof(name_copy));
    memcpy(w->topic, topic_copy, sizeof(topic_copy));
    fill_iox_type_name(w->type_name, sizeof(w->type_name), type_copy, is_array);
    w->elem_size = elem_size;
    w->elem_align = elem_align;
    w->max_count = max_count;
    w->is_array = is_array;
    w->supports_backpressure = create_bp ? 1 : 0;
    w->be_subscriber_buffer = create_be ? be_subscriber_buffer : 0;
    w->bp_subscriber_buffer = create_bp ? bp_subscriber_buffer : 0;
    w->descriptor = desc_copy;
    compute_value_layout(is_array, elem_size, elem_align, max_count, &w->value_size, &w->value_align);

    st = lock_exclusive();
    if (st != IOX2_CIF_OK) {
        ps_writer_free(w);
        return st;
    }
    st = ensure_node_unlocked(NULL);
    if (st != IOX2_CIF_OK) {
        unlock_exclusive();
        ps_writer_free(w);
        return st;
    }
    if (ps_writer_exists_unlocked(name_copy, topic_copy)) {
        unlock_exclusive();
        ps_writer_free(w);
        return IOX2_CIF_ERR_WRITER_HELD;
    }
    if (find_local_writer(name_copy, topic_copy) != NULL) {
        unlock_exclusive();
        ps_writer_free(w);
        return IOX2_CIF_ERR_SCHEMA_MISMATCH;
    }

    if (create_be) {
        st = ps_chan_create(
            &w->be,
            be_name,
            w->type_name,
            w->value_size,
            w->value_align,
            1,
            0,
            1,
            be_max_subscribers,
            be_subscriber_buffer);
        if (st != IOX2_CIF_OK) {
            unlock_exclusive();
            ps_writer_free(w);
            return st;
        }
    }
    if (create_bp) {
        st = ps_chan_create(
            &w->bp,
            bp_name,
            w->type_name,
            w->value_size,
            w->value_align,
            0,
            1,
            1,
            bp_max_subscribers,
            bp_subscriber_buffer);
        if (st != IOX2_CIF_OK) {
            unlock_exclusive();
            ps_writer_free(w);
            return st;
        }
    }

    w->handle = alloc_handle(IOX2_CIF_KIND_PS_WRITER, w);
    if (w->handle == IOX2_CIF_INVALID_HANDLE) {
        unlock_exclusive();
        ps_writer_free(w);
        return IOX2_CIF_ERR_NO_MEMORY;
    }
    st = meta_upsert(w->name, w->topic, w->type_name, w->descriptor);
    unlock_exclusive();
    if (st != IOX2_CIF_OK) {
        iox2_cif_ps_destroy_writer(w->handle);
        return st;
    }
    st = store_u64(writer, w->handle);
    if (st != IOX2_CIF_OK) {
        iox2_cif_ps_destroy_writer(w->handle);
        return st;
    }
    return IOX2_CIF_OK;
}

static iox2_cif_status ps_open_event(
    iox2_service_name_ptr name_ptr, iox2_port_factory_event_h* event) {
    iox2_service_builder_h sb = iox2_node_service_builder(&g_state.node, NULL, name_ptr);
    iox2_service_builder_event_h evb = iox2_service_builder_event(sb);
    return map_event_open_error(iox2_service_builder_event_open(evb, NULL, event));
}

#define IOX2_CIF_META_SWEEP_GRACE_SEC 30

/* 1 = service exists, 0 = does not exist, -1 = probe failed (keep the file). */
static int service_exists(const char* svc_name, enum iox2_messaging_pattern_e pattern) {
    iox2_service_name_h name_h = NULL;
    iox2_static_config_t details;
    bool does_exist = false;
    int rc = 0;

    if (svc_name == NULL || svc_name[0] == 0 || g_state.node == NULL) {
        return -1;
    }
    rc = iox2_service_name_new(NULL, svc_name, strlen(svc_name), &name_h);
    if (rc != IOX2_OK || name_h == NULL) {
        return -1;
    }
    memset(&details, 0, sizeof(details));
    rc = iox2_service_details(
        iox2_service_type_e_IPC,
        iox2_cast_service_name_ptr(name_h),
        iox2_node_config(&g_state.node),
        pattern,
        &details,
        &does_exist);
    iox2_service_name_drop(name_h);
    if (rc != IOX2_OK) {
        return -1;
    }
    drop_static_config(&details);
    return does_exist ? 1 : 0;
}

static int meta_file_too_new(const char* path) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA info;
    FILETIME now;
    ULARGE_INTEGER n;
    ULARGE_INTEGER f;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &info)) {
        return 1;
    }
    GetSystemTimeAsFileTime(&now);
    n.LowPart = now.dwLowDateTime;
    n.HighPart = now.dwHighDateTime;
    f.LowPart = info.ftLastWriteTime.dwLowDateTime;
    f.HighPart = info.ftLastWriteTime.dwHighDateTime;
    if (n.QuadPart < f.QuadPart) {
        return 1;
    }
    return (n.QuadPart - f.QuadPart) < (ULONGLONG)IOX2_CIF_META_SWEEP_GRACE_SEC * 10000000ULL;
#else
    struct stat st;
    time_t now = 0;
    if (stat(path, &st) != 0) {
        return 1;
    }
    now = time(NULL);
    if (now < st.st_mtime) {
        return 1;
    }
    return (now - st.st_mtime) < (time_t)IOX2_CIF_META_SWEEP_GRACE_SEC;
#endif
}

/* Reads the stored name and topic strings, skipping type names and descriptors. */
static int meta_read_topics(
    const char* path,
    char* name,
    size_t name_cap,
    char (*topics)[IOX2_CIF_TOPIC_CAP],
    uint32_t* count) {
    FILE* f = NULL;
    uint32_t magic = 0;
    uint32_t version = 0;
    uint32_t name_len = 0;
    uint32_t n = 0;
    uint32_t i = 0;

    f = fopen(path, "rb");
    if (f == NULL) {
        return 0;
    }
    if (!meta_read_u32(f, &magic) || magic != IOX2_CIF_META_MAGIC || !meta_read_u32(f, &version)
        || version != IOX2_CIF_META_VERSION || !meta_read_u32(f, &name_len) || name_len == 0
        || name_len >= name_cap || !meta_read_blob(f, name, name_len)) {
        fclose(f);
        return 0;
    }
    name[name_len] = 0;
    if (!meta_read_u32(f, &n) || n > IOX2_CIF_DIR_MAX_TOPICS) {
        fclose(f);
        return 0;
    }
    for (i = 0; i < n; ++i) {
        uint32_t tlen = 0;
        uint32_t ylen = 0;
        uint32_t dlen = 0;
        if (!meta_read_u32(f, &tlen) || !meta_read_u32(f, &ylen) || !meta_read_u32(f, &dlen)
            || tlen == 0 || tlen >= IOX2_CIF_TOPIC_CAP || ylen == 0 || ylen >= IOX2_CIF_TYPE_NAME_CAP
            || dlen >= IOX2_CIF_DESC_MAX || !meta_read_blob(f, topics[i], tlen)) {
            fclose(f);
            return 0;
        }
        topics[i][tlen] = 0;
        if (fseek(f, (long)ylen + (long)dlen, SEEK_CUR) != 0) {
            fclose(f);
            return 0;
        }
    }
    fclose(f);
    *count = n;
    return 1;
}

static void meta_sweep_one(const char* path) {
    char name[IOX2_CIF_NAME_CAP];
    char (*topics)[IOX2_CIF_TOPIC_CAP] = NULL;
    uint32_t count = 0;
    uint32_t i = 0;

    if (meta_file_too_new(path)) {
        return;
    }
    topics = (char (*)[IOX2_CIF_TOPIC_CAP])calloc(IOX2_CIF_DIR_MAX_TOPICS, IOX2_CIF_TOPIC_CAP);
    if (topics == NULL) {
        return;
    }
    if (!meta_read_topics(path, name, sizeof(name), topics, &count)) {
        free(topics);
        return;
    }
    if (service_exists(name, iox2_messaging_pattern_e_BLACKBOARD) != 0) {
        free(topics);
        return;
    }
    for (i = 0; i < count; ++i) {
        char be_name[IOX2_SERVICE_NAME_LENGTH];
        char bp_name[IOX2_SERVICE_NAME_LENGTH];
        if (ps_compose_name(name, topics[i], IOX2_CIF_PS_BE_SUFFIX, be_name, sizeof(be_name)) != IOX2_CIF_OK
            || ps_compose_name(name, topics[i], IOX2_CIF_PS_BP_SUFFIX, bp_name, sizeof(bp_name)) != IOX2_CIF_OK) {
            free(topics);
            return;
        }
        if (service_exists(be_name, iox2_messaging_pattern_e_PUBLISH_SUBSCRIBE) != 0
            || service_exists(bp_name, iox2_messaging_pattern_e_PUBLISH_SUBSCRIBE) != 0
            || service_exists(be_name, iox2_messaging_pattern_e_EVENT) != 0
            || service_exists(bp_name, iox2_messaging_pattern_e_EVENT) != 0) {
            free(topics);
            return;
        }
    }
    free(topics);
    remove(path);
}

static void meta_sweep_orphans(void) {
    static int done = 0;
    if (done || g_state.node == NULL) {
        return;
    }
    done = 1;
#ifdef _WIN32
    {
        char pattern[512];
        WIN32_FIND_DATAA fd;
        HANDLE found = INVALID_HANDLE_VALUE;
        snprintf(pattern, sizeof(pattern), "%s\\*.meta", IOX2_CIF_META_ROOT);
        found = FindFirstFileA(pattern, &fd);
        if (found == INVALID_HANDLE_VALUE) {
            return;
        }
        do {
            char path[512];
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                continue;
            }
            snprintf(path, sizeof(path), "%s\\%s", IOX2_CIF_META_ROOT, fd.cFileName);
            meta_sweep_one(path);
        } while (FindNextFileA(found, &fd));
        FindClose(found);
    }
#else
    {
        DIR* dir = opendir(IOX2_CIF_META_ROOT);
        struct dirent* ent = NULL;
        if (dir == NULL) {
            return;
        }
        while ((ent = readdir(dir)) != NULL) {
            size_t len = strlen(ent->d_name);
            char path[512];
            if (len < 5 || strcmp(ent->d_name + len - 5, ".meta") != 0) {
                continue;
            }
            snprintf(path, sizeof(path), "%s/%s", IOX2_CIF_META_ROOT, ent->d_name);
            meta_sweep_one(path);
        }
        closedir(dir);
    }
#endif
}

static iox2_cif_status ps_wait_open_sub(
    const char* name,
    const char* topic,
    const char* type_name,
    uint32_t elem_size,
    uint32_t elem_align,
    int is_array,
    int use_backpressure,
    uint32_t timeout_ms,
    uint32_t* value_size,
    uint32_t* value_align,
    uint32_t* subscriber_buffer,
    iox2_service_name_h* name_h,
    iox2_port_factory_pub_sub_h* pubsub,
    iox2_port_factory_event_h* event) {
    char want[IOX2_SERVICE_NAME_LENGTH];
    char other[IOX2_SERVICE_NAME_LENGTH];
    uint64_t start = now_ms();
    iox2_cif_status st = IOX2_CIF_OK;

    st = ps_compose_name(
        name,
        topic,
        use_backpressure ? IOX2_CIF_PS_BP_SUFFIX : IOX2_CIF_PS_BE_SUFFIX,
        want,
        sizeof(want));
    if (st != IOX2_CIF_OK) {
        return st;
    }
    st = ps_compose_name(
        name,
        topic,
        use_backpressure ? IOX2_CIF_PS_BE_SUFFIX : IOX2_CIF_PS_BP_SUFFIX,
        other,
        sizeof(other));
    if (st != IOX2_CIF_OK) {
        return st;
    }

    for (;;) {
        int exists = 0;
        uint32_t vs = 0;
        uint32_t va = 0;
        uint32_t sbuf = 0;
        st = ps_probe_pubsub(want, type_name, elem_size, elem_align, is_array, &exists, &vs, &va, &sbuf);
        if (st != IOX2_CIF_OK) {
            return st;
        }
        if (exists) {
            iox2_service_name_h tmp_name = NULL;
            iox2_port_factory_pub_sub_h tmp_svc = NULL;
            int rc = try_open_pubsub(
                want, type_name, vs, va, use_backpressure ? 0 : 1, &tmp_name, &tmp_svc);
            if (rc == IOX2_OK) {
                iox2_cif_status ev = ps_open_event(iox2_cast_service_name_ptr(tmp_name), event);
                if (ev == IOX2_CIF_OK) {
                    *value_size = vs;
                    *value_align = va;
                    if (subscriber_buffer != NULL) {
                        *subscriber_buffer = sbuf;
                    }
                    *name_h = tmp_name;
                    *pubsub = tmp_svc;
                    return IOX2_CIF_OK;
                }
                if (tmp_svc != NULL) {
                    iox2_port_factory_pub_sub_drop(tmp_svc);
                }
                if (tmp_name != NULL) {
                    iox2_service_name_drop(tmp_name);
                }
                if (ev != IOX2_CIF_ERR_TIMEOUT) {
                    return ev;
                }
            } else {
                return map_pubsub_open_error(rc);
            }
        } else {
            int other_exists = 0;
            uint32_t dummy_vs = 0;
            uint32_t dummy_va = 0;
            iox2_cif_status other_st =
                ps_probe_pubsub(
                    other, type_name, elem_size, elem_align, is_array, &other_exists, &dummy_vs, &dummy_va, NULL);
            if (other_st == IOX2_CIF_OK && other_exists) {
                return IOX2_CIF_ERR_UNSUPPORTED;
            }
            if (other_st != IOX2_CIF_OK && other_st != IOX2_CIF_ERR_SCHEMA_MISMATCH) {
                return other_st;
            }
            if (other_st == IOX2_CIF_ERR_SCHEMA_MISMATCH && other_exists) {
                return IOX2_CIF_ERR_UNSUPPORTED;
            }
        }

        if (timeout_ms == 0 || (now_ms() - start) >= timeout_ms) {
            return IOX2_CIF_ERR_TIMEOUT;
        }
        sleep_ms(10);
    }
}

iox2_cif_status iox2_cif_ps_sub_create(
    const char* name,
    const char* topic,
    const char* type_name,
    uint32_t elem_size,
    uint32_t elem_align,
    int32_t is_iox2_cif_array,
    int32_t use_backpressure,
    uint32_t timeout_ms,
    uint64_t* subscriber) {
    iox2_cif_ps_sub* s = NULL;
    char name_copy[IOX2_CIF_NAME_CAP];
    char topic_copy[IOX2_CIF_TOPIC_CAP];
    char type_copy[IOX2_CIF_TYPE_NAME_CAP];
    iox2_port_factory_subscriber_builder_h subb;
    iox2_port_factory_listener_builder_h lisb;
    iox2_waitset_builder_h wsb = NULL;
    int rc = 0;
    iox2_cif_status st = IOX2_CIF_OK;

    if (subscriber == NULL || elem_size == 0
        || (is_iox2_cif_array != 0 && is_iox2_cif_array != 1)
        || (use_backpressure != 0 && use_backpressure != 1)) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    st = copy_cstr_bounded(name_copy, sizeof(name_copy), name);
    if (st != IOX2_CIF_OK || name_copy[0] == 0) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    st = copy_cstr_bounded(topic_copy, sizeof(topic_copy), topic);
    if (st != IOX2_CIF_OK || topic_copy[0] == 0) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    st = copy_cstr_bounded(type_copy, sizeof(type_copy), type_name);
    if (st != IOX2_CIF_OK || type_copy[0] == 0) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    if (strcmp(type_copy, IOX2_CIF_PCAP_TYPE) == 0 || starts_with(type_copy, IOX2_CIF_PCAP_TYPE_PREFIX)) {
        return IOX2_CIF_ERR_TYPE_REJECTED;
    }
    if (elem_align == 0) {
        elem_align = 1;
    }

    s = (iox2_cif_ps_sub*)calloc(1, sizeof(*s));
    if (s == NULL) {
        return IOX2_CIF_ERR_NO_MEMORY;
    }
    s->is_array = is_iox2_cif_array ? 1 : 0;
    s->use_backpressure = use_backpressure ? 1 : 0;
    s->elem_size = elem_size;
    s->elem_align = elem_align;
    s->max_count = 1;
    fill_iox_type_name(s->type_name, sizeof(s->type_name), type_copy, s->is_array);

    st = lock_exclusive();
    if (st != IOX2_CIF_OK) {
        ps_sub_free(s);
        return st;
    }
    st = ensure_node_unlocked(NULL);
    unlock_exclusive();
    if (st != IOX2_CIF_OK) {
        ps_sub_free(s);
        return st;
    }

    st = ps_wait_open_sub(
        name_copy,
        topic_copy,
        s->type_name,
        elem_size,
        elem_align,
        s->is_array,
        s->use_backpressure,
        timeout_ms,
        &s->value_size,
        &s->value_align,
        &s->subscriber_buffer,
        &s->name,
        &s->pubsub,
        &s->event);
    if (st != IOX2_CIF_OK) {
        ps_sub_free(s);
        return st;
    }
    if (s->subscriber_buffer < 1u) {
        ps_sub_free(s);
        return IOX2_CIF_ERR_SCHEMA_MISMATCH;
    }

    if (s->is_array) {
        s->max_count = (s->value_size - IOX2_CIF_SLOT_HEADER_SIZE) / elem_size;
    } else {
        s->max_count = 1;
    }

    subb = iox2_port_factory_pub_sub_subscriber_builder(&s->pubsub, NULL);
    iox2_port_factory_subscriber_builder_set_buffer_size(&subb, s->subscriber_buffer);
    iox2_port_factory_subscriber_builder_set_history_request(&subb, 0);
    rc = iox2_port_factory_subscriber_builder_create(subb, NULL, &s->subscriber);
    if (rc != IOX2_OK) {
        if (rc == (int)iox2_subscriber_create_error_e_BUFFER_SIZE_EXCEEDS_MAX_SUPPORTED_BUFFER_SIZE_OF_SERVICE
            || rc == (int)iox2_subscriber_create_error_e_EXCEEDS_MAX_SUPPORTED_SUBSCRIBERS) {
            ps_sub_free(s);
            return IOX2_CIF_ERR_INVALID_ARGUMENT;
        }
        set_last_detail(iox2_subscriber_create_error_string((enum iox2_subscriber_create_error_e)rc));
        ps_sub_free(s);
        return IOX2_CIF_ERR_INTERNAL;
    }

    lisb = iox2_port_factory_event_listener_builder(&s->event, NULL);
    rc = iox2_port_factory_listener_builder_create(lisb, NULL, &s->listener);
    if (rc != IOX2_OK) {
        set_last_detail("listener create");
        ps_sub_free(s);
        return IOX2_CIF_ERR_INTERNAL;
    }
    iox2_waitset_builder_new(NULL, &wsb);
    rc = iox2_waitset_builder_create(wsb, iox2_service_type_e_IPC, NULL, &s->waitset);
    if (rc != IOX2_OK) {
        set_last_detail("waitset create");
        ps_sub_free(s);
        return IOX2_CIF_ERR_INTERNAL;
    }
    rc = iox2_waitset_attach_notification(
        &s->waitset, iox2_listener_get_file_descriptor(&s->listener), NULL, &s->guard);
    if (rc != IOX2_OK) {
        set_last_detail("waitset attach");
        ps_sub_free(s);
        return IOX2_CIF_ERR_INTERNAL;
    }

    st = lock_exclusive();
    if (st != IOX2_CIF_OK) {
        ps_sub_free(s);
        return st;
    }
    s->handle = alloc_handle(IOX2_CIF_KIND_PS_SUB, s);
    if (s->handle == IOX2_CIF_INVALID_HANDLE) {
        unlock_exclusive();
        ps_sub_free(s);
        return IOX2_CIF_ERR_NO_MEMORY;
    }
    unlock_exclusive();
    st = store_u64(subscriber, s->handle);
    if (st != IOX2_CIF_OK) {
        iox2_cif_ps_destroy_sub(s->handle);
        return st;
    }
    return IOX2_CIF_OK;
}

static iox2_cif_status ps_send_copy(iox2_publisher_h publisher, const void* payload, uint32_t value_size) {
    size_t recipients = 0;
    int rc = iox2_publisher_send_copy(&publisher, payload, value_size, &recipients);
    if (rc == IOX2_OK) {
        return IOX2_CIF_OK;
    }
    if (rc == (int)iox2_send_error_e_UNABLE_TO_DELIVER) {
        return IOX2_CIF_ERR_TIMEOUT;
    }
    set_last_detail(iox2_send_error_string((enum iox2_send_error_e)rc));
    return IOX2_CIF_ERR_INTERNAL;
}

iox2_cif_status iox2_cif_ps_write(
    uint64_t writer, const void* elems, uint32_t count, uint32_t timeout_ms, int32_t* timed_out) {
    iox2_cif_ps_writer* w = NULL;
    void* payload = NULL;
    iox2_publisher_h be_pub = NULL;
    iox2_publisher_h bp_pub = NULL;
    iox2_notifier_h be_nt = NULL;
    iox2_notifier_h bp_nt = NULL;
    uint32_t value_size = 0;
    int supports_bp = 0;
    iox2_cif_status st = IOX2_CIF_OK;

    if (timed_out == NULL) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    st = store_i32(timed_out, 0);
    if (st != IOX2_CIF_OK) {
        return st;
    }
    if (elems == NULL && count != 0) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }

    st = lock_exclusive();
    if (st != IOX2_CIF_OK) {
        return st;
    }
    w = (iox2_cif_ps_writer*)lookup_handle(writer, IOX2_CIF_KIND_PS_WRITER);
    if (w == NULL || w->dying) {
        unlock_exclusive();
        return IOX2_CIF_ERR_INVALID_HANDLE;
    }
    if (count > w->max_count) {
        unlock_exclusive();
        return IOX2_CIF_ERR_ARRAY_TOO_LARGE;
    }
    if (!w->is_array && count != 1) {
        unlock_exclusive();
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    payload = make_ps_payload(w->is_array, w->elem_size, w->value_size, elems, count);
    if (payload == NULL) {
        unlock_exclusive();
        return IOX2_CIF_ERR_NO_MEMORY;
    }
    w->inflight++;
    be_pub = w->be.publisher;
    bp_pub = w->bp.publisher;
    be_nt = w->be.notifier;
    bp_nt = w->bp.notifier;
    value_size = w->value_size;
    supports_bp = w->supports_backpressure;
    unlock_exclusive();

    if (supports_bp && bp_pub != NULL) {
        g_ps_bp_one_try = timeout_ms == 0 ? 1 : 0;
#ifdef _WIN32
        g_ps_bp_deadline = GetTickCount64() + (ULONGLONG)timeout_ms;
#else
        g_ps_bp_deadline_ms = now_ms() + (uint64_t)timeout_ms;
#endif
        st = ps_send_copy(bp_pub, payload, value_size);
        if (st == IOX2_CIF_ERR_TIMEOUT) {
            store_i32(timed_out, 1);
            st = IOX2_CIF_OK;
            free(payload);
            goto release_writer;
        }
        if (st != IOX2_CIF_OK) {
            free(payload);
            goto release_writer;
        }
    }

    if (be_pub != NULL) {
        st = ps_send_copy(be_pub, payload, value_size);
        if (st != IOX2_CIF_OK) {
            free(payload);
            goto release_writer;
        }
    }
    free(payload);
    if (supports_bp && bp_nt != NULL) {
        iox2_notifier_notify(&bp_nt, NULL);
    }
    if (be_nt != NULL) {
        iox2_notifier_notify(&be_nt, NULL);
    }

release_writer:
    {
        iox2_cif_status lk = lock_exclusive();
        if (lk == IOX2_CIF_OK) {
            if (w != NULL && w->inflight > 0) {
                w->inflight--;
                if (w->dying && w->inflight == 0) {
                    ps_writer_free(w);
                }
            }
            unlock_exclusive();
        }
    }
    return st;
}

static iox2_cif_status ps_take_sample(
    iox2_cif_ps_sub* s, void* elems, uint32_t cap, uint32_t* actual, uint32_t* max, int32_t* got) {
    iox2_sample_h sample = NULL;
    const void* payload = NULL;
    c_size_t nbytes = 0;
    int rc = iox2_subscriber_receive(&s->subscriber, NULL, &sample);
    if (rc != IOX2_OK) {
        set_last_detail("subscriber receive");
        return IOX2_CIF_ERR_INTERNAL;
    }
    if (sample == NULL) {
        *got = 0;
        return IOX2_CIF_OK;
    }
    iox2_sample_payload(&sample, &payload, NULL);
    nbytes = iox2_sample_payload_number_of_bytes(&sample);
    {
        iox2_cif_status st = ps_copy_sample(s, payload, (uint32_t)nbytes, elems, cap, actual, max);
        iox2_sample_drop(sample);
        if (st != IOX2_CIF_OK) {
            return st;
        }
    }
    *got = 1;
    return IOX2_CIF_OK;
}

iox2_cif_status iox2_cif_ps_receive(
    uint64_t subscriber,
    void* elems,
    uint32_t cap,
    uint32_t timeout_ms,
    uint32_t* actual,
    uint32_t* max,
    int32_t* timed_out) {
    iox2_cif_ps_sub* s = NULL;
    int32_t got = 0;
    iox2_cif_status st = IOX2_CIF_OK;

    if (timed_out == NULL) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    st = store_i32(timed_out, 0);
    if (st != IOX2_CIF_OK) {
        return st;
    }
    if (actual != NULL) {
        st = store_u32(actual, 0);
        if (st != IOX2_CIF_OK) {
            return st;
        }
    }

    st = lock_exclusive();
    if (st != IOX2_CIF_OK) {
        return st;
    }
    s = (iox2_cif_ps_sub*)lookup_handle(subscriber, IOX2_CIF_KIND_PS_SUB);
    if (s == NULL || s->dying || s->subscriber == NULL) {
        unlock_exclusive();
        return IOX2_CIF_ERR_INVALID_HANDLE;
    }
    s->inflight++;
    unlock_exclusive();

    st = ps_take_sample(s, elems, cap, actual, max, &got);
    if (st == IOX2_CIF_OK && !got && timeout_ms > 0) {
        enum iox2_waitset_run_result_e result = iox2_waitset_run_result_e_ALL_EVENTS_HANDLED;
        uint64_t sec = (uint64_t)(timeout_ms / 1000u);
        uint32_t nsec = (uint32_t)((timeout_ms % 1000u) * 1000000u);
        int rc = iox2_waitset_wait_and_process_once_with_timeout(
            &s->waitset, ps_on_wait, s, sec, nsec, &result);
        if (rc != IOX2_OK && rc != (int)iox2_waitset_run_error_e_NO_ATTACHMENTS) {
            set_last_detail("waitset wait");
            st = IOX2_CIF_ERR_INTERNAL;
        } else {
            st = ps_take_sample(s, elems, cap, actual, max, &got);
        }
    }
    if (st == IOX2_CIF_OK && !got) {
        store_i32(timed_out, 1);
    }

    {
        iox2_cif_status lk = lock_exclusive();
        if (lk == IOX2_CIF_OK) {
            if (s->inflight > 0) {
                s->inflight--;
            }
            if (s->dying && s->inflight == 0) {
                ps_sub_free(s);
            }
            unlock_exclusive();
        }
    }
    return st;
}

iox2_cif_status iox2_cif_ps_has_sample(uint64_t subscriber, int32_t* has) {
    iox2_cif_ps_sub* s = NULL;
    bool ready = false;
    int rc = 0;
    iox2_cif_status st = IOX2_CIF_OK;

    if (has == NULL) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    st = store_i32(has, 0);
    if (st != IOX2_CIF_OK) {
        return st;
    }
    st = lock_shared();
    if (st != IOX2_CIF_OK) {
        return st;
    }
    s = (iox2_cif_ps_sub*)lookup_handle(subscriber, IOX2_CIF_KIND_PS_SUB);
    if (s == NULL || s->dying || s->subscriber == NULL) {
        unlock_shared();
        return IOX2_CIF_ERR_INVALID_HANDLE;
    }
    rc = iox2_subscriber_has_samples(&s->subscriber, &ready);
    unlock_shared();
    if (rc != IOX2_OK) {
        return IOX2_CIF_ERR_INTERNAL;
    }
    return store_i32(has, ready ? 1 : 0);
}

iox2_cif_status iox2_cif_ps_writer_subscriber_count(
    uint64_t writer, uint32_t* best_effort, uint32_t* backpressure) {
    iox2_cif_ps_writer* w = NULL;
    uint32_t be = 0;
    uint32_t bp = 0;
    iox2_cif_status st = lock_shared();
    if (st != IOX2_CIF_OK) {
        return st;
    }
    w = (iox2_cif_ps_writer*)lookup_handle(writer, IOX2_CIF_KIND_PS_WRITER);
    if (w == NULL || w->dying) {
        unlock_shared();
        return IOX2_CIF_ERR_INVALID_HANDLE;
    }
    if (w->be.pubsub != NULL) {
        be = (uint32_t)iox2_port_factory_pub_sub_dynamic_config_number_of_subscribers(&w->be.pubsub);
    }
    if (w->supports_backpressure && w->bp.pubsub != NULL) {
        bp = (uint32_t)iox2_port_factory_pub_sub_dynamic_config_number_of_subscribers(&w->bp.pubsub);
    }
    unlock_shared();
    if (best_effort != NULL) {
        st = store_u32(best_effort, be);
        if (st != IOX2_CIF_OK) {
            return st;
        }
    }
    if (backpressure != NULL) {
        st = store_u32(backpressure, bp);
        if (st != IOX2_CIF_OK) {
            return st;
        }
    }
    return IOX2_CIF_OK;
}

iox2_cif_status iox2_cif_ps_destroy_writer(uint64_t writer) {
    iox2_cif_ps_writer* w = NULL;
    iox2_cif_status st = lock_exclusive();
    if (st != IOX2_CIF_OK) {
        return st;
    }
    w = (iox2_cif_ps_writer*)lookup_handle(writer, IOX2_CIF_KIND_PS_WRITER);
    if (w == NULL) {
        unlock_exclusive();
        return IOX2_CIF_ERR_INVALID_HANDLE;
    }
    destroy_ps_writer_unlocked(w);
    unlock_exclusive();
    return IOX2_CIF_OK;
}

iox2_cif_status iox2_cif_ps_destroy_sub(uint64_t subscriber) {
    iox2_cif_ps_sub* s = NULL;
    iox2_cif_status st = lock_exclusive();
    if (st != IOX2_CIF_OK) {
        return st;
    }
    s = (iox2_cif_ps_sub*)lookup_handle(subscriber, IOX2_CIF_KIND_PS_SUB);
    if (s == NULL) {
        unlock_exclusive();
        return IOX2_CIF_ERR_INVALID_HANDLE;
    }
    destroy_ps_sub_unlocked(s);
    unlock_exclusive();
    return IOX2_CIF_OK;
}

static int is_pcap_type(const char* t) {
    return t != NULL
        && (strcmp(t, IOX2_CIF_PCAP_TYPE) == 0 || starts_with(t, IOX2_CIF_PCAP_TYPE_PREFIX));
}

static iox2_cif_status pcap_format_type(uint32_t linktype, char* dst, size_t dst_sz) {
    int n = 0;
    if (dst == NULL || dst_sz == 0) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    if (linktype == IOX2_CIF_PCAP_LINKTYPE_ANY) {
        n = snprintf(dst, dst_sz, "%s", IOX2_CIF_PCAP_TYPE);
    } else {
        n = snprintf(dst, dst_sz, "%s%u", IOX2_CIF_PCAP_TYPE_PREFIX, (unsigned)linktype);
    }
    if (n <= 0 || (size_t)n >= dst_sz) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    return IOX2_CIF_OK;
}

static uint32_t pcap_parse_linktype(const char* t) {
    char* end = NULL;
    unsigned long v = 0;
    if (t == NULL || strcmp(t, IOX2_CIF_PCAP_TYPE) == 0 || !starts_with(t, IOX2_CIF_PCAP_TYPE_PREFIX)) {
        return IOX2_CIF_PCAP_LINKTYPE_ANY;
    }
    v = strtoul(t + strlen(IOX2_CIF_PCAP_TYPE_PREFIX), &end, 10);
    if (end == NULL || *end != 0 || v > (unsigned long)UINT32_MAX) {
        return IOX2_CIF_PCAP_LINKTYPE_ANY;
    }
    return (uint32_t)v;
}

static int pcap_type_compatible(int match_any, const char* want, const char* have) {
    if (!is_pcap_type(have)) {
        return 0;
    }
    if (match_any) {
        return 1;
    }
    if (want != NULL && strcmp(want, have) == 0) {
        return 1;
    }
    return want != NULL && starts_with(want, IOX2_CIF_PCAP_TYPE_PREFIX)
        && strcmp(have, IOX2_CIF_PCAP_TYPE) == 0;
}

static uint32_t primitive_elem_size(const char* name) {
    if (name == NULL) {
        return 0;
    }
    if (strcmp(name, "i8") == 0 || strcmp(name, "u8") == 0 || strcmp(name, "bool") == 0) {
        return 1;
    }
    if (strcmp(name, "i16") == 0 || strcmp(name, "u16") == 0) {
        return 2;
    }
    if (strcmp(name, "i32") == 0 || strcmp(name, "u32") == 0 || strcmp(name, "f32") == 0) {
        return 4;
    }
    if (strcmp(name, "i64") == 0 || strcmp(name, "u64") == 0 || strcmp(name, "f64") == 0) {
        return 8;
    }
    return 0;
}

static uint32_t ps_info_max_count(const char* type_name, uint32_t value_size) {
    if (is_pcap_type(type_name)) {
        return value_size >= IOX2_CIF_PCAP_HDR_SIZE ? value_size - IOX2_CIF_PCAP_HDR_SIZE : 0;
    }
    if (starts_with(type_name, IOX2_CIF_ARRAY_TYPE_PREFIX)) {
        uint32_t es = primitive_elem_size(type_name + strlen(IOX2_CIF_ARRAY_TYPE_PREFIX));
        if (es == 0 || value_size < IOX2_CIF_SLOT_HEADER_SIZE) {
            return 0;
        }
        return (value_size - IOX2_CIF_SLOT_HEADER_SIZE) / es;
    }
    return 1;
}

typedef struct ps_svc_info {
    int exists;
    char type_name[IOX2_CIF_TYPE_NAME_CAP];
    uint32_t value_size;
    uint32_t value_align;
    uint32_t subscriber_buffer;
} ps_svc_info;

static iox2_cif_status ps_load_svc(const char* svc_name, ps_svc_info* out) {
    iox2_service_name_h name_h = NULL;
    iox2_static_config_t details;
    bool does_exist = false;
    int rc = 0;
    const iox2_type_detail_t* payload = NULL;

    memset(out, 0, sizeof(*out));
    rc = iox2_service_name_new(NULL, svc_name, strlen(svc_name), &name_h);
    if (rc != IOX2_OK) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    memset(&details, 0, sizeof(details));
    rc = iox2_service_details(
        iox2_service_type_e_IPC,
        iox2_cast_service_name_ptr(name_h),
        iox2_node_config(&g_state.node),
        iox2_messaging_pattern_e_PUBLISH_SUBSCRIBE,
        &details,
        &does_exist);
    iox2_service_name_drop(name_h);
    if (rc != IOX2_OK) {
        set_last_detail(iox2_service_details_error_string((enum iox2_service_details_error_e)rc));
        return IOX2_CIF_ERR_INTERNAL;
    }
    if (!does_exist) {
        return IOX2_CIF_OK;
    }
    out->exists = 1;
    if (details.messaging_pattern != iox2_messaging_pattern_e_PUBLISH_SUBSCRIBE) {
        drop_static_config(&details);
        return IOX2_CIF_ERR_SCHEMA_MISMATCH;
    }
    payload = &details.details.publish_subscribe.message_type_details.payload;
    snprintf(out->type_name, sizeof(out->type_name), "%s", payload->type_name);
    out->value_size = (uint32_t)payload->size;
    out->value_align = (uint32_t)payload->alignment;
    out->subscriber_buffer = (uint32_t)details.details.publish_subscribe.subscriber_max_buffer_size;
    drop_static_config(&details);
    return IOX2_CIF_OK;
}

static iox2_cif_status pcap_wait_svc(
    const char* name,
    const char* topic,
    const char* want_type,
    int match_any,
    int use_bp,
    uint32_t timeout_ms,
    ps_svc_info* info) {
    char want[IOX2_SERVICE_NAME_LENGTH];
    char other[IOX2_SERVICE_NAME_LENGTH];
    uint64_t start = now_ms();
    iox2_cif_status st = IOX2_CIF_OK;

    st = ps_compose_name(
        name, topic, use_bp ? IOX2_CIF_PS_BP_SUFFIX : IOX2_CIF_PS_BE_SUFFIX, want, sizeof(want));
    if (st != IOX2_CIF_OK) {
        return st;
    }
    st = ps_compose_name(
        name, topic, use_bp ? IOX2_CIF_PS_BE_SUFFIX : IOX2_CIF_PS_BP_SUFFIX, other, sizeof(other));
    if (st != IOX2_CIF_OK) {
        return st;
    }

    for (;;) {
        st = ps_load_svc(want, info);
        if (st != IOX2_CIF_OK) {
            return st;
        }
        if (info->exists) {
            if (!pcap_type_compatible(match_any, want_type, info->type_name)
                || info->value_size < IOX2_CIF_PCAP_HDR_SIZE) {
                return IOX2_CIF_ERR_SCHEMA_MISMATCH;
            }
            return IOX2_CIF_OK;
        }
        {
            ps_svc_info other_info;
            st = ps_load_svc(other, &other_info);
            if (st != IOX2_CIF_OK) {
                return st;
            }
            if (other_info.exists) {
                return IOX2_CIF_ERR_UNSUPPORTED;
            }
        }
        if (timeout_ms == 0 || (now_ms() - start) >= timeout_ms) {
            return IOX2_CIF_ERR_TIMEOUT;
        }
        sleep_ms(10);
    }
}

static iox2_cif_status pcap_finish_sub(iox2_cif_ps_sub* s) {
    iox2_port_factory_subscriber_builder_h subb;
    iox2_port_factory_listener_builder_h lisb;
    iox2_waitset_builder_h wsb = NULL;
    int rc = 0;

    subb = iox2_port_factory_pub_sub_subscriber_builder(&s->pubsub, NULL);
    iox2_port_factory_subscriber_builder_set_buffer_size(&subb, s->subscriber_buffer);
    iox2_port_factory_subscriber_builder_set_history_request(&subb, 0);
    rc = iox2_port_factory_subscriber_builder_create(subb, NULL, &s->subscriber);
    if (rc != IOX2_OK) {
        if (rc == (int)iox2_subscriber_create_error_e_BUFFER_SIZE_EXCEEDS_MAX_SUPPORTED_BUFFER_SIZE_OF_SERVICE
            || rc == (int)iox2_subscriber_create_error_e_EXCEEDS_MAX_SUPPORTED_SUBSCRIBERS) {
            return IOX2_CIF_ERR_INVALID_ARGUMENT;
        }
        set_last_detail(iox2_subscriber_create_error_string((enum iox2_subscriber_create_error_e)rc));
        return IOX2_CIF_ERR_INTERNAL;
    }
    lisb = iox2_port_factory_event_listener_builder(&s->event, NULL);
    rc = iox2_port_factory_listener_builder_create(lisb, NULL, &s->listener);
    if (rc != IOX2_OK) {
        set_last_detail("listener create");
        return IOX2_CIF_ERR_INTERNAL;
    }
    iox2_waitset_builder_new(NULL, &wsb);
    rc = iox2_waitset_builder_create(wsb, iox2_service_type_e_IPC, NULL, &s->waitset);
    if (rc != IOX2_OK) {
        set_last_detail("waitset create");
        return IOX2_CIF_ERR_INTERNAL;
    }
    rc = iox2_waitset_attach_notification(
        &s->waitset, iox2_listener_get_file_descriptor(&s->listener), NULL, &s->guard);
    if (rc != IOX2_OK) {
        set_last_detail("waitset attach");
        return IOX2_CIF_ERR_INTERNAL;
    }
    return IOX2_CIF_OK;
}

static iox2_cif_status pcap_sub_open_common(
    const char* name,
    const char* topic,
    uint32_t linktype,
    int match_any,
    int32_t use_backpressure,
    uint32_t timeout_ms,
    uint64_t* subscriber) {
    iox2_cif_ps_sub* s = NULL;
    char name_copy[IOX2_CIF_NAME_CAP];
    char topic_copy[IOX2_CIF_TOPIC_CAP];
    char want_type[IOX2_CIF_TYPE_NAME_CAP];
    char svc[IOX2_SERVICE_NAME_LENGTH];
    ps_svc_info info;
    iox2_cif_status st = IOX2_CIF_OK;

    if (subscriber == NULL || (use_backpressure != 0 && use_backpressure != 1)) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    st = copy_cstr_bounded(name_copy, sizeof(name_copy), name);
    if (st != IOX2_CIF_OK || name_copy[0] == 0) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    st = copy_cstr_bounded(topic_copy, sizeof(topic_copy), topic);
    if (st != IOX2_CIF_OK || topic_copy[0] == 0) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    if (!match_any) {
        st = pcap_format_type(linktype, want_type, sizeof(want_type));
        if (st != IOX2_CIF_OK) {
            return st;
        }
    } else {
        want_type[0] = 0;
    }

    s = (iox2_cif_ps_sub*)calloc(1, sizeof(*s));
    if (s == NULL) {
        return IOX2_CIF_ERR_NO_MEMORY;
    }
    s->is_pcap = 1;
    s->use_backpressure = use_backpressure ? 1 : 0;
    s->elem_size = 1;
    s->elem_align = 8;
    s->linktype = match_any ? IOX2_CIF_PCAP_LINKTYPE_ANY : linktype;

    st = lock_exclusive();
    if (st != IOX2_CIF_OK) {
        ps_sub_free(s);
        return st;
    }
    st = ensure_node_unlocked(NULL);
    unlock_exclusive();
    if (st != IOX2_CIF_OK) {
        ps_sub_free(s);
        return st;
    }

    st = pcap_wait_svc(
        name_copy, topic_copy, match_any ? NULL : want_type, match_any, s->use_backpressure, timeout_ms, &info);
    if (st != IOX2_CIF_OK) {
        ps_sub_free(s);
        return st;
    }

    st = ps_compose_name(
        name_copy,
        topic_copy,
        s->use_backpressure ? IOX2_CIF_PS_BP_SUFFIX : IOX2_CIF_PS_BE_SUFFIX,
        svc,
        sizeof(svc));
    if (st != IOX2_CIF_OK) {
        ps_sub_free(s);
        return st;
    }
    {
        int rc = try_open_pubsub(
            svc, info.type_name, info.value_size, info.value_align, s->use_backpressure ? 0 : 1, &s->name, &s->pubsub);
        if (rc != IOX2_OK) {
            ps_sub_free(s);
            return map_pubsub_open_error(rc);
        }
    }
    st = ps_open_event(iox2_cast_service_name_ptr(s->name), &s->event);
    if (st != IOX2_CIF_OK) {
        ps_sub_free(s);
        return st;
    }
    snprintf(s->type_name, sizeof(s->type_name), "%s", info.type_name);
    s->value_size = info.value_size;
    s->value_align = info.value_align;
    s->max_count = info.value_size - IOX2_CIF_PCAP_HDR_SIZE;
    s->subscriber_buffer = info.subscriber_buffer;
    s->linktype = pcap_parse_linktype(info.type_name);
    if (s->subscriber_buffer < 1u) {
        ps_sub_free(s);
        return IOX2_CIF_ERR_SCHEMA_MISMATCH;
    }

    st = pcap_finish_sub(s);
    if (st != IOX2_CIF_OK) {
        ps_sub_free(s);
        return st;
    }

    st = lock_exclusive();
    if (st != IOX2_CIF_OK) {
        ps_sub_free(s);
        return st;
    }
    s->handle = alloc_handle(IOX2_CIF_KIND_PCAP_SUB, s);
    if (s->handle == IOX2_CIF_INVALID_HANDLE) {
        unlock_exclusive();
        ps_sub_free(s);
        return IOX2_CIF_ERR_NO_MEMORY;
    }
    unlock_exclusive();
    st = store_u64(subscriber, s->handle);
    if (st != IOX2_CIF_OK) {
        iox2_cif_pcap_destroy_sub(s->handle);
        return st;
    }
    return IOX2_CIF_OK;
}

iox2_cif_status iox2_cif_pcap_sub_create(
    const char* name,
    const char* topic,
    uint32_t linktype,
    int32_t use_backpressure,
    uint32_t timeout_ms,
    uint64_t* subscriber) {
    if (linktype == IOX2_CIF_PCAP_LINKTYPE_ANY) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    return pcap_sub_open_common(name, topic, linktype, 0, use_backpressure, timeout_ms, subscriber);
}

iox2_cif_status iox2_cif_pcap_sub_open(
    const char* name,
    const char* topic,
    int32_t use_backpressure,
    uint32_t timeout_ms,
    uint64_t* subscriber) {
    return pcap_sub_open_common(
        name, topic, IOX2_CIF_PCAP_LINKTYPE_ANY, 1, use_backpressure, timeout_ms, subscriber);
}

iox2_cif_status iox2_cif_pcap_writer_create(
    const char* name,
    const char* topic,
    uint32_t linktype,
    uint32_t max_pcap_bytes,
    const char* descriptor,
    uint32_t be_max_subscribers,
    uint32_t bp_max_subscribers,
    uint32_t be_subscriber_buffer,
    uint32_t bp_subscriber_buffer,
    uint64_t* writer) {
    iox2_cif_ps_writer* w = NULL;
    char name_copy[IOX2_CIF_NAME_CAP];
    char topic_copy[IOX2_CIF_TOPIC_CAP];
    char be_name[IOX2_SERVICE_NAME_LENGTH];
    char bp_name[IOX2_SERVICE_NAME_LENGTH];
    char* desc_copy = NULL;
    int create_be = 0;
    int create_bp = 0;
    iox2_cif_status st = IOX2_CIF_OK;

    if (writer == NULL || max_pcap_bytes > UINT32_MAX - IOX2_CIF_PCAP_HDR_SIZE) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    st = ps_select_sides(
        be_max_subscribers,
        bp_max_subscribers,
        be_subscriber_buffer,
        bp_subscriber_buffer,
        &create_be,
        &create_bp);
    if (st != IOX2_CIF_OK) {
        return st;
    }
    st = copy_cstr_bounded(name_copy, sizeof(name_copy), name);
    if (st != IOX2_CIF_OK || name_copy[0] == 0) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    st = copy_cstr_bounded(topic_copy, sizeof(topic_copy), topic);
    if (st != IOX2_CIF_OK || topic_copy[0] == 0) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    if (iox2_cif_topic_key(topic_copy) == IOX2_CIF_RESERVED_KEY) {
        return IOX2_CIF_ERR_KEY_RESERVED;
    }
    st = dup_optional_cstr(descriptor, IOX2_CIF_DESC_MAX - 1u, &desc_copy);
    if (st != IOX2_CIF_OK) {
        return st;
    }

    w = (iox2_cif_ps_writer*)calloc(1, sizeof(*w));
    if (w == NULL) {
        free(desc_copy);
        return IOX2_CIF_ERR_NO_MEMORY;
    }
    memcpy(w->name, name_copy, sizeof(name_copy));
    memcpy(w->topic, topic_copy, sizeof(topic_copy));
    st = pcap_format_type(linktype, w->type_name, sizeof(w->type_name));
    if (st != IOX2_CIF_OK) {
        free(desc_copy);
        free(w);
        return st;
    }
    w->is_pcap = 1;
    w->linktype = linktype;
    w->elem_size = 1;
    w->elem_align = 8;
    w->max_count = max_pcap_bytes;
    w->value_align = 8;
    w->value_size = align_up_u32(IOX2_CIF_PCAP_HDR_SIZE + max_pcap_bytes, 8);
    w->supports_backpressure = create_bp ? 1 : 0;
    w->be_subscriber_buffer = create_be ? be_subscriber_buffer : 0;
    w->bp_subscriber_buffer = create_bp ? bp_subscriber_buffer : 0;
    w->descriptor = desc_copy;

    if (create_be) {
        st = ps_compose_name(name_copy, topic_copy, IOX2_CIF_PS_BE_SUFFIX, be_name, sizeof(be_name));
        if (st != IOX2_CIF_OK) {
            ps_writer_free(w);
            return st;
        }
    }
    if (create_bp) {
        st = ps_compose_name(name_copy, topic_copy, IOX2_CIF_PS_BP_SUFFIX, bp_name, sizeof(bp_name));
        if (st != IOX2_CIF_OK) {
            ps_writer_free(w);
            return st;
        }
    }

    st = lock_exclusive();
    if (st != IOX2_CIF_OK) {
        ps_writer_free(w);
        return st;
    }
    st = ensure_node_unlocked(NULL);
    if (st != IOX2_CIF_OK) {
        unlock_exclusive();
        ps_writer_free(w);
        return st;
    }
    if (ps_writer_exists_unlocked(name_copy, topic_copy)) {
        unlock_exclusive();
        ps_writer_free(w);
        return IOX2_CIF_ERR_WRITER_HELD;
    }
    if (find_local_writer(name_copy, topic_copy) != NULL) {
        unlock_exclusive();
        ps_writer_free(w);
        return IOX2_CIF_ERR_SCHEMA_MISMATCH;
    }
    if (create_be) {
        st = ps_chan_create(
            &w->be,
            be_name,
            w->type_name,
            w->value_size,
            w->value_align,
            1,
            0,
            1,
            be_max_subscribers,
            be_subscriber_buffer);
        if (st != IOX2_CIF_OK) {
            unlock_exclusive();
            ps_writer_free(w);
            return st;
        }
    }
    if (create_bp) {
        st = ps_chan_create(
            &w->bp,
            bp_name,
            w->type_name,
            w->value_size,
            w->value_align,
            0,
            1,
            1,
            bp_max_subscribers,
            bp_subscriber_buffer);
        if (st != IOX2_CIF_OK) {
            unlock_exclusive();
            ps_writer_free(w);
            return st;
        }
    }
    w->handle = alloc_handle(IOX2_CIF_KIND_PCAP_WRITER, w);
    if (w->handle == IOX2_CIF_INVALID_HANDLE) {
        unlock_exclusive();
        ps_writer_free(w);
        return IOX2_CIF_ERR_NO_MEMORY;
    }
    st = meta_upsert(w->name, w->topic, w->type_name, w->descriptor);
    unlock_exclusive();
    if (st != IOX2_CIF_OK) {
        iox2_cif_pcap_destroy_writer(w->handle);
        return st;
    }
    st = store_u64(writer, w->handle);
    if (st != IOX2_CIF_OK) {
        iox2_cif_pcap_destroy_writer(w->handle);
        return st;
    }
    return IOX2_CIF_OK;
}

iox2_cif_status iox2_cif_ps_info(
    const char* name,
    const char* topic,
    uint32_t timeout_ms,
    char* type_name,
    uint32_t type_cap,
    uint32_t* type_len,
    uint32_t* max_count,
    uint32_t* be_subscriber_buffer,
    uint32_t* bp_subscriber_buffer,
    uint32_t* value_size) {
    char name_copy[IOX2_CIF_NAME_CAP];
    char topic_copy[IOX2_CIF_TOPIC_CAP];
    char be_name[IOX2_SERVICE_NAME_LENGTH];
    char bp_name[IOX2_SERVICE_NAME_LENGTH];
    uint64_t start = 0;
    iox2_cif_status st = IOX2_CIF_OK;

    st = copy_cstr_bounded(name_copy, sizeof(name_copy), name);
    if (st != IOX2_CIF_OK || name_copy[0] == 0) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    st = copy_cstr_bounded(topic_copy, sizeof(topic_copy), topic);
    if (st != IOX2_CIF_OK || topic_copy[0] == 0) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    st = ps_compose_name(name_copy, topic_copy, IOX2_CIF_PS_BE_SUFFIX, be_name, sizeof(be_name));
    if (st != IOX2_CIF_OK) {
        return st;
    }
    st = ps_compose_name(name_copy, topic_copy, IOX2_CIF_PS_BP_SUFFIX, bp_name, sizeof(bp_name));
    if (st != IOX2_CIF_OK) {
        return st;
    }

    st = lock_exclusive();
    if (st != IOX2_CIF_OK) {
        return st;
    }
    st = ensure_node_unlocked(NULL);
    unlock_exclusive();
    if (st != IOX2_CIF_OK) {
        return st;
    }

    start = now_ms();
    for (;;) {
        ps_svc_info be;
        ps_svc_info bp;
        ps_svc_info* primary = NULL;
        st = ps_load_svc(be_name, &be);
        if (st != IOX2_CIF_OK) {
            return st;
        }
        st = ps_load_svc(bp_name, &bp);
        if (st != IOX2_CIF_OK) {
            return st;
        }
        if (be.exists) {
            primary = &be;
        } else if (bp.exists) {
            primary = &bp;
        }
        if (primary != NULL) {
            uint32_t tlen = (uint32_t)strlen(primary->type_name);
            st = write_cstr_out(type_name, type_cap, type_len, primary->type_name, tlen);
            if (st != IOX2_CIF_OK) {
                return st;
            }
            if (max_count != NULL) {
                st = store_u32(max_count, ps_info_max_count(primary->type_name, primary->value_size));
                if (st != IOX2_CIF_OK) {
                    return st;
                }
            }
            if (be_subscriber_buffer != NULL) {
                st = store_u32(be_subscriber_buffer, be.exists ? be.subscriber_buffer : 0u);
                if (st != IOX2_CIF_OK) {
                    return st;
                }
            }
            if (bp_subscriber_buffer != NULL) {
                st = store_u32(bp_subscriber_buffer, bp.exists ? bp.subscriber_buffer : 0u);
                if (st != IOX2_CIF_OK) {
                    return st;
                }
            }
            if (value_size != NULL) {
                st = store_u32(value_size, primary->value_size);
            }
            return st;
        }
        if (timeout_ms == 0 || (now_ms() - start) >= timeout_ms) {
            return IOX2_CIF_ERR_TIMEOUT;
        }
        sleep_ms(10);
    }
}

iox2_cif_status iox2_cif_pcap_info(
    uint64_t port,
    char* type_name,
    uint32_t type_cap,
    uint32_t* type_len,
    uint32_t* linktype,
    uint32_t* max_pcap_bytes,
    uint32_t* be_subscriber_buffer,
    uint32_t* bp_subscriber_buffer) {
    iox2_cif_ps_writer* w = NULL;
    iox2_cif_ps_sub* s = NULL;
    const char* tn = NULL;
    uint32_t lt = 0;
    uint32_t maxb = 0;
    uint32_t be_buf = 0;
    uint32_t bp_buf = 0;
    iox2_cif_status st = lock_shared();
    if (st != IOX2_CIF_OK) {
        return st;
    }
    w = (iox2_cif_ps_writer*)lookup_handle(port, IOX2_CIF_KIND_PCAP_WRITER);
    s = (iox2_cif_ps_sub*)lookup_handle(port, IOX2_CIF_KIND_PCAP_SUB);
    if (w != NULL && !w->dying) {
        tn = w->type_name;
        lt = w->linktype;
        maxb = w->max_count;
        be_buf = w->be_subscriber_buffer;
        bp_buf = w->bp_subscriber_buffer;
    } else if (s != NULL && !s->dying) {
        tn = s->type_name;
        lt = s->linktype;
        maxb = s->max_count;
        if (s->use_backpressure) {
            bp_buf = s->subscriber_buffer;
        } else {
            be_buf = s->subscriber_buffer;
        }
    } else {
        unlock_shared();
        return IOX2_CIF_ERR_INVALID_HANDLE;
    }
    {
        uint32_t tlen = (uint32_t)strlen(tn);
        st = write_cstr_out(type_name, type_cap, type_len, tn, tlen);
    }
    unlock_shared();
    if (st != IOX2_CIF_OK) {
        return st;
    }
    if (linktype != NULL) {
        st = store_u32(linktype, lt);
        if (st != IOX2_CIF_OK) {
            return st;
        }
    }
    if (max_pcap_bytes != NULL) {
        st = store_u32(max_pcap_bytes, maxb);
        if (st != IOX2_CIF_OK) {
            return st;
        }
    }
    if (be_subscriber_buffer != NULL) {
        st = store_u32(be_subscriber_buffer, be_buf);
        if (st != IOX2_CIF_OK) {
            return st;
        }
    }
    if (bp_subscriber_buffer != NULL) {
        st = store_u32(bp_subscriber_buffer, bp_buf);
    }
    return st;
}

iox2_cif_status iox2_cif_pcap_write(
    uint64_t writer,
    const void* header,
    const void* pcap,
    uint32_t offset,
    uint32_t length,
    uint32_t timeout_ms,
    int32_t* timed_out,
    uint64_t* samples,
    uint64_t* timeouts) {
    iox2_cif_ps_writer* w = NULL;
    uint8_t* payload = NULL;
    iox2_cif_pcap_hdr_t hdr;
    iox2_publisher_h be_pub = NULL;
    iox2_publisher_h bp_pub = NULL;
    iox2_notifier_h be_nt = NULL;
    iox2_notifier_h bp_nt = NULL;
    uint32_t value_size = 0;
    int supports_bp = 0;
    uint64_t stamped = 0;
    int did_timeout = 0;
    int send_ok = 0;
    iox2_cif_status st = IOX2_CIF_OK;

    if (timed_out == NULL || samples == NULL || timeouts == NULL) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    st = store_i32(timed_out, 0);
    if (st != IOX2_CIF_OK) {
        return st;
    }
    if (header == NULL || (pcap == NULL && length != 0) || (length != 0 && offset > UINT32_MAX - length)) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }

    st = lock_exclusive();
    if (st != IOX2_CIF_OK) {
        return st;
    }
    w = (iox2_cif_ps_writer*)lookup_handle(writer, IOX2_CIF_KIND_PCAP_WRITER);
    if (w == NULL || w->dying) {
        unlock_exclusive();
        return IOX2_CIF_ERR_INVALID_HANDLE;
    }
    if (length > w->max_count) {
        store_u64(samples, w->stat_samples);
        store_u64(timeouts, w->stat_timeouts);
        unlock_exclusive();
        return IOX2_CIF_ERR_ARRAY_TOO_LARGE;
    }
    payload = (uint8_t*)calloc(1, w->value_size);
    if (payload == NULL) {
        unlock_exclusive();
        return IOX2_CIF_ERR_NO_MEMORY;
    }
    if (copy_bytes_bounded(&hdr, header, sizeof(hdr)) != IOX2_CIF_OK) {
        free(payload);
        unlock_exclusive();
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    stamped = w->pcap_ctr;
    w->pcap_ctr++;
    hdr.length = (uint64_t)IOX2_CIF_PCAP_HDR_SIZE + (uint64_t)length;
    hdr.ctr = stamped;
    memset(hdr.reserved, 0, sizeof(hdr.reserved));
    memcpy(payload, &hdr, sizeof(hdr));
    if (length > 0) {
        if (copy_bytes_bounded(payload + IOX2_CIF_PCAP_HDR_SIZE, (const uint8_t*)pcap + offset, length)
            != IOX2_CIF_OK) {
            w->pcap_ctr--;
            free(payload);
            unlock_exclusive();
            return IOX2_CIF_ERR_INVALID_ARGUMENT;
        }
    }
    w->inflight++;
    be_pub = w->be.publisher;
    bp_pub = w->bp.publisher;
    be_nt = w->be.notifier;
    bp_nt = w->bp.notifier;
    value_size = w->value_size;
    supports_bp = w->supports_backpressure;
    unlock_exclusive();

    if (supports_bp && bp_pub != NULL) {
        g_ps_bp_one_try = timeout_ms == 0 ? 1 : 0;
#ifdef _WIN32
        g_ps_bp_deadline = GetTickCount64() + (ULONGLONG)timeout_ms;
#else
        g_ps_bp_deadline_ms = now_ms() + (uint64_t)timeout_ms;
#endif
        st = ps_send_copy(bp_pub, payload, value_size);
        if (st == IOX2_CIF_ERR_TIMEOUT) {
            store_i32(timed_out, 1);
            did_timeout = 1;
            st = IOX2_CIF_OK;
            free(payload);
            goto release_writer;
        }
        if (st != IOX2_CIF_OK) {
            free(payload);
            goto release_writer;
        }
    }
    if (be_pub != NULL) {
        st = ps_send_copy(be_pub, payload, value_size);
        if (st != IOX2_CIF_OK) {
            free(payload);
            goto release_writer;
        }
    }
    free(payload);
    send_ok = 1;
    if (supports_bp && bp_nt != NULL) {
        iox2_notifier_notify(&bp_nt, NULL);
    }
    if (be_nt != NULL) {
        iox2_notifier_notify(&be_nt, NULL);
    }

release_writer:
    {
        iox2_cif_status lk = lock_exclusive();
        if (lk == IOX2_CIF_OK) {
            iox2_cif_ps_writer* live = (iox2_cif_ps_writer*)lookup_handle(writer, IOX2_CIF_KIND_PCAP_WRITER);
            if (live != NULL) {
                if (!send_ok) {
                    live->pcap_ctr--;
                }
                if (did_timeout) {
                    live->stat_timeouts++;
                } else if (send_ok && st == IOX2_CIF_OK) {
                    live->stat_samples++;
                    live->stat_last_ctr = stamped;
                }
                store_u64(samples, live->stat_samples);
                store_u64(timeouts, live->stat_timeouts);
                if (live->inflight > 0) {
                    live->inflight--;
                }
                if (live->dying && live->inflight == 0) {
                    ps_writer_free(live);
                }
            }
            unlock_exclusive();
        }
    }
    return st;
}

static void pcap_note_receive_ctr(iox2_cif_ps_sub* s, uint64_t ctr) {
    if (!s->stat_have_last) {
        s->stat_have_last = 1;
        s->stat_last_ctr = ctr;
        s->stat_samples++;
        return;
    }
    {
        uint64_t expected = s->stat_last_ctr + 1u;
        if (ctr != expected) {
            if (ctr == s->stat_last_ctr) {
                s->stat_dropped++;
            } else if (ctr > s->stat_last_ctr) {
                s->stat_dropped += (ctr - expected);
            } else {
                /* backward, including wrap-to-nonzero after UINT64_MAX */
                s->stat_dropped++;
            }
        }
    }
    s->stat_last_ctr = ctr;
    s->stat_samples++;
}

static iox2_cif_status pcap_take_sample(
    iox2_cif_ps_sub* s,
    void* header,
    void* pcap,
    uint32_t cap,
    uint32_t* actual,
    uint32_t* max,
    int32_t* got) {
    iox2_sample_h sample = NULL;
    const void* payload = NULL;
    c_size_t nbytes = 0;
    iox2_cif_pcap_hdr_t hdr;
    uint64_t live = 0;
    uint32_t copy_n = 0;
    int rc = iox2_subscriber_receive(&s->subscriber, NULL, &sample);
    if (rc != IOX2_OK) {
        set_last_detail("subscriber receive");
        return IOX2_CIF_ERR_INTERNAL;
    }
    if (sample == NULL) {
        *got = 0;
        return IOX2_CIF_OK;
    }
    iox2_sample_payload(&sample, &payload, NULL);
    nbytes = iox2_sample_payload_number_of_bytes(&sample);
    if (nbytes < IOX2_CIF_PCAP_HDR_SIZE || payload == NULL) {
        iox2_sample_drop(sample);
        return IOX2_CIF_ERR_INTERNAL;
    }
    memcpy(&hdr, payload, sizeof(hdr));
    if (hdr.length < IOX2_CIF_PCAP_HDR_SIZE) {
        iox2_sample_drop(sample);
        return IOX2_CIF_ERR_INTERNAL;
    }
    live = hdr.length - IOX2_CIF_PCAP_HDR_SIZE;
    if (live > s->max_count) {
        live = s->max_count;
    }
    if (copy_bytes_bounded(header, &hdr, sizeof(hdr)) != IOX2_CIF_OK) {
        iox2_sample_drop(sample);
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    copy_n = (uint32_t)live;
    if (copy_n > cap) {
        copy_n = cap;
    }
    if (copy_n > 0 && pcap != NULL) {
        if (copy_bytes_bounded(pcap, (const uint8_t*)payload + IOX2_CIF_PCAP_HDR_SIZE, copy_n) != IOX2_CIF_OK) {
            iox2_sample_drop(sample);
            return IOX2_CIF_ERR_INVALID_ARGUMENT;
        }
    }
    iox2_sample_drop(sample);
    if (actual != NULL) {
        store_u32(actual, (uint32_t)live);
    }
    if (max != NULL) {
        store_u32(max, s->max_count);
    }
    pcap_note_receive_ctr(s, hdr.ctr);
    *got = 1;
    return IOX2_CIF_OK;
}

iox2_cif_status iox2_cif_pcap_receive(
    uint64_t subscriber,
    void* header,
    void* pcap,
    uint32_t cap,
    uint32_t timeout_ms,
    uint32_t* actual,
    uint32_t* max,
    int32_t* timed_out,
    uint64_t* samples,
    uint64_t* dropped,
    uint64_t* timeouts) {
    iox2_cif_ps_sub* s = NULL;
    int32_t got = 0;
    int did_timeout = 0;
    iox2_cif_status st = IOX2_CIF_OK;

    if (timed_out == NULL || samples == NULL || dropped == NULL || timeouts == NULL || header == NULL) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    st = store_i32(timed_out, 0);
    if (st != IOX2_CIF_OK) {
        return st;
    }
    if (actual != NULL) {
        store_u32(actual, 0);
    }

    st = lock_exclusive();
    if (st != IOX2_CIF_OK) {
        return st;
    }
    s = (iox2_cif_ps_sub*)lookup_handle(subscriber, IOX2_CIF_KIND_PCAP_SUB);
    if (s == NULL || s->dying || s->subscriber == NULL) {
        unlock_exclusive();
        return IOX2_CIF_ERR_INVALID_HANDLE;
    }
    s->inflight++;
    unlock_exclusive();

    st = pcap_take_sample(s, header, pcap, cap, actual, max, &got);
    if (st == IOX2_CIF_OK && !got && timeout_ms > 0) {
        enum iox2_waitset_run_result_e result = iox2_waitset_run_result_e_ALL_EVENTS_HANDLED;
        uint64_t sec = (uint64_t)(timeout_ms / 1000u);
        uint32_t nsec = (uint32_t)((timeout_ms % 1000u) * 1000000u);
        int rc = iox2_waitset_wait_and_process_once_with_timeout(
            &s->waitset, ps_on_wait, s, sec, nsec, &result);
        if (rc != IOX2_OK && rc != (int)iox2_waitset_run_error_e_NO_ATTACHMENTS) {
            set_last_detail("waitset wait");
            st = IOX2_CIF_ERR_INTERNAL;
        } else {
            st = pcap_take_sample(s, header, pcap, cap, actual, max, &got);
        }
    }
    if (st == IOX2_CIF_OK && !got) {
        store_i32(timed_out, 1);
        did_timeout = 1;
    }

    {
        iox2_cif_status lk = lock_exclusive();
        if (lk == IOX2_CIF_OK) {
            iox2_cif_ps_sub* live = (iox2_cif_ps_sub*)lookup_handle(subscriber, IOX2_CIF_KIND_PCAP_SUB);
            if (live != NULL) {
                if (did_timeout) {
                    live->stat_timeouts++;
                }
                store_u64(samples, live->stat_samples);
                store_u64(dropped, live->stat_dropped);
                store_u64(timeouts, live->stat_timeouts);
                if (live->inflight > 0) {
                    live->inflight--;
                }
                if (live->dying && live->inflight == 0) {
                    ps_sub_free(live);
                }
            }
            unlock_exclusive();
        }
    }
    return st;
}

iox2_cif_status iox2_cif_pcap_stats(
    uint64_t subscriber, uint64_t* samples, uint64_t* dropped, uint64_t* timeouts, uint64_t* last_ctr) {
    iox2_cif_ps_sub* s = NULL;
    iox2_cif_status st = lock_shared();
    if (st != IOX2_CIF_OK) {
        return st;
    }
    s = (iox2_cif_ps_sub*)lookup_handle(subscriber, IOX2_CIF_KIND_PCAP_SUB);
    if (s == NULL || s->dying) {
        unlock_shared();
        return IOX2_CIF_ERR_INVALID_HANDLE;
    }
    if (samples != NULL) {
        store_u64(samples, s->stat_samples);
    }
    if (dropped != NULL) {
        store_u64(dropped, s->stat_dropped);
    }
    if (timeouts != NULL) {
        store_u64(timeouts, s->stat_timeouts);
    }
    if (last_ctr != NULL) {
        store_u64(last_ctr, s->stat_last_ctr);
    }
    unlock_shared();
    return IOX2_CIF_OK;
}

iox2_cif_status iox2_cif_pcap_stats_reset(uint64_t subscriber) {
    iox2_cif_ps_sub* s = NULL;
    iox2_cif_status st = lock_exclusive();
    if (st != IOX2_CIF_OK) {
        return st;
    }
    s = (iox2_cif_ps_sub*)lookup_handle(subscriber, IOX2_CIF_KIND_PCAP_SUB);
    if (s == NULL || s->dying) {
        unlock_exclusive();
        return IOX2_CIF_ERR_INVALID_HANDLE;
    }
    s->stat_samples = 0;
    s->stat_dropped = 0;
    s->stat_timeouts = 0;
    s->stat_last_ctr = 0;
    s->stat_have_last = 0;
    unlock_exclusive();
    return IOX2_CIF_OK;
}

iox2_cif_status iox2_cif_pcap_writer_stats(
    uint64_t writer, uint64_t* samples, uint64_t* timeouts, uint64_t* last_ctr) {
    iox2_cif_ps_writer* w = NULL;
    iox2_cif_status st = lock_shared();
    if (st != IOX2_CIF_OK) {
        return st;
    }
    w = (iox2_cif_ps_writer*)lookup_handle(writer, IOX2_CIF_KIND_PCAP_WRITER);
    if (w == NULL || w->dying) {
        unlock_shared();
        return IOX2_CIF_ERR_INVALID_HANDLE;
    }
    if (samples != NULL) {
        store_u64(samples, w->stat_samples);
    }
    if (timeouts != NULL) {
        store_u64(timeouts, w->stat_timeouts);
    }
    if (last_ctr != NULL) {
        store_u64(last_ctr, w->stat_last_ctr);
    }
    unlock_shared();
    return IOX2_CIF_OK;
}

iox2_cif_status iox2_cif_pcap_writer_stats_reset(uint64_t writer) {
    iox2_cif_ps_writer* w = NULL;
    iox2_cif_status st = lock_exclusive();
    if (st != IOX2_CIF_OK) {
        return st;
    }
    w = (iox2_cif_ps_writer*)lookup_handle(writer, IOX2_CIF_KIND_PCAP_WRITER);
    if (w == NULL || w->dying) {
        unlock_exclusive();
        return IOX2_CIF_ERR_INVALID_HANDLE;
    }
    w->stat_samples = 0;
    w->stat_timeouts = 0;
    w->stat_last_ctr = 0;
    unlock_exclusive();
    return IOX2_CIF_OK;
}

iox2_cif_status iox2_cif_pcap_flush(uint64_t subscriber, uint32_t* discarded) {
    iox2_cif_ps_sub* s = NULL;
    uint32_t n = 0;
    iox2_cif_status st = lock_exclusive();
    if (st != IOX2_CIF_OK) {
        return st;
    }
    s = (iox2_cif_ps_sub*)lookup_handle(subscriber, IOX2_CIF_KIND_PCAP_SUB);
    if (s == NULL || s->dying || s->subscriber == NULL) {
        unlock_exclusive();
        return IOX2_CIF_ERR_INVALID_HANDLE;
    }
    s->inflight++;
    unlock_exclusive();

    for (;;) {
        iox2_sample_h sample = NULL;
        const void* payload = NULL;
        c_size_t nbytes = 0;
        int rc = iox2_subscriber_receive(&s->subscriber, NULL, &sample);
        if (rc != IOX2_OK) {
            st = IOX2_CIF_ERR_INTERNAL;
            break;
        }
        if (sample == NULL) {
            break;
        }
        iox2_sample_payload(&sample, &payload, NULL);
        nbytes = iox2_sample_payload_number_of_bytes(&sample);
        if (payload != NULL && nbytes >= IOX2_CIF_PCAP_HDR_SIZE) {
            iox2_cif_pcap_hdr_t hdr;
            memcpy(&hdr, payload, sizeof(hdr));
            s->stat_have_last = 1;
            s->stat_last_ctr = hdr.ctr;
        }
        iox2_sample_drop(sample);
        n++;
    }
    if (s->listener != NULL) {
        uint64_t ev = 0;
        iox2_listener_try_wait(&s->listener, &ev, ignore_event, NULL);
    }

    {
        iox2_cif_status lk = lock_exclusive();
        if (lk == IOX2_CIF_OK) {
            if (s->inflight > 0) {
                s->inflight--;
            }
            if (s->dying && s->inflight == 0) {
                ps_sub_free(s);
            }
            unlock_exclusive();
        }
    }
    if (discarded != NULL && st == IOX2_CIF_OK) {
        st = store_u32(discarded, n);
    }
    return st;
}

iox2_cif_status iox2_cif_pcap_has_sample(uint64_t subscriber, int32_t* has) {
    iox2_cif_ps_sub* s = NULL;
    bool ready = false;
    int rc = 0;
    iox2_cif_status st = IOX2_CIF_OK;
    if (has == NULL) {
        return IOX2_CIF_ERR_INVALID_ARGUMENT;
    }
    st = store_i32(has, 0);
    if (st != IOX2_CIF_OK) {
        return st;
    }
    st = lock_shared();
    if (st != IOX2_CIF_OK) {
        return st;
    }
    s = (iox2_cif_ps_sub*)lookup_handle(subscriber, IOX2_CIF_KIND_PCAP_SUB);
    if (s == NULL || s->dying || s->subscriber == NULL) {
        unlock_shared();
        return IOX2_CIF_ERR_INVALID_HANDLE;
    }
    rc = iox2_subscriber_has_samples(&s->subscriber, &ready);
    unlock_shared();
    if (rc != IOX2_OK) {
        return IOX2_CIF_ERR_INTERNAL;
    }
    return store_i32(has, ready ? 1 : 0);
}

iox2_cif_status iox2_cif_pcap_writer_subscriber_count(
    uint64_t writer, uint32_t* best_effort, uint32_t* backpressure) {
    iox2_cif_ps_writer* w = NULL;
    uint32_t be = 0;
    uint32_t bp = 0;
    iox2_cif_status st = lock_shared();
    if (st != IOX2_CIF_OK) {
        return st;
    }
    w = (iox2_cif_ps_writer*)lookup_handle(writer, IOX2_CIF_KIND_PCAP_WRITER);
    if (w == NULL || w->dying) {
        unlock_shared();
        return IOX2_CIF_ERR_INVALID_HANDLE;
    }
    if (w->be.pubsub != NULL) {
        be = (uint32_t)iox2_port_factory_pub_sub_dynamic_config_number_of_subscribers(&w->be.pubsub);
    }
    if (w->supports_backpressure && w->bp.pubsub != NULL) {
        bp = (uint32_t)iox2_port_factory_pub_sub_dynamic_config_number_of_subscribers(&w->bp.pubsub);
    }
    unlock_shared();
    if (best_effort != NULL) {
        st = store_u32(best_effort, be);
        if (st != IOX2_CIF_OK) {
            return st;
        }
    }
    if (backpressure != NULL) {
        st = store_u32(backpressure, bp);
    }
    return st;
}

iox2_cif_status iox2_cif_pcap_destroy_writer(uint64_t writer) {
    iox2_cif_ps_writer* w = NULL;
    iox2_cif_status st = lock_exclusive();
    if (st != IOX2_CIF_OK) {
        return st;
    }
    w = (iox2_cif_ps_writer*)lookup_handle(writer, IOX2_CIF_KIND_PCAP_WRITER);
    if (w == NULL) {
        unlock_exclusive();
        return IOX2_CIF_ERR_INVALID_HANDLE;
    }
    destroy_ps_writer_unlocked(w);
    unlock_exclusive();
    return IOX2_CIF_OK;
}

iox2_cif_status iox2_cif_pcap_destroy_sub(uint64_t subscriber) {
    iox2_cif_ps_sub* s = NULL;
    iox2_cif_status st = lock_exclusive();
    if (st != IOX2_CIF_OK) {
        return st;
    }
    s = (iox2_cif_ps_sub*)lookup_handle(subscriber, IOX2_CIF_KIND_PCAP_SUB);
    if (s == NULL) {
        unlock_exclusive();
        return IOX2_CIF_ERR_INVALID_HANDLE;
    }
    destroy_ps_sub_unlocked(s);
    unlock_exclusive();
    return IOX2_CIF_OK;
}

#ifdef _WIN32
BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    HMODULE self = NULL;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        /* No fopen/logging here — loader lock. */
        if (GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                (LPCWSTR)(const void*)&pin_library,
                &self)) {
            InterlockedExchange(&g_pinned, 1);
        }
        return TRUE;
    }
    if (reason == DLL_PROCESS_DETACH) {
        (void)reserved;
        g_state.running = 0;
        /* Do not join the wait thread or drop iceoryx2 under the loader lock. */
        return TRUE;
    }
    (void)inst;
    return TRUE;
}
#endif

