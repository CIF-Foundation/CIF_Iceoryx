# LabVIEW — call the CIF wrapper (`iox2_cif.dll`) on Windows

64-bit LabVIEW only. The DLL is cdecl. After `scripts/build.ps1` succeeds, point Call Library Function nodes at:

```
D:\dev\iceoryx2_lvwrapper\dist\iox2_cif.dll
```

That is the only native binary to ship (iceoryx2 is linked into it).

If a node **never returns on the first call**, abort LabVIEW and set create/start/destroy to **Run in UI thread**. First-load of this fat DLL from a LabVIEW execution thread can deadlock in `LoadLibrary` (Rust runtime vs Windows loader lock). That hang is before any wrapper code runs; lock timeouts cannot catch it. Hot-path read/write can stay **Run in any thread**.

Closing a VI must not quit LabVIEW. The DLL starts a background `node.wait()` thread; if LabVIEW unloads `iox2_cif.dll` while that thread is still running, LabVIEW.exe crashes (it looks like the whole application just closed). This build **pins** the DLL so that unload is deferred until LabVIEW exits. In each Call Library Function node, leave **Unload after call** unchecked. Still call `iox2_cif_bb_destroy` / `iox2_cif_shutdown` on stop if you want SHM cleaned up before the next run.

If a later call used to hang forever after a crash, it should now return **8117** (`lock timeout`, ~5 s) instead. After 8117, abort LabVIEW if calls keep failing — the previous owner of the lock may have died mid-update.

iceoryx2 does not use RouDi. No extra service process is required.

## One-time build

From PowerShell (CMake, git, Rust MSVC, Visual Studio C++ tools):

```powershell
powershell -ExecutionPolicy Bypass -File D:\dev\iceoryx2_lvwrapper\scripts\build.ps1
```

That produces `dist\iox2_cif.dll` and runs a C smoke test (I32 write/read).

## Minimum LabVIEW test (I32)

Use two loops or two VIs in the same bitness. Handles are **U64**. Status is **I32** (`0` = success, `8100–8199` = wrapper errors).

### Call Library Function settings (every node)

| Field | Value |
|---|---|
| Library | `...\dist\iox2_cif.dll` |
| Calling convention | C (cdecl) |
| Run in | **UI thread** for create/start/destroy; **any thread** for write/read |
| Return type | Signed 32-bit Integer |

Strings: **C String Pointer**. Output handles: **Unsigned 64-bit Integer**, Pass **Pointer to Value**.

### Writer VI (run first)

1. `iox2_cif_bb_create`
   - `name` = `robot` (or any unique string)
   - `max_readers` = `8` (U32, Pass **Value**, not Pointer)
   - `bb` = U64 out
2. `iox2_cif_bb_writer_create`

Prototype must be exactly:

```
int32_t iox2_cif_bb_writer_create(uint64_t bb, CStr topic, CStr type_name,
    uint32_t elem_size, uint32_t elem_align, uint32_t max_count,
    void *initial, uint32_t initial_count, CStr descriptor, uint64_t *writer);
```

| Parameter | Type | Pass |
|---|---|---|
| `bb` | Unsigned 64-bit | **Value** (the handle from create, not a pointer) |
| `topic` | C String Pointer | — |
| `type_name` | C String Pointer | — (`i32` for I32) |
| `elem_size` | U32 | Value (`4` for I32) |
| `elem_align` | U32 | Value (`4` for I32) |
| `max_count` | U32 | Value (`1` for a scalar — not 32) |
| `initial` | I32 (or Adapt to Type) | **Pointer to Value** |
| `initial_count` | U32 | Value (`1` for a scalar) |
| `descriptor` | C String Pointer | JSON interface descriptor; empty string is OK |
| `writer` | Unsigned 64-bit | **Pointer to Value** |

If `initial` or `writer` is Pass **Value**, LabVIEW reports “error occurred in external code”. The next call should return **8117** rather than hang; still **abort LabVIEW** before replacing the DLL or retrying a long sequence.
   - `initial` = Adapt to Type, I32 (e.g. `0`)
   - `initial_count` = `1`
   - `writer` = U64 out
3. `iox2_cif_bb_start` (`bb`)
4. Loop: `iox2_cif_bb_write` (`writer`, Adapt to Type I32, `count` = `1`)
5. Stop: `iox2_cif_bb_destroy` (`bb`, `readers_remaining` can be a U32 pointer or omitted by passing 0 if you configure the node as a pointer that may be NULL — easiest: add a U32 indicator and ignore it)

### Reader VI (same machine, same blackboard name)

