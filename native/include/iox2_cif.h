/* SPDX-License-Identifier: Apache-2.0
 *
 * iceoryx2 CIF wrapper — public C ABI (blackboard + publish-subscribe).
 * CIF = Composable Instrumentation Framework.
 *
 * cdecl, generation-counted uint64 handles. LabVIEW CLFN notes: 64-bit C
 * calling convention; I32 flags (not 8-bit Boolean); first load on the UI
 * thread. See docs/LABVIEW_PROTOTYPE.md.
 */

#ifndef IOX2_CIF_H
#define IOX2_CIF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* DLL export / calling convention                                              */
/*                                                                            */
/* Calling convention = C (cdecl) on 64-bit Windows.                          */
/* -------------------------------------------------------------------------- */

#if defined(_WIN32)
#    ifdef IOX2_CIF_EXPORTS
#        define IOX2_CIF_API __declspec(dllexport)
#    else
#        define IOX2_CIF_API __declspec(dllimport)
#    endif
#    define IOX2_CIF_CALL __cdecl
#else
#    define IOX2_CIF_API __attribute__((visibility("default")))
#    define IOX2_CIF_CALL
#endif

#define IOX2_CIF_EXPORT IOX2_CIF_API IOX2_CIF_CALL

/* -------------------------------------------------------------------------- */
/* Constants                                                                    */
/* -------------------------------------------------------------------------- */

/** Wrapper ABI marker (optional iceoryx2 service attribute `iox2_cif.abi`). */
#define IOX2_CIF_ABI_VERSION 1u

/** Invalid / never-issued handle. Also used as the reserved blackboard key. */
#define IOX2_CIF_INVALID_HANDLE UINT64_C(0)
#define IOX2_CIF_RESERVED_KEY UINT64_C(0)

/** FNV-1a 64 (DESIGN.md Topic key ABI). Hash UTF-8 topic bytes, no NUL. */
#define IOX2_CIF_FNV1A64_OFFSET UINT64_C(0xcbf29ce484222325)
#define IOX2_CIF_FNV1A64_PRIME UINT64_C(0x100000001b3)

/** Prefix for 1D-array iceoryx2 type_name: `iox2_cif.array.<elem_type_name>`. */
#define IOX2_CIF_ARRAY_TYPE_PREFIX "iox2_cif.array."

/** CIF-pcap iceoryx2 type: universal `iox2_cif.pcap`, typed `iox2_cif.pcap.<linktype>`. */
#define IOX2_CIF_PCAP_TYPE "iox2_cif.pcap"
#define IOX2_CIF_PCAP_TYPE_PREFIX "iox2_cif.pcap."
#define IOX2_CIF_PCAP_HDR_SIZE 48u
#define IOX2_CIF_PCAP_CLOCK_TAI 0u
/** Writer create / info: no LinkType. Typed subscribers may still connect. */
#define IOX2_CIF_PCAP_LINKTYPE_ANY UINT32_MAX

/** Primitive type_name strings (clusters use a caller-supplied name). */
#define IOX2_CIF_TYPE_I8 "i8"
#define IOX2_CIF_TYPE_I16 "i16"
#define IOX2_CIF_TYPE_I32 "i32"
#define IOX2_CIF_TYPE_I64 "i64"
#define IOX2_CIF_TYPE_U8 "u8"
#define IOX2_CIF_TYPE_U16 "u16"
#define IOX2_CIF_TYPE_U32 "u32"
#define IOX2_CIF_TYPE_U64 "u64"
#define IOX2_CIF_TYPE_F32 "f32" /* LabVIEW SGL */
#define IOX2_CIF_TYPE_F64 "f64" /* LabVIEW DBL */
#define IOX2_CIF_TYPE_BOOL "bool" /* 1 byte, 0 or 1; distinct from u8 */

/**
 * Array slot: 8-byte header then `max_count * elem_size` payload bytes.
 * iceoryx2 value size  = 8 + max_count * elem_size  (then aligned)
 * iceoryx2 value align = max(8, elem_align)
 * Only payload[0, count) is valid (`count` is elements, not bytes).
 */
#define IOX2_CIF_SLOT_HEADER_SIZE 8u /* bytes */

/*
 * Units used throughout this header (names do not encode them):
 *
 *   bytes        — elem_size, elem_align, buf_len, out_len, *_bytes, slot header
 *   elements     — count, cap, actual, max, max_count, initial_count
 *                  (one LabVIEW scalar, cluster, or array cell — not a byte length)
 *   reader ports — max_readers, readers_remaining
 *   milliseconds — timeout_ms
 *
 * `iox2_cif_bb_read` / `write` use elements.
 * `iox2_cif_bb_read_raw` / `iox2_cif_bb_read_raw_at` use bytes.
 * `iox2_cif_pcap_write` / `receive` length / actual / max / cap are pcap-record
 * bytes (not including the 48-byte CIF header).
 * `iox2_cif_pcap_write_raw` / `receive_raw` length / actual / max / cap are
 * full sample bytes (48-byte CIF header + records).
 * `iox2_cif_status_text` / `iox2_cif_bb_topic_info` buf_len / out_len are bytes
 * (out_len is strlen, not including the terminating NUL).
 */

