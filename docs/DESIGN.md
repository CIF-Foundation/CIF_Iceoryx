# iceoryx2 CIF wrapper — design plan

Status: **v7, CIF C ABI (in development).**

CIF = Composable Instrumentation Framework. One iceoryx2 blackboard service per
blackboard name. Topics are keys on that service. Stage topics, then Start
(create or reopen). Any language with a C FFI opens the same service name and
hashed topic keys. LabVIEW is one client.

---

## Goals

- IPC so other processes and languages can attach.
- Dynamically add topics **before** the iceoryx2 service exists (stage, then Start).
- Topic names are ordinary strings (portable via a documented u64 hash).
- Create/open writer refs, then read/write from other threads in the process.
- VIM for any primitive or flat cluster.
- 1D arrays of flat types, with a required max size at writer create and a live count.
- Raw byte read that skips the LabVIEW type.
- Multiple independent readers in one LabVIEW process.
- Writer restart with the same blackboard name while readers remain (subset reopen).
- Highly performant hot path (pointer + memcpy, no Flatten To String).

## Non-goals (v1)

- iceoryx2 events / request-response as a public ABI (pub/sub uses a paired Event service internally).
- Adding or removing keys **after** the iceoryx2 service is created (iceoryx2 cannot).
- LabVIEW handles: strings, paths, variants, waveforms, refnums, nested arrays, clusters that contain arrays or strings, 2D+ arrays.
- Truncating array writes that exceed max size (those fail).
- Forcibly destroying SHM while remote readers are mapped.

---

## iceoryx2 constraints this design respects

- Blackboard keys are frozen at `create()`. Dynamic add/remove is planned upstream, not available.
- C/Python cannot share `StaticString` keys; keys here are `uint64_t`.
- One Writer port per blackboard service globally.
- `max_readers` is pre-allocated at create and cannot grow.
- Service resources are refcounted. Dropping the local Writer does not kick readers.
- iceoryx2 service **attributes** are not a type catalog (max 16 tags, short strings). Do not store the topic list there. A single marker such as `iox2_cif.abi=1` is optional.

---

## Mapping

| LabVIEW | iceoryx2 |
|---|---|
| Blackboard name, e.g. `robot` | `ServiceName` (IPC) |
| Topic string, e.g. `pose` | Key `uint64_t` = FNV-1a-64 of UTF-8 topic (no NUL) |
| Process | One IPC-threadsafe Node + background `node.wait()` |
| Writer Create (before Start) | Local staging list only |
| Start (first time) | `blackboard_creator`, `.add` each topic, `writer_builder.create()`, bind `EntryHandleMut` |
| Start (reopen) | `blackboard_opener`, claim Writer if slot empty, bind staged topics only |
| Each Reader Create | New Reader port + `entry(key)` — counts against `max_readers` |
| Payload | iceoryx2 type_name + size + alignment; optional array slot header |

One blackboard name is one iceoryx2 service. Multiple blackboard names are multiple services.

`max_readers` is once per blackboard, sized for concurrent reader ports across all topics and all processes. It applies only on **first create**. Reopen ignores it.

---

## Lifecycle

```
Create Blackboard          → blackboard ref          (no iceoryx2)
  Writer Create × N        → writer refs             (no iceoryx2; stage topics)
Start Blackboard           → create or reopen        (first iceoryx2 call)
  Reader Create            → reader refs             (open by name + topic)
  Write / Read / Read Raw
Destroy Writer / Reader    → free that ref
Destroy Blackboard         → invalidate writers, drop Writer, readers may remain
```

Writer Create after Start is an error. New topic names on a live service fail until that service is fully gone (no remaining nodes).

### Start: create vs reopen

| Existing service | Staged topics vs stored keys | Writer slot | Result |
|---|---|---|---|
| None | — | — | **Create:** add every staged topic, take Writer, write initials from Writer Create |
| Present | Every staged topic exists with same type_name, size, align, array max. Extra keys OK | Empty | **Open:** take Writer, bind staged topics only. Do not rewrite values |
| Present | Staged topics exist and match | Held | **Error:** writer still attached |
| Present | Staged topic missing, or type/size/array max differs | Any | **Error:** schema mismatch (cannot add or reshape keys) |

