# iceoryx2 CIF wrapper

C ABI (`iox2_cif`) for the **Composable Instrumentation Framework**: [iceoryx2](https://github.com/eclipse-iceoryx/iceoryx2) **blackboard** and **publish-subscribe** from C, C++, Python (FFI), or LabVIEW Call Library Function nodes. Blackboard: one iceoryx2 service per name, topics as FNV-1a-64 keys. Raw pub/sub: `iox2_cif_ps_*`. CIF-pcap streams: `iox2_cif_pcap_*` (48-byte header + pcap records).

- Public header: [`native/include/iox2_cif.h`](native/include/iox2_cif.h)
- C API notes: [`docs/C_API.md`](docs/C_API.md)
- Design: [`docs/DESIGN.md`](docs/DESIGN.md)
- LabVIEW CLFN wiring: [`docs/LABVIEW_PROTOTYPE.md`](docs/LABVIEW_PROTOTYPE.md)

Build (Windows DLL + smoke test):

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1
```

That writes `dist\windows\iox2_cif-windows-<version>.dll`. Linux RT: `scripts\build-linuxrt.ps1` → `dist\linuxrt\libiox2_cif-linuxrt.so.<version>`. Ubuntu 20.04: `scripts\build-ubuntu2004.ps1` → `dist\ubuntu\libiox2_cif-ubuntu.so.<version>`.

Native installers ([`Installers/readme.md`](Installers/readme.md)) install the versioned file and symlink the LabVIEW names:

- **Windows:** `C:\Program Files\CIF_Foundation\Libraries\iox2_cif-windows-<version>.dll` → `iox2_cif.dll`
- **Linux RT / Ubuntu:** `/usr/lib/cif/libiox2_cif-<platform>.so.<version>` → `/usr/lib/cif/libiox2_cif.so` and `/usr/lib/libiox2_cif.so`

Call Library Function nodes should use the library name only (`iox2_cif.dll` / `libiox2_cif.so`).

No RouDi or extra daemon. iceoryx2 is linked into the library.

---

## Concepts

| You pass | iceoryx2 stores |
|---|---|
| Blackboard **name** (e.g. `robot`) | Service name |
| **Topic** string (e.g. `pose`) | `uint64_t` key = FNV-1a-64 of the UTF-8 topic (no NUL) |
| **Type name** + **elem_size** + **elem_align** | Value schema. Bind fails unless all three match |
| **max_readers** | Concurrent Reader **ports** (all topics, all processes). Applied only on first create |

Handles are generation-counted `uint64_t`. `0` is invalid. Status is `int32_t`: `0` = OK, `8100–8199` = errors.

There is **one Writer port** per blackboard service in the whole system. Readers open by **name + topic**; they do not use the writer’s blackboard handle.

`iox2_cif_init` / `iox2_cif_shutdown` are optional. The first Start or Reader Create creates a process-wide Node and a background `node.wait()` thread.

---

## Call sequence

Staging (`Create`, `Writer Create`) does **no** iceoryx2 work. **Start** is the first service create/open. Writer Create after Start is an error (`8102`). You cannot add keys after the service exists.

### Writer process

```
iox2_cif_bb_create(name, max_readers)     → bb
  iox2_cif_bb_writer_create(bb, topic, …) → writer     (repeat per topic)
iox2_cif_bb_start(bb)                     → create or reopen the service
  loop: iox2_cif_bb_write(writer, elems, count)
iox2_cif_bb_destroy_writer(writer)        → optional, per topic
iox2_cif_bb_destroy(bb, &readers_left)    → drop Writer; remote readers may remain
iox2_cif_shutdown()                       → optional; tear down the process Node
```

### Reader process (same machine, same blackboard name)

Start the writer first, or pass a `timeout_ms` so the reader waits for the service.

```
iox2_cif_bb_reader_create(name, topic, type, size, align, is_iox2_cif_array, timeout_ms) → reader
  loop: iox2_cif_bb_read(reader, elems, cap, &actual, &max)
iox2_cif_bb_destroy_reader(reader)
```

`is_iox2_cif_array` means **this wrapper’s** array slot only, not “any array”:

- **1** — a CIF 1D array. Pass the element type (`i32`), element size/align. The wrapper prefixes `iox2_cif.array.` and reads `max_count` from the topic directory.
- **0** — bind the publisher’s type name, size, and alignment as-is. Use this for scalars, clusters, and arrays from other tools. For a foreign array, pass **their** type string and the **whole value** size/align (not our slot header).

Raw reader (`iox2_cif_bb_reader_create_raw`) takes only name, topic, and timeout; then use `iox2_cif_bb_read_raw` or `iox2_cif_bb_read_raw_at`.

### Start: create vs reopen

| Existing service | Staged topics | Writer slot | Result |
|---|---|---|---|
| None | — | — | **Create:** add every staged topic, take Writer, write initials |
| Present | Every staged topic exists with same type/size/align/max. Extra keys OK | Empty | **Reopen:** claim Writer, bind staged topics only. Do not rewrite values |
| Present | Match | Held | **8105** writer still attached |
| Present | Missing topic or schema differs | Any | **8106** schema mismatch |

If any bind fails, the Writer is dropped so the slot is not left claimed.

### I32 scalar example

Writer: `name=robot`, `max_readers=8`, topic `pose`, `type_name=i32`, `elem_size=4`, `elem_align=4`, `max_count=1`, `initial_count=1`, then Start, then Write with `count=1`.

Reader: same `robot` / `pose` / `i32` / `4` / `4`, `timeout_ms` e.g. `5000`, Read with `cap=1`.

---

## API overview

Every function returns `iox2_cif_status` except `iox2_cif_topic_key` (returns the `uint64_t` hash).

### Process Node

| Function | Role |
|---|---|
| `iox2_cif_init(node_name)` | Create the process Node if missing |
| `iox2_cif_shutdown()` | Drop the Node after all blackboards/readers are destroyed |
| `iox2_cif_status_text(status, buf, buf_len, out_len)` | Error text. `buf_len` / `out_len` are **bytes**; `out_len` is strlen without NUL. A 256-byte buffer always fits. For 8116 this includes the iceoryx2 detail |
| `iox2_cif_topic_key(topic)` | FNV-1a-64 of the topic; `0` is reserved |

### Stage and start

| Function | Role |
|---|---|
| `iox2_cif_bb_create(name, max_readers, &bb)` | Local staging object. `max_readers` is reader **ports** (1–4096), not payload size |
| `iox2_cif_bb_writer_create(bb, topic, type_name, elem_size, elem_align, max_count, initial, initial_count, descriptor, &writer)` | Stage one topic. `descriptor` is optional JSON (NULL or `""` ok). Must run **before** Start |
| `iox2_cif_bb_start(bb)` | Create or reopen the iceoryx2 service |

`elem_size` / `elem_align` are **bytes**. `max_count` / `initial_count` are **elements**. `max_count` is 1 for a scalar or cluster; `> 1` means a 1D array (capacity).

### Readers

| Function | Role |
|---|---|
| `iox2_cif_bb_reader_create(bb_name, topic, type_name, elem_size, elem_align, is_iox2_cif_array, timeout_ms, &reader)` | Typed reader. `is_iox2_cif_array`: I32 0 or 1 |
| `iox2_cif_bb_reader_create_raw(bb_name, topic, timeout_ms, &reader)` | No LabVIEW type check; discover layout from the service / key-0 directory |
| `iox2_cif_bb_topic_info(bb_name, topic, timeout_ms, type, type_cap, &type_len, desc, desc_cap, &desc_len)` | Any process: type string + JSON from the wrapper sidecar (not iceoryx2). Probe with cap 0 |

### Hot path (pointer + count)

| Function | Units |
|---|---|
| `iox2_cif_bb_write(writer, elems, count)` | `count` = **elements**. Scalars/clusters: `1` |
| `iox2_cif_bb_read(reader, elems, cap, &actual, &max)` | `cap` / `actual` / `max` = **elements**. `elems=NULL`, `cap=0` probes sizes |
| `iox2_cif_bb_read_raw(reader, bytes, cap_bytes, &actual_bytes, &max_bytes)` | All sizes in **bytes**. `cap_bytes` must cover the full used payload |
| `iox2_cif_bb_read_raw_at(reader, bytes, offset_bytes, cap_bytes, &actual_bytes, &max_bytes)` | Slice of the used payload. Clamps at the end; `actual_bytes` is bytes copied |

Write with `count > max_count` returns `8108` (no truncate).

### Teardown

| Function | Role |
|---|---|
| `iox2_cif_bb_destroy_writer(writer)` | Before Start: unstage. After Start: drop that entry handle; last value stays readable |
| `iox2_cif_bb_destroy_reader(reader)` | Free one reader port |
| `iox2_cif_bb_destroy(bb, &readers_remaining)` | Invalidate writers, drop the Writer port. Does not kick remote readers |

### Pub/sub

| Function | Role |
|---|---|
| `iox2_cif_ps_writer_create(...)` | Create the stream. `be_max_subscribers` or `bp_max_subscribers` of 0 skips that publisher |
| `iox2_cif_ps_sub_create(...)` | Open a subscriber. `use_backpressure` 0 or 1 |
| `iox2_cif_ps_write` / `iox2_cif_ps_receive` | Timeouts are `timed_out` outs, not status errors |
| `iox2_cif_ps_has_sample` / `iox2_cif_ps_writer_subscriber_count` | Non-blocking probes |
| `iox2_cif_ps_destroy_writer` / `iox2_cif_ps_destroy_sub` | Drop ports |
| `iox2_cif_ps_info` | Probe raw or pcap stream type / max / BE and BP queue depths / value_size |
| `iox2_cif_pcap_writer_create` | CIF-pcap writer (typed or universal). One writer per (name, topic) |
| `iox2_cif_pcap_sub_create` / `sub_open` | Typed or universal pcap subscriber |
| `iox2_cif_pcap_write` / `receive` | CIF header + pcap slice; `length`/`actual` are pcap bytes; `timed_out` not an error |
| `iox2_cif_pcap_write_raw` / `receive_raw` | Whole sample as a U8 array (header + records), for remote transport |
| `iox2_cif_pcap_stats` / `flush` | Reader metrics and queue drain |

Same `(name, topic)` must not be both a blackboard and a pub/sub stream. See [`docs/C_API.md`](docs/C_API.md).

---

## Types

Pass iceoryx2/Rust type names, not LabVIEW names (`I32`, `DBL`). iceoryx2 checks **type name + size + alignment**. The topic string is only used to find the key.

| LabVIEW data | `type_name` to pass | Stored iceoryx2 type | `elem_size` / `elem_align` |
|---|---|---|---|
| I8 I16 I32 I64 | `i8` `i16` `i32` `i64` | same | natural (I32: 4 / 4) |
| U8 U16 U32 U64 | `u8` `u16` `u32` `u64` | same | natural |
| SGL / DBL | `f32` / `f64` | same | 4/4 or 8/8 |
| Boolean | `bool` | `bool` | 1 / 1, values 0 or 1 (not C++ `bool`, not `u8`) |
| Flat cluster | e.g. `RobotPose` | same string | LabVIEW cluster size / align |
| 1D array of the above | **element** name (`i32`, `RobotPose`); `max_count` > 1 | `iox2_cif.array.<elem>` | element size / align; value align is `max(8, elem_align)` because of an 8-byte slot header |

Do not pass `(i32)` as an array marker. Parentheses are stored literally.

Array payload in SHM: `uint32_t count`, `uint32_t reserved`, then `max_count` elements. Only `[0, count)` is valid.

Other languages must use the same type name, size, and alignment (for arrays: `iox2_cif.array.i32` and size `8 + max_count * elem_size`, aligned). There is no iceoryx2/Python standard array name; `iox2_cif.array.*` is this wrapper’s contract.

A reader of a standard scalar may use a different standard scalar than the publisher. The entry opens as the published iceoryx2 type, and `iox2_cif_bb_read` converts (saturate; float-to-int rounds to nearest, ties to even). That path does not use the JSON descriptor.

A reader topic `topic.field` reads one cluster element. `field` is a `struct_elems` `name` in the writer's IDL JSON, which is required. The element's JSON `type` is the conversion source. The open fails if that topic is an array. `iox2_cif_bb_read_raw` still returns the full published bytes.

Not supported: strings, paths, variants, handles, nested arrays, 2D+ arrays, clusters that contain arrays or strings.

---

## Status codes

| Code | Meaning |
|---|---|
| 0 | OK |
| 8100 | Invalid argument |
| 8101 | Invalid / stale handle |
| 8102 | Writer Create after Start |
| 8103 | Write before Start |
| 8104 | Duplicate topic |
| 8105 | Writer slot still held |
| 8106 | Schema mismatch |
| 8107 | Topic not found |
| 8108 | Array write longer than `max_count` |
| 8109 | Reader / subscriber create timed out |
| 8110 | Type rejected (raw API given a pcap type) |
| 8115 | Unsupported (e.g. BP subscriber on a BE-only writer) |
| 8111 | Topic hashes to reserved key 0 |
| 8112 | Two topics share an FNV-1a key |
| 8116 | iceoryx2 / internal — use `iox2_cif_status_text` |
| 8117 | Wrapper lock timeout (~5 s) |
| 8118 | Maximum number of subscribers already attached |

---

## LabVIEW notes

64-bit only. Calling convention **C (cdecl)**. Return type **I32**. Strings: **C String Pointer**. Output handles: U64 **Pointer to Value**. Input handles: U64 **Value**. Install the native library with the packages in [`Installers/`](Installers/readme.md), then use library name `iox2_cif.dll` or `libiox2_cif.so`.

Create / Start / Destroy: **Run in UI thread** (first load of this fat DLL from an execution thread can deadlock in `LoadLibrary`). Leave **Unload after call** unchecked. The DLL is pinned so closing a VI does not unmap it while the wait thread is running.

Hot-path Write / Read can **Run in any thread**.