/* -------------------------------------------------------------------------- */
/* Status codes                                                                 */
/*                                                                            */
/* Status range 8100–8199 (fits a LabVIEW error cluster). 0 is success.       */
/* -------------------------------------------------------------------------- */

typedef enum iox2_cif_status {
    IOX2_CIF_OK = 0,

    IOX2_CIF_ERR_BASE = 8100,
    IOX2_CIF_ERR_INVALID_ARGUMENT = 8100, /* NULL, empty name, max_count < 1 elements, … */
    IOX2_CIF_ERR_INVALID_HANDLE = 8101,   /* stale, destroyed, or never issued */
    IOX2_CIF_ERR_ALREADY_STARTED = 8102,  /* Writer Create after Start */
    IOX2_CIF_ERR_NOT_STARTED = 8103,      /* Write before Start */
    IOX2_CIF_ERR_DUPLICATE_TOPIC = 8104,  /* same topic staged twice */
    IOX2_CIF_ERR_WRITER_HELD = 8105,      /* reopen, writer slot still claimed */
    IOX2_CIF_ERR_SCHEMA_MISMATCH = 8106,  /* topic missing, or type / elem_size(bytes) / max_count(elements) differ */
    IOX2_CIF_ERR_TOPIC_NOT_FOUND = 8107,  /* reader: no such key on the service */
    IOX2_CIF_ERR_ARRAY_TOO_LARGE = 8108,  /* write count(elements) > max_count(elements); no truncate */
    IOX2_CIF_ERR_TIMEOUT = 8109,          /* reader create: service did not appear */
    IOX2_CIF_ERR_TYPE_REJECTED = 8110,    /* handles, strings, 2D+, nested arrays, … */
    IOX2_CIF_ERR_KEY_RESERVED = 8111,     /* topic hashes to key 0 */
    IOX2_CIF_ERR_KEY_COLLISION = 8112,    /* two staged topics share an FNV-1a key */
    IOX2_CIF_ERR_NO_MEMORY = 8113,
    IOX2_CIF_ERR_SERVICE_GONE = 8114,     /* iceoryx2 service disappeared under us */
    IOX2_CIF_ERR_UNSUPPORTED = 8115,      /* e.g. backpressure subscriber on a BE-only writer */
    IOX2_CIF_ERR_INTERNAL = 8116,         /* iceoryx2 or wrapper failure */
    IOX2_CIF_ERR_LOCK_TIMEOUT = 8117,     /* wrapper lock not acquired (stuck holder) */
    IOX2_CIF_ERR_MAX_SUBSCRIBERS = 8118,  /* subscriber create: max ports already attached */

    IOX2_CIF_ERR_MAX = 8199
} iox2_cif_status;

/** Opaque generation-counted refnums (LabVIEW: Unsigned 64-bit Integer). */
typedef uint64_t iox2_cif_bb_h;
typedef uint64_t iox2_cif_writer_h;
typedef uint64_t iox2_cif_reader_h;
typedef uint64_t iox2_cif_ps_writer_h;
typedef uint64_t iox2_cif_ps_sub_h;
typedef uint64_t iox2_cif_pcap_writer_h;
typedef uint64_t iox2_cif_pcap_sub_h;

/* -------------------------------------------------------------------------- */
/* Shared-memory layouts (cross-language)                                       */
/* -------------------------------------------------------------------------- */

#pragma pack(push, 8)

/**
 * Prefix of every 1D-array blackboard value.
 *
 *     [ iox2_cif_slot_t | uint8_t payload[max_count * elem_size] ]
 *
 * Scalars and clusters do not use this header; the iceoryx2 value is the
 * element itself (max_count = 1).
 */
typedef struct iox2_cif_slot {
    uint32_t count;    /* live length in elements, 0 .. max_count; not bytes */
    uint32_t reserved; /* write 0 */
} iox2_cif_slot_t;

/**
 * CIF pub/sub sample prefix (48 bytes), then a pcap record stream (no global header).
 * clock_id is application-defined; 0 means TAI by convention for t0 / t_out.
 */
typedef struct iox2_cif_pcap_hdr {
    uint64_t length;   /* full sample bytes, including this header */
    int64_t t0;        /* creation ns, clock_id */
    int64_t t_out;     /* send/consume-by ns, clock_id */
    int64_t t_tx;      /* published-into-this-queue ns TAI (caller-supplied) */
    uint64_t ctr;      /* per-writer publish count, wraps; wrapper stamps on send */
    uint8_t clock_id;  /* any u8; 0 = TAI by convention */
    uint8_t reserved[7];
} iox2_cif_pcap_hdr_t;