1. `iox2_cif_bb_reader_create`
   - `bb_name` = `robot`
   - `topic` = `pose`
   - `type_name` = `i32`
   - `elem_size` = `4`, `elem_align` = `4`
   - `is_iox2_cif_array` = `0` (I32 Value; `1` for this wrapper’s 1D arrays)
   - `timeout_ms` = `5000`
   - `reader` = U64 out
2. Loop: `iox2_cif_bb_read`
   - `reader`
   - `elems` = Adapt to Type I32
   - `cap` = `1`
   - `actual`, `max` = U32, Pass Pointer to Value
3. Stop: `iox2_cif_bb_destroy_reader`

Start the writer (through **Start**) before the reader, or give the reader a few seconds of `timeout_ms`.

## Function checklist for CLFN

| C function | Extra parameters |
|---|---|
| `iox2_cif_bb_create` | `const char*`, `uint32`, `uint64*` |
| `iox2_cif_bb_writer_create` | `uint64`, `const char*` topic, `const char*` type, `uint32` size/align/max, Adapt to Type data, `uint32` count, `const char*` descriptor, `uint64*` |
| `iox2_cif_bb_start` | `uint64` |
| `iox2_cif_bb_reader_create` | two C strings, type string, size/align, I32 `is_iox2_cif_array`, timeout ms, `uint64*` |
| `iox2_cif_bb_topic_info` | two C strings, timeout ms, type buf + cap + len, descriptor buf + cap + len |
| `iox2_cif_bb_write` | `uint64`, Adapt to Type, `uint32` count |
| `iox2_cif_bb_read` | `uint64`, Adapt to Type, `uint32` cap, `uint32*` actual, `uint32*` max |
| `iox2_cif_bb_read_raw` | `uint64`, Adapt to Type bytes, `uint32` cap_bytes, `uint32*` actual, `uint32*` max |
| `iox2_cif_bb_read_raw_at` | `uint64`, Adapt to Type bytes, `uint32` offset_bytes, `uint32` cap_bytes, `uint32*` actual, `uint32*` max |
| `iox2_cif_bb_destroy_writer` / `destroy_reader` | `uint64` |
| `iox2_cif_bb_destroy` | `uint64`, `uint32*` remaining (Pointer to Value) |

Optional: `iox2_cif_init("cif")` once at start; `iox2_cif_shutdown` when done. If you skip them, the first Start/Reader Create creates the process Node. Closing the VI without shutdown should leave LabVIEW running; the node lives until LabVIEW.exe exits.

## Error codes

`0` success. Common failures: `8100` bad argument, `8101` bad handle, `8105` writer still held, `8106` schema mismatch, `8109` reader-create timeout, `8116` internal (iceoryx2), `8117` wrapper lock timeout. Wire the I32 return into a LabVIEW error cluster (`source` = function name).

## Notes for this prototype

- Use **pointer + count** (`iox2_cif_bb_write` / `read` / `read_raw`) on all targets. Resize LabVIEW arrays in the VI before the copy.
- Keep the DLL path stable, or copy `iox2_cif.dll` next to the VI / into `LabVIEW\resource`.
- Destroy the blackboard in the writer process before creating a new one with the same name **if** you need a different topic set. Same schema can **Start** again (reopen) while readers remain.
- Cluster payloads later: pass Adapt to Type on a **flat** cluster, `type_name` required (e.g. `RobotPose`), `elem_size` / `elem_align` from LabVIEW’s cluster size.

## Raw pub/sub (`iox2_cif_ps_*`)

One writer handle. `be_max_subscribers` 0 skips best-effort. `bp_max_subscribers` 0 skips backpressure. Both 0 is an error. Write sends backpressure first when that publisher exists; if that wait expires, best-effort is not sent. Timeouts are I32 `timed_out` outs (Pointer to Value), not error-cluster codes. Do not pass `iox2_cif.pcap` types here (use the CIF-pcap API).

Create/destroy / `ps_info`: **Run in UI thread**. `ps_write` / `ps_receive` / `ps_has_sample` / `ps_writer_subscriber_count`: any thread. Flags are I32 Value (0 or 1), not 8-bit Boolean. Payload: Adapt to Type, **Array Data Pointer**.