Reopen is all-or-nothing for the **staged** set. Extra keys on the service are not an error; they keep last values. If any staged bind fails, drop the Writer so the slot is not left claimed.

A crashed writer may occupy the slot until iceoryx2 dead-node cleanup (`node.wait()`). Start may fail briefly, then succeed.

---

## LabVIEW API

Typed refnums: blackboard, writer, reader. VIMs are preallocated-clone reentrant. Call Library Function: **Run in any thread**. Adapt to Type on the payload.

### Stage (no iceoryx2)

**`iox2 BB Create.vi`**
`[name] [max readers] [timeout?] → [blackboard] [error]`

**`iox2 BB Writer Create.vim`**
`[blackboard] [topic] [data] [max array size*] [type name?] → [blackboard] [writer] [error]`

- Blackboard ref in and out so calls can be chained.
- Duplicate topic names on the same blackboard: error.
- Max array size is **required** when Data is an array (`≥ 1` and `≥` initial length). Ignored for scalars/clusters (`max_count = 1`). No Init default.
- Type name optional for primitives (mapped automatically); **required** for clusters.

### Start / readers

**`iox2 BB Start.vi`**
`[blackboard] → [blackboard] [error]`

**`iox2 BB Reader Create.vim`**
`[blackboard name] [topic] [type] [type name?] [timeout] → [reader] [error]`

Readers do not use the staging ref. Other processes open by blackboard name + topic string.

**`iox2 BB Open Raw Reader.vi`**
`[blackboard name] [topic] [timeout] → [reader] [error]`

Discovers iceoryx2 type details from the existing entry (and/or key-0 directory). No LabVIEW type check.

### Hot path

**`iox2 BB Write.vim`**
`[writer] [data] → [writer] [error]`

Array length `> max` → error, no write, no truncate.

**`iox2 BB Read.vim`**
`[reader] → [data] [actual size] [max size] [reader] [error]`

Actual/max are element counts. Scalars report `1` / `1`. The VI resizes LabVIEW arrays; the DLL does not.

**`iox2 BB Read Raw.vi`**
`[reader] → [bytes U8[]] [actual bytes] [max bytes] [reader] [error]`

Used payload bytes only. Works on typed or raw readers. No LabVIEW type check.

### Teardown

**`iox2 BB Destroy Writer.vi`**
- Before Start: unregister that topic from the staging list.
- After Start: drop that topic’s `EntryHandleMut`. Does **not** remove the iceoryx2 key. Last value stays readable. Does **not** drop the Writer port.

**`iox2 BB Destroy Reader.vi`**
Drop that Reader port (frees one `max_readers` slot).

**`iox2 BB Destroy Blackboard.vi`**
- Invalidate **every** writer ref created on this blackboard (whether Destroy Writer was called or not).
- Later Write on those refs → invalid-reference **error** (not a no-op, not a recreate).
- Drop the iceoryx2 Writer and PortFactory.
- Do not wait for, unmap, or kill remote readers.
- Optional output: remaining reader count.
- To write again: new Create Blackboard, restage writers, Start.

iceoryx2 has no “destroy service and disconnect everyone.” If readers remain, SHM stays until the last node closes. Same name can Start again (reopen) while they remain, if the schema allows.

---

## Native C surface

Process-wide Node is created lazily on first Start/Reader Create (IPC threadsafe) unless an Init helper is added later.