/**
 * Optional key-0 directory entry (DESIGN.md open item 1).
 * Filled at first Start; subset reopen does not shrink the table.
 * Primary discovery remains: hash the known topic string.
 */
#define IOX2_CIF_DIR_TOPIC_MAX 128 /* topic[] capacity in bytes (chars), including NUL */
#define IOX2_CIF_DIR_TYPE_MAX 64   /* type_name[] capacity in bytes (chars), including NUL */
/** Safety stop when walking a descriptor C string (not a reserved allocation). */
#define IOX2_CIF_DESC_MAX (16u * 1024u * 1024u)

typedef struct iox2_cif_dir_header {
    uint32_t abi;            /* IOX2_CIF_ABI_VERSION */
    uint32_t entry_count;    /* number of topic entries (not bytes) */
    uint32_t entry_capacity; /* max topic entries, frozen at first create (not bytes) */
    uint32_t reserved;
} iox2_cif_dir_header_t;

typedef struct iox2_cif_dir_entry {
    uint64_t key;
    uint32_t elem_size;  /* bytes per element */
    uint32_t elem_align; /* alignment in bytes */
    uint32_t max_count;  /* capacity in elements, not bytes */
    uint32_t flags;      /* bit 0: is_array */
    char type_name[IOX2_CIF_DIR_TYPE_MAX];
    char topic[IOX2_CIF_DIR_TOPIC_MAX];
} iox2_cif_dir_entry_t;

#pragma pack(pop)

/* -------------------------------------------------------------------------- */
/* Process-wide node (optional; otherwise lazy on first Start / Reader Create) */
/*                                                                            */
/* Open item 4. Safe to omit: first Start or Reader Create creates an IPC     */
/* threadsafe Node and a background node.wait() thread.                       */
/* -------------------------------------------------------------------------- */

/**
 * Create the process Node if it does not exist.
 *
 * @param node_name  UTF-8, may be NULL for a default name.
 */
IOX2_CIF_API iox2_cif_status IOX2_CIF_CALL iox2_cif_init(const char* node_name);

/**
 * Drop the process Node after all blackboards / readers are destroyed.
 * Call this from LabVIEW before closing the VI if you want iceoryx2 SHM torn
 * down promptly. Closing a VI without this is safe: the DLL stays mapped until
 * LabVIEW.exe exits (it is pinned so unload cannot kill the wait thread).
 */
IOX2_CIF_API iox2_cif_status IOX2_CIF_CALL iox2_cif_shutdown(void);

/**
 * Human-readable text for an error code.
 *
 * @param buf      Destination C string, or NULL to query length only.
 * @param buf_len  Capacity of buf in bytes, including room for the terminating NUL.
 *                 Ignored when buf is NULL. 0 means do not write to buf.
 * @param out_len  Optional. strlen of the full message in bytes, not including NUL.
 *                 A 256-byte buf always fits. Allocate out_len+1 if sizing from a probe.
 */
IOX2_CIF_API iox2_cif_status IOX2_CIF_CALL
iox2_cif_status_text(iox2_cif_status status, char* buf, uint32_t buf_len /* bytes */, uint32_t* out_len /* strlen, not including NUL */);

/**
 * FNV-1a 64 of UTF-8 topic bytes (no terminating NUL).
 * Case-sensitive. A result of 0 is reserved — Writer Create / Start must fail.
 */
IOX2_CIF_API uint64_t IOX2_CIF_CALL iox2_cif_topic_key(const char* topic);

/* -------------------------------------------------------------------------- */
/* Stage (no iceoryx2)                                                          */
/* -------------------------------------------------------------------------- */

/**
 * Allocate a local blackboard staging object. Does not create an iceoryx2
 * service. `max_readers` is applied only on first create; reopen ignores it.
 *
 * @param max_readers  Max concurrent Reader ports (not payload elements, not bytes).
 *                     U32 Pass Value, 1..4096.
 *
 * CLFN: name = C String Pointer; max_readers = U32 Value; bb = U64, Pass Pointer.
 */
IOX2_CIF_API iox2_cif_status IOX2_CIF_CALL
iox2_cif_bb_create(const char* name, uint32_t max_readers /* reader ports */, uint64_t* bb);