| C function | LabVIEW parameters |
|---|---|
| `iox2_cif_ps_writer_create` | CStr name/topic/type/desc; U32 size/align/max_count/`be_max_subscribers`/`bp_max_subscribers`/`be_subscriber_buffer`/`bp_subscriber_buffer`; U64* writer. A max of 0 skips that publisher. Both maxes 0 is an error. |
| `iox2_cif_ps_sub_create` | CStr name/topic/type; U32 size/align/timeout; I32 is_iox2_cif_array, use_bp (Value); U64* subscriber. Queue depth is the writer's advertised max for that side. |
| `iox2_cif_ps_write` | U64 writer (Value); payload Array Data Pointer; U32 count, timeout_ms; I32* timed_out |
| `iox2_cif_ps_receive` | U64 sub; payload Array Data Pointer; U32 cap, timeout_ms; U32* actual, max; I32* timed_out |
| `iox2_cif_ps_has_sample` | U64; I32* has |
| `iox2_cif_ps_writer_subscriber_count` | U64; U32* best_effort, backpressure |
| `iox2_cif_ps_info` | CStr name/topic; U32 timeout; type buf + U32 cap + U32* len; U32* max_count, be_subscriber_buffer, bp_subscriber_buffer, value_size |
| `iox2_cif_ps_destroy_writer` / `destroy_sub` | U64 |

`timeout_ms` 0 on receive is one-shot (no wait). `timeout_ms` > 0 waits. No valid sample → `timed_out` 1, status 0, even when `timeout_ms` is 0. `timed_out` 0 means the payload is valid.

## CIF-pcap pub/sub (`iox2_cif_pcap_*`)

Header: Adapt to Type on a **flat 48-byte cluster** — `U64 length`, `I64 t0`, `I64 t_out`, `I64 t_tx`, `U64 ctr`, `U8 clock_id`, 7 reserved bytes. Pcap records: **Array Data Pointer** (U8). `clock_id` is any U8 (0 = TAI by convention only). The wrapper overwrites `length` and `ctr` on a successful write; it does not change `t_tx`.

Typed writer: pass a concrete LinkType (0 is valid). Universal writer: `linktype` = `IOX2_CIF_PCAP_LINKTYPE_ANY` (`0xFFFFFFFF`). Typed subscriber: `pcap_sub_create` with that LinkType. Universal subscriber: `pcap_sub_open` (no LinkType). Typed-to-typed mismatch is **8106**. A typed subscriber may attach to a universal writer.

Create/destroy / `pcap_info` / `pcap_sub_open`: **UI thread**. Write/receive/stats/flush/has_sample/counts: any thread.

| C function | LabVIEW parameters |
|---|---|
| `iox2_cif_pcap_writer_create` | CStr name/topic/desc; U32 linktype, max_pcap_bytes, be_max_subscribers, bp_max_subscribers, be_subscriber_buffer, bp_subscriber_buffer; U64* writer. A max of 0 skips that publisher. One writer per (name, topic). |
| `iox2_cif_pcap_sub_create` | CStr name/topic; U32 linktype, timeout; I32 use_bp; U64* subscriber |
| `iox2_cif_pcap_sub_open` | CStr name/topic; I32 use_bp; U32 timeout; U64* — universal subscriber |
| `iox2_cif_pcap_info` | U64 port; type buf + cap + len; U32* linktype, max_pcap_bytes, be_subscriber_buffer, bp_subscriber_buffer |
| `iox2_cif_pcap_write` | U64; header Pointer to Value; pcap Array Data Pointer; U32 offset, length, timeout; I32* timed_out; U64* samples, timeouts |
| `iox2_cif_pcap_writer_stats` | U64; U64* samples, timeouts, last_ctr |
| `iox2_cif_pcap_writer_stats_reset` | U64 — does not rewind on-wire `ctr` |
| `iox2_cif_pcap_receive` | U64; header Pointer to Value; pcap Array Data Pointer; U32 cap, timeout; U32* actual, max; I32* timed_out; U64* samples, dropped, timeouts |
| `iox2_cif_pcap_stats` | U64; U64* samples, dropped, timeouts, last_ctr |
| `iox2_cif_pcap_stats_reset` | U64 |
| `iox2_cif_pcap_flush` | U64; U32* discarded — next receive times out if idle |
| `iox2_cif_pcap_has_sample` | U64; I32* has |
| `iox2_cif_pcap_writer_subscriber_count` | U64; U32* be, bp |
| `iox2_cif_pcap_destroy_writer` / `destroy_sub` | U64 |

## Linux RT (NI Linux Real-Time, x64)

Build the shared library on the RT target (needs SSH, gcc, and internet for the first iceoryx2/Rust compile):

```powershell
powershell -ExecutionPolicy Bypass -File D:\dev\iceoryx2_lvwrapper\scripts\build-linuxrt.ps1
```

That produces `dist\libiox2_cif.so` and also installs it on the target as `/usr/local/lib/libiox2_cif.so` and `/home/lvuser/libiox2_cif.so`.

LabVIEW Real-Time Call Library Function: library name `libiox2_cif.so` (or the full path), calling convention C, same prototypes as Windows. The RT UI-thread vs any-thread LoadLibrary deadlock does not apply the same way; still keep create/start/destroy off the hot path.