```c
iox2_cif_status iox2_cif_bb_create(const char* name, uint32_t max_readers, uint64_t* bb);

iox2_cif_status iox2_cif_bb_writer_create(
    uint64_t bb, const char* topic, const char* type_name,
    uint32_t elem_size, uint32_t elem_align, uint32_t max_count,
    const void* initial, uint32_t initial_count, const char* descriptor,
    uint64_t* writer);

iox2_cif_status iox2_cif_bb_start(uint64_t bb);  /* create, or open + claim writer (subset OK) */

iox2_cif_status iox2_cif_bb_reader_create(
    const char* bb_name, const char* topic, const char* type_name,
    uint32_t elem_size, uint32_t elem_align, int32_t is_iox2_cif_array,
    uint32_t timeout_ms, uint64_t* reader);

iox2_cif_status iox2_cif_bb_reader_create_raw(
    const char* bb_name, const char* topic, uint32_t timeout_ms,
    uint64_t* reader);

iox2_cif_status iox2_cif_bb_topic_info(
    const char* bb_name, const char* topic, uint32_t timeout_ms,
    char* type_name, uint32_t type_cap, uint32_t* type_len,
    char* descriptor, uint32_t desc_cap, uint32_t* desc_len);

iox2_cif_status iox2_cif_bb_write(uint64_t writer, const void* elems, uint32_t count);
iox2_cif_status iox2_cif_bb_read(uint64_t reader, void* elems, uint32_t cap,
    uint32_t* actual, uint32_t* max);
iox2_cif_status iox2_cif_bb_read_raw(uint64_t reader, void* bytes, uint32_t cap_bytes,
    uint32_t* actual_bytes, uint32_t* max_bytes);
iox2_cif_status iox2_cif_bb_read_raw_at(uint64_t reader, void* bytes,
    uint32_t offset_bytes, uint32_t cap_bytes,
    uint32_t* actual_bytes, uint32_t* max_bytes);

iox2_cif_status iox2_cif_bb_destroy_writer(uint64_t writer);
iox2_cif_status iox2_cif_bb_destroy_reader(uint64_t reader);
iox2_cif_status iox2_cif_bb_destroy(uint64_t bb, uint32_t* readers_remaining);
```

Handles are generation-counted `uint64_t` refnums. Hot path: table load + memcpy. Mutex only on create/destroy. Writer interned per blackboard (one iceoryx2 Writer). Readers are **not** interned.

Hot path is pointer + count (or raw bytes). Same C ABI on Windows, NI Linux RT, and Linux desktop.

---

## Topic key ABI (cross-language)

C/Python cannot use `StaticString` keys. The contract is:

1. **Service** = blackboard name string.
2. **Key** = FNV-1a 64 of the topic UTF-8 bytes (no NUL).
3. **Value** = iceoryx2 `type_name` + `size` + `alignment`. iceoryx2 does not know struct fields.

### FNV-1a 64

```
offset basis = 0xcbf29ce484222325
prime        = 0x100000001b3

hash = basis
for each byte b in UTF-8(topic):
    hash ^= b
    hash *= prime
```

Case-sensitive. No path normalization. Two staged names that collide fail at first Start. **Key 0 is reserved** (optional directory / invalid). A topic that hashes to 0 fails at first Start.

Example: other languages call `entry` with `fnv1a_64("pose")` on service `"robot"`.

### Value type_name

| LabVIEW Data | type_name | size / align | Foreign side |
|---|---|---|---|
| I8 I16 I32 I64 | `i8` `i16` `i32` `i64` | natural | same integer |
| U8 U16 U32 U64 | `u8` `u16` `u32` `u64` | natural | same integer |
| SGL / DBL | `f32` / `f64` | 4/4 or 8/8 | `float` / `double` |
| Boolean | `bool` | 1 / 1 | `uint8_t` 0 or 1 — not C++ `bool`, and not `u8` |
| Flat cluster | user string, **required**, e.g. `RobotPose` | LabVIEW cluster size and alignment | matching `repr(C)` struct, same name/size/align |
| 1D array of the above | `iox2_cif.array.<elem_type_name>` | `8 + max_count * elem_size` (aligned) | slot header then elements |

### Array slot layout

```c
typedef struct {
    uint32_t count;     /* live element count, 0..max_count */
    uint32_t reserved;  /* 0 */
    /* uint8_t payload[max_count * elem_size];  only [0, count) valid */
} iox2_cif_slot_t;
```

### Clusters are a named blob