/**
 * Stage a topic on `bb`. Duplicate topic names fail. Must be called before
 * Start; after Start this returns IOX2_CIF_ERR_ALREADY_STARTED.
 *
 * @param type_name      Primitive names from the IOX2_CIF_TYPE_* macros.
 *                       Required for clusters (e.g. "RobotPose").
 *                       For arrays the wrapper stores
 *                       `iox2_cif.array.<elem_type_name>` as the iceoryx2 type.
 * @param elem_size      Size of one element in bytes (LabVIEW cluster size for clusters).
 * @param elem_align     Alignment of one element in bytes.
 * @param max_count      Capacity in elements, not bytes. Required >= 1 for arrays,
 *                       and >= initial_count. Ignored for scalars/clusters (stored as 1).
 * @param initial        Pointer to initial_count elements (Adapt to Type).
 *                       Buffer must be initial_count * elem_size bytes.
 * @param initial_count  Number of elements at initial, not bytes.
 *                       1 for scalars/clusters; live array length for arrays.
 * @param descriptor     Optional interface descriptor (JSON). C String Pointer.
 *                       Stored on this writer in the wrapper (not in iceoryx2).
 *                       NULL or "" stores an empty string.
 *                       Scalar readers ignore it and convert from the iceoryx2
 *                       type name. Cluster field readers (`topic.field`) require
 *                       it and parse docs/IDL Descriptor.schema.json.
 *                       Heap/file store the real length. A string with no NUL
 *                       within IOX2_CIF_DESC_MAX bytes → 8100.
 * @param writer         Out: generation-counted writer refnum.
 */
IOX2_CIF_API iox2_cif_status IOX2_CIF_CALL iox2_cif_bb_writer_create(
    uint64_t bb,
    const char* topic,
    const char* type_name,
    uint32_t elem_size,      /* bytes per element */
    uint32_t elem_align,     /* bytes */
    uint32_t max_count,      /* elements, not bytes */
    const void* initial,
    uint32_t initial_count,  /* elements, not bytes */
    const char* descriptor,  /* JSON, may be NULL or empty */
    uint64_t* writer);

/* -------------------------------------------------------------------------- */
/* Start / readers                                                              */
/* -------------------------------------------------------------------------- */

/**
 * First iceoryx2 call for this blackboard.
 * - No service: create, add every staged topic, take Writer, write initials.
 * - Service exists, staged topics match (extra keys OK), writer slot empty:
 *   open, claim Writer, bind staged topics only. Do not rewrite values.
 * - Writer slot held, or any staged topic missing / type mismatch: error.
 *   On bind failure the Writer is dropped so the slot is not left claimed.
 */
IOX2_CIF_API iox2_cif_status IOX2_CIF_CALL iox2_cif_bb_start(uint64_t bb);

/**
 * Open a typed reader by blackboard name + topic. Does not use the staging
 * ref. Other processes call this the same way. Counts against max_readers
 * (reader ports, not payload elements).
 *
 * `is_iox2_cif_array` is only for this wrapper's array ABI (`iox2_cif.array.*`
 * plus the 8-byte slot header). It is not "is any array".
 *
 * - Non-zero: LabVIEW/iox2_cif writer published a 1D array. Pass the **element**
 *   `type_name` (`i32`, `RobotPose`), element `elem_size` / `elem_align`, and
 *   `max_count` is taken from the topic directory. The wrapper prefixes
 *   `iox2_cif.array.` onto `type_name` before the iceoryx2 bind.
 * - Zero: bind exactly `type_name` + `elem_size` + `elem_align` when that entry
 *   exists. Use this for scalars, clusters, and arrays from other tools. In
 *   that case `elem_size` / `elem_align` are the publisher's **value** size and
 *   alignment (the whole iceoryx2 entry — e.g. a fixed `[i32; 32]` is 128 / 4),
 *   not our slot layout.
 *   Standard scalars (`bool`, `i8`..`i64`, `u8`..`u64`, `f32`, `f64`) may name
 *   a different standard scalar than the publisher. The entry is opened as the
 *   published iceoryx2 type (no JSON required) and `iox2_cif_bb_read` converts.
 *   `elem_size` / `elem_align` are then the subscriber type's natural size.
 *   A topic string `topic.field` reads one cluster element. `field` is a
 *   `struct_elems` name from the writer's IDL JSON, which is required. The
 *   JSON `type` is the conversion source. This fails if that topic is an array.
 *
 * LabVIEW: `is_iox2_cif_array` is I32 Value, 0 or 1 (do not use 8-bit Boolean).
 *
 * @param timeout_ms  Wait for the service/entry to appear, in milliseconds; 0 = one shot.
 */
IOX2_CIF_API iox2_cif_status IOX2_CIF_CALL iox2_cif_bb_reader_create(
    const char* bb_name,
    const char* topic,
    const char* type_name,
    uint32_t elem_size,
    uint32_t elem_align,
    int32_t is_iox2_cif_array, /* 1 = iox2_cif.array.* only; 0 = bind as-is */
    uint32_t timeout_ms,      /* milliseconds */
    uint64_t* reader);

/**
 * Open a raw reader. Discovers iceoryx2 type details from the existing entry
 * (and/or key-0 directory). No LabVIEW type check.
 */
IOX2_CIF_API iox2_cif_status IOX2_CIF_CALL iox2_cif_bb_reader_create_raw(
    const char* bb_name, const char* topic, uint32_t timeout_ms, uint64_t* reader);

/**
 * Look up a topic's type_name and interface descriptor.
 *
 * Reads the wrapper sidecar (not iceoryx2). Any process on this machine can
 * call this after Writer Create in the publisher process. The sidecar lives
 * under C:\\Temp\\iox2_cif\\meta (Windows) or /tmp/iox2_cif/meta (Linux).
 *
 * `type_name` is the stored iceoryx2 type (`i32`, `iox2_cif.array.f64`, …).
 * `descriptor` is the JSON string from Writer Create (may be empty).
 *
 * Same buffer convention as `iox2_cif_status_text`: cap 0 / NULL buf is a
 * length probe; `*_len` is strlen without NUL. Size `descriptor` from `desc_len`.
 * A type buffer of IOX2_CIF_DIR_TYPE_MAX always fits.
 *
 * @param timeout_ms  Wait for the sidecar entry, in milliseconds; 0 = one shot.
 */
IOX2_CIF_API iox2_cif_status IOX2_CIF_CALL iox2_cif_bb_topic_info(
    const char* bb_name,
    const char* topic,
    uint32_t timeout_ms,       /* milliseconds */
    char* type_name,           /* out C string, or NULL to probe */
    uint32_t type_cap,         /* bytes, including NUL */
    uint32_t* type_len,        /* out: strlen, not including NUL */
    char* descriptor,          /* out C string, or NULL to probe */
    uint32_t desc_cap,         /* bytes, including NUL */
    uint32_t* desc_len         /* out: strlen, not including NUL */
);

/* -------------------------------------------------------------------------- */
/* Hot path — pointer + element count                                           */
/*                                                                            */
/* Mutex-free after table load. Write uses iceoryx2 update_with_copy.         */
/*                                                                            */
/* count / cap / actual / max are elements, never bytes.                      */
/* Byte length of the buffer is that value times elem_size.                   */
/*                                                                            */
/* Scalars / clusters: elems points at the value, count = 1 element.          */
/* Arrays: elems points at element [0], count = live length in elements.      */
/* LabVIEW CLFN: Adapt to Type (data pointer) + U32 count (elements).         */
/* -------------------------------------------------------------------------- */

/**
 * Copy `count` elements into the inactive slot and publish.
 *
 * @param elems  Pointer to count elements. Buffer is count * elem_size bytes.
 * @param count  Number of elements to publish, not bytes. Scalars/clusters: 1.
 *
 * count > max_count → IOX2_CIF_ERR_ARRAY_TOO_LARGE, no write, no truncate.
 * Destroyed / invalidated writer → IOX2_CIF_ERR_INVALID_HANDLE.
 */
IOX2_CIF_API iox2_cif_status IOX2_CIF_CALL
iox2_cif_bb_write(uint64_t writer, const void* elems, uint32_t count /* elements */);

/**
 * Copy up to `cap` elements into `elems`.
 *
 * @param elems   Destination for elements, or NULL to probe sizes only.
 *                When non-NULL, buffer must be at least cap * elem_size bytes.
 * @param cap     Capacity of elems in elements, not bytes. 0 with elems NULL is a size probe.
 * @param actual  Out: live length in elements (scalars/clusters report 1). Not bytes.
 * @param max     Out: topic max_count in elements (scalars/clusters report 1). Not bytes.
 *
 * When copying, cap must be >= *actual and `elems` must point at that many
 * elements (LabVIEW: Adapt to Type, Array Data Pointer — not Handles by Value).
 * cap == 0 is a size probe even if elems is non-NULL.
 *
 * A converting scalar reader, or a `topic.field` reader, writes one element in
 * the subscriber type. Same-type whole values are copied. Raw reads are not
 * converted and are not sliced to a field.
 */
IOX2_CIF_API iox2_cif_status IOX2_CIF_CALL iox2_cif_bb_read(
    uint64_t reader,
    void* elems,
    uint32_t cap,     /* elements, not bytes */
    uint32_t* actual, /* out: live elements */
    uint32_t* max     /* out: max_count elements */
);

/**
 * Copy the used payload as raw bytes (no LabVIEW type check). Typed or raw readers.
 *
 * Array: actual_bytes is 8-byte slot header + live_count * elem_size (not the unused tail).
 * Scalar/cluster: actual_bytes is elem_size.
 *
 * @param bytes         Destination byte buffer. Ignored when cap_bytes is 0.
 * @param cap_bytes     Capacity of bytes, in bytes (not elements).
 *                      0 = size probe: fill actual_bytes / max_bytes, do not copy.
 *                      LabVIEW still passes a non-NULL bytes pointer on a probe.
 * @param actual_bytes  Out: used payload size in bytes.
 * @param max_bytes     Out: iceoryx2 value size in bytes (array: full slot including unused tail).
 */
IOX2_CIF_API iox2_cif_status IOX2_CIF_CALL iox2_cif_bb_read_raw(
    uint64_t reader,
    void* bytes,
    uint32_t cap_bytes,     /* bytes */
    uint32_t* actual_bytes, /* out: used bytes */
    uint32_t* max_bytes     /* out: value size in bytes */
);