iceoryx2 checks name, size, and alignment only. If `RobotPose` is 24 bytes / align 8 on both sides, the entry opens. Field order and padding must match LabVIEW (Boolean as `bool`, 1 byte, no handles). Wrong padding with the same size is a silent data bug.

A scalar reader may request a different standard numeric type. Conversion uses the published iceoryx2 type name and does not require the IDL JSON. A reader named `topic.field` reads one `struct_elems` entry from that JSON (`type`, `offset`, `size`, `Endian`) and converts into the subscriber type. That open fails when the descriptor's `Array` flag is true or the topic is a 1D array.

### Optional key-0 directory

Not iceoryx2 attributes. Reserved key `0` holds a C struct listing `name`, hashed key, type_name, size, align, max_count — filled at first Start, sized to that topic count. Subset reopen does not shrink it. Primary discovery remains: hash the known topic string.

Interface-descriptor JSON is stored by the wrapper in a sidecar file (not an iceoryx2 key), so any process can call `iox2_cif_bb_topic_info` without a fixed SHM reservation.

---

## CIF-pcap pub/sub

Raw `iox2_cif_ps_*` is typed pointer+count (same BE/BP and Event+WaitSet as planned). CIF-pcap is a second ABI on the same iceoryx2 services (`{name}/{topic}.iox2_cif.ps.be` / `.bp`):

- On-wire: 48-byte `iox2_cif_pcap_hdr_t` then opaque pcap records (no global header, no `iox2_cif_slot_t`).
- Typed type `iox2_cif.pcap.<linktype>`; universal type `iox2_cif.pcap` (writer create with `IOX2_CIF_PCAP_LINKTYPE_ANY`).
- Typed subscribers accept an exact type or a universal writer. Universal subscribers accept any pcap type. Typed-to-typed LinkType mismatch is 8106.
- Write stamps `length` and `ctr` only. Subscriber/writer stats and subscriber-only flush are pcap-only.
- One writer per `(name, topic)` for both raw and pcap.

Same `(name, topic)` must not mix blackboard, raw, and pcap.

## Allowed / rejected types

**Allowed:** integers, SGL, DBL, Boolean; nested clusters of those; 1D array of those (array of a flat cluster is OK).

**Rejected at Writer/Reader Create:** String, Path, Variant, Waveform, Refnum, array of strings, array of arrays, cluster containing an array or string, 2D+.

---

## Threading and performance

- Transport: `ipc_threadsafe` only for v1.
- Create/open on any thread; fork writer/reader refnums into other loops.
- Background thread calls `node.wait()` so LabVIEW loops do not.
- Write: `update_with_copy` (memcpy into inactive slot, atomic publish).
- Concurrent in-process writes on the same topic: last-write-wins.
- No Flatten To String, no Variant on the hot path.

---

## Suggested repo layout

```
native/          CMake C wrapper over iceoryx2-ffi-c (Windows DLL)
labview/         lvlib, refnum .ctls, .vim, error codes, palette
examples/        LV writer/reader; Python/C counterpart on the same names
docs/DESIGN.md   this file
docs/abi.md      extracted ABI tables once implementation starts (optional)
```

---

## Open items (refine during implementation)

1. Key-0 directory: include in v1 vs hash-only first.
2. LabVIEW cluster packing vs MSVC default align — document and add a layout test VI.
3. Error code range for the LabVIEW error cluster.
4. Optional `iox2 Init.vi` (node name) vs lazy Node on first Start.
5. `max_nodes` as an extra Create Blackboard input if many processes attach.

---

## Revision history

| Version | Summary |
|---|---|
| v1 | One iceoryx2 service per topic; interned readers |
| v2 | IPC; independent readers; raw read; bounded arrays |
| v3 | Required max array size and max_readers on create |
| v4 | Stage blackboard / Writer Create / Start; one service, many keys |
| v5 | Start reopen when schema matches and writer slot empty |
| v6 | Subset reopen; Destroy Blackboard invalidates writers; FNV-1a key ABI; no attribute catalog |
| v7 | Raw `iox2_cif_ps_*` plus CIF-pcap `iox2_cif_pcap_*` (48-byte header + pcap records, universal type) |