/**
 * Copy a byte slice of the used payload (same buffer as `iox2_cif_bb_read_raw`).
 *
 * Offset 0 is the first used byte: array slot header then live elements;
 * scalar/cluster is the value itself. Unused array tail is not readable.
 *
 * Copies min(cap_bytes, used - offset) bytes. If offset is past the used
 * payload, copies nothing. Never returns 8100 for a short tail — clamp and
 * report the copied length in actual_bytes.
 *
 * @param bytes         Destination. Ignored when cap_bytes is 0 or offset is
 *                      past the used payload. LabVIEW: Array Data Pointer.
 * @param offset_bytes  Start index into the used payload, in bytes.
 * @param cap_bytes     Maximum bytes to copy (destination capacity).
 * @param actual_bytes  Out: bytes copied this call (0 if offset >= used).
 * @param max_bytes     Out: iceoryx2 value size in bytes (same as read_raw).
 */
IOX2_CIF_API iox2_cif_status IOX2_CIF_CALL iox2_cif_bb_read_raw_at(
    uint64_t reader,
    void* bytes,
    uint32_t offset_bytes,  /* bytes into used payload */
    uint32_t cap_bytes,     /* bytes */
    uint32_t* actual_bytes, /* out: bytes copied */
    uint32_t* max_bytes     /* out: value size in bytes */
);

/* -------------------------------------------------------------------------- */
/* Teardown                                                                     */
/* -------------------------------------------------------------------------- */

/**
 * Before Start: unregister the topic from the staging list.
 * After Start: drop that topic's EntryHandleMut. Does not remove the iceoryx2
 * key, does not drop the Writer port. Last value stays readable.
 */
IOX2_CIF_API iox2_cif_status IOX2_CIF_CALL iox2_cif_bb_destroy_writer(uint64_t writer);

/** Drop that Reader port (frees one max_readers slot — a reader port, not payload elements). */
IOX2_CIF_API iox2_cif_status IOX2_CIF_CALL iox2_cif_bb_destroy_reader(uint64_t reader);

/**
 * Invalidate every writer ref created on this blackboard.
 * Drop the iceoryx2 Writer and PortFactory.
 * Do not wait for, unmap, or kill remote readers.
 *
 * @param readers_remaining  Optional out: iceoryx2 readers still attached (ports, not bytes).
 *                           NULL if unused.
 */
IOX2_CIF_API iox2_cif_status IOX2_CIF_CALL iox2_cif_bb_destroy(uint64_t bb, uint32_t* readers_remaining /* out: reader ports */);

/* -------------------------------------------------------------------------- */
/* Publish-subscribe                                                            */
/*                                                                            */
/* One LabVIEW writer handle, two internal iceoryx2 services when the writer  */
/* opts in: best-effort (miss if late) and backpressure.                      */
/* Same (name, topic) must not also be a blackboard.                          */
/* Timeouts on write/receive are I32 timed_out outs, not status errors.       */
/* -------------------------------------------------------------------------- */

/**
 * Create the stream (first iceoryx2 pub/sub create).
 *
 * `be_max_subscribers` / `bp_max_subscribers` are ports per internal service.
 * 0 skips that publisher. Both 0 is IOX2_CIF_ERR_INVALID_ARGUMENT.
 * `be_subscriber_buffer` / `bp_subscriber_buffer` are queue depths (samples).
 * A created side requires its depth >= 1. A skipped side ignores its depth.
 * Payload chunks are not one copy per subscriber. Best-effort reserves
 * `be_subscriber_buffer + be_max_subscribers + 1`. Backpressure reserves
 * `bp_subscriber_buffer + 2`. One publish copies the payload once.
 */
IOX2_CIF_API iox2_cif_status IOX2_CIF_CALL iox2_cif_ps_writer_create(
    const char* name,
    const char* topic,
    const char* type_name,
    uint32_t elem_size,               /* bytes per element */
    uint32_t elem_align,              /* bytes */
    uint32_t max_count,               /* elements; 1 for scalar/cluster */
    const char* descriptor,           /* JSON sidecar, same as blackboard */
    uint32_t be_max_subscribers,      /* BE subscriber ports; 0 skips BE */
    uint32_t bp_max_subscribers,      /* BP subscriber ports; 0 skips BP */
    uint32_t be_subscriber_buffer,    /* BE queue depth (samples); ignored if no BE */
    uint32_t bp_subscriber_buffer,    /* BP queue depth (samples); ignored if no BP */
    uint64_t* writer);

/**
 * Open a subscriber on an existing stream.
 *
 * `use_backpressure` 0 = miss if late (BE); 1 = apply backpressure (BP).
 * The side that was not created → 8115.
 * Service never appears → 8109. Type/size/align mismatch → 8106.
 */
IOX2_CIF_API iox2_cif_status IOX2_CIF_CALL iox2_cif_ps_sub_create(
    const char* name,
    const char* topic,
    const char* type_name,
    uint32_t elem_size,
    uint32_t elem_align,
    int32_t is_iox2_cif_array,         /* 1 = iox2_cif.array.* only; 0 = bind as-is */
    int32_t use_backpressure,         /* 0 = miss if late; 1 = apply backpressure */
    uint32_t timeout_ms,              /* wait for the service */
    uint64_t* subscriber);

/**
 * Publish `count` elements.
 *
 * `timeout_ms` applies only to the BP send. 0 = one try.
 * If BP times out, do not send BE. Status is 0; `*timed_out` is 1.
 * LabVIEW: timed_out is I32 Pointer to Value (0 or 1), not 8-bit Boolean.
 */
IOX2_CIF_API iox2_cif_status IOX2_CIF_CALL iox2_cif_ps_write(
    uint64_t writer,
    const void* elems,
    uint32_t count,                   /* elements */
    uint32_t timeout_ms,
    int32_t* timed_out);

/**
 * Copy one queued sample into `elems`.
 *
 * timeout_ms 0 = one shot (no wait). timeout_ms > 0 waits for a sample or notify.
 * No valid sample → status 0, actual 0, timed_out 1 (even when timeout_ms is 0).
 * timed_out 0 means the payload/actual outputs are valid.
 */
IOX2_CIF_API iox2_cif_status IOX2_CIF_CALL iox2_cif_ps_receive(
    uint64_t subscriber,
    void* elems,
    uint32_t cap,                     /* elements */
    uint32_t timeout_ms,
    uint32_t* actual,
    uint32_t* max,
    int32_t* timed_out);

/** Non-blocking probe. Does not wait. `has` is I32 Pointer to Value, 0 or 1. */
IOX2_CIF_API iox2_cif_status IOX2_CIF_CALL iox2_cif_ps_has_sample(uint64_t subscriber, int32_t* has);

/**
 * Live subscriber-port counts on this writer's internal services.
 * `backpressure` is 0 when this writer has no backpressure publisher.
 */
IOX2_CIF_API iox2_cif_status IOX2_CIF_CALL iox2_cif_ps_writer_subscriber_count(
    uint64_t writer,
    uint32_t* best_effort,
    uint32_t* backpressure);

IOX2_CIF_API iox2_cif_status IOX2_CIF_CALL iox2_cif_ps_destroy_writer(uint64_t writer);
IOX2_CIF_API iox2_cif_status IOX2_CIF_CALL iox2_cif_ps_destroy_sub(uint64_t subscriber);

/**
 * Probe a pub/sub stream by name + topic (raw or pcap). Reads iceoryx2 service_details
 * on `{name}/{topic}.iox2_cif.ps.be`. Missing after timeout → 8109.
 */
IOX2_CIF_API iox2_cif_status IOX2_CIF_CALL iox2_cif_ps_info(
    const char* name,
    const char* topic,
    uint32_t timeout_ms,
    char* type_name,
    uint32_t type_cap,
    uint32_t* type_len,
    uint32_t* max_count,
    uint32_t* be_subscriber_buffer,
    uint32_t* bp_subscriber_buffer,
    uint32_t* value_size);

/* -------------------------------------------------------------------------- */
/* CIF-pcap pub/sub                                                             */
/*                                                                            */
/* Sample: 48-byte iox2_cif_pcap_hdr_t + pcap records. */
/* Typed type_name iox2_cif.pcap.<linktype>; universal iox2_cif.pcap.         */
/* LINKTYPE_ANY on writer create originates a universal stream.               */
/* One writer per (name, topic), same as raw pub/sub.                         */
/* -------------------------------------------------------------------------- */

IOX2_CIF_API iox2_cif_status IOX2_CIF_CALL iox2_cif_pcap_writer_create(
    const char* name,
    const char* topic,
    uint32_t linktype,
    uint32_t max_pcap_bytes,
    const char* descriptor,
    uint32_t be_max_subscribers,
    uint32_t bp_max_subscribers,
    uint32_t be_subscriber_buffer,
    uint32_t bp_subscriber_buffer,
    uint64_t* writer);

IOX2_CIF_API iox2_cif_status IOX2_CIF_CALL iox2_cif_pcap_sub_create(
    const char* name,
    const char* topic,
    uint32_t linktype,
    int32_t use_backpressure,
    uint32_t timeout_ms,
    uint64_t* subscriber);

IOX2_CIF_API iox2_cif_status IOX2_CIF_CALL iox2_cif_pcap_sub_open(
    const char* name,
    const char* topic,
    int32_t use_backpressure,
    uint32_t timeout_ms,
    uint64_t* subscriber);

IOX2_CIF_API iox2_cif_status IOX2_CIF_CALL iox2_cif_pcap_info(
    uint64_t port,
    char* type_name,
    uint32_t type_cap,
    uint32_t* type_len,
    uint32_t* linktype,
    uint32_t* max_pcap_bytes,
    uint32_t* be_subscriber_buffer,
    uint32_t* bp_subscriber_buffer);

/**
 * Publish CIF header, then pcap records. `length` is pcap-record bytes only
 * and must be <= `max_pcap_bytes` or the call returns 8108.
 */
IOX2_CIF_API iox2_cif_status IOX2_CIF_CALL iox2_cif_pcap_write(
    uint64_t writer,
    const void* header,
    const void* pcap,
    uint32_t offset,
    uint32_t length,
    uint32_t timeout_ms,
    int32_t* timed_out,
    uint64_t* samples,
    uint64_t* timeouts);

/**
 * Publish a whole CIF-pcap sample as opaque bytes: 48-byte header, then pcap
 * records. Intended for remote transport (TCP and similar): pass the buffer
 * from `iox2_cif_pcap_receive_raw` (or an equivalent flatten) unchanged.
 *
 * Same BE/BP send rules as `iox2_cif_pcap_write`. Stamps `length` (this
 * `length` argument) and `ctr` (this writer). Leaves `t0` / `t_out` / `t_tx` /
 * `clock_id` from the buffer. Zeros `reserved`.
 *
 * `length` is full sample bytes, including the header, and must be >= 48.
 * `length - 48` must be <= the writer's `max_pcap_bytes` or the call returns
 * 8108 (no truncate). LabVIEW: U8 Array Data Pointer.
 */
IOX2_CIF_API iox2_cif_status IOX2_CIF_CALL iox2_cif_pcap_write_raw(
    uint64_t writer,
    const void* sample,
    uint32_t length,
    uint32_t timeout_ms,
    int32_t* timed_out,
    uint64_t* samples,
    uint64_t* timeouts);

IOX2_CIF_API iox2_cif_status IOX2_CIF_CALL iox2_cif_pcap_writer_stats(
    uint64_t writer, uint64_t* samples, uint64_t* timeouts, uint64_t* last_ctr);

IOX2_CIF_API iox2_cif_status IOX2_CIF_CALL iox2_cif_pcap_writer_stats_reset(uint64_t writer);

/**
 * Same timeout / timed_out rules as iox2_cif_ps_receive: no valid sample →
 * status 0, actual 0, timed_out 1 (including timeout_ms 0).
 *
 * `actual` is pcap-record bytes only (not including the CIF header).
 */
IOX2_CIF_API iox2_cif_status IOX2_CIF_CALL iox2_cif_pcap_receive(
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
    uint64_t* timeouts);

/**
 * Receive a whole CIF-pcap sample as opaque bytes (header + records). Same
 * timeout / timed_out rules as `iox2_cif_pcap_receive`.
 *
 * `actual` is the full sample size (`hdr.length`, including the 48-byte
 * header). `max` is 48 + `max_pcap_bytes`. Size the U8 array to `max` so the
 * copy is never truncated. If `cap` < `actual`, copies `cap` bytes, still
 * reports the full `actual`, and the sample is consumed.
 *
 * No sample → status 0, `actual` 0, `timed_out` 1 (including `timeout_ms` 0).
 * LabVIEW: U8 Array Data Pointer.
 */
IOX2_CIF_API iox2_cif_status IOX2_CIF_CALL iox2_cif_pcap_receive_raw(
    uint64_t subscriber,
    void* sample,
    uint32_t cap,
    uint32_t timeout_ms,
    uint32_t* actual,
    uint32_t* max,
    int32_t* timed_out,
    uint64_t* samples,
    uint64_t* dropped,
    uint64_t* timeouts);

IOX2_CIF_API iox2_cif_status IOX2_CIF_CALL iox2_cif_pcap_stats(
    uint64_t subscriber,
    uint64_t* samples,
    uint64_t* dropped,
    uint64_t* timeouts,
    uint64_t* last_ctr);

IOX2_CIF_API iox2_cif_status IOX2_CIF_CALL iox2_cif_pcap_stats_reset(uint64_t subscriber);

IOX2_CIF_API iox2_cif_status IOX2_CIF_CALL iox2_cif_pcap_flush(uint64_t subscriber, uint32_t* discarded);

IOX2_CIF_API iox2_cif_status IOX2_CIF_CALL iox2_cif_pcap_has_sample(uint64_t subscriber, int32_t* has);

IOX2_CIF_API iox2_cif_status IOX2_CIF_CALL iox2_cif_pcap_writer_subscriber_count(
    uint64_t writer, uint32_t* best_effort, uint32_t* backpressure);

IOX2_CIF_API iox2_cif_status IOX2_CIF_CALL iox2_cif_pcap_destroy_writer(uint64_t writer);
IOX2_CIF_API iox2_cif_status IOX2_CIF_CALL iox2_cif_pcap_destroy_sub(uint64_t subscriber);

#ifdef __cplusplus
}
#endif

#endif /* IOX2_CIF_H */
