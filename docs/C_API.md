# CIF C API (`iox2_cif`)

Public header: [`native/include/iox2_cif.h`](../native/include/iox2_cif.h). After `scripts/build.ps1`, the shared library is `dist/iox2_cif.dll` (Windows) or `dist/libiox2_cif.so` (Linux). Iceoryx2 is linked in; do not also link `iceoryx2-c`.

CIF = Composable Instrumentation Framework. This is a cdecl C ABI. LabVIEW, C, C++, Python (ctypes/cffi), and other FFIs call the same symbols.

## Conventions

- Return type is `iox2_cif_status` (`int32_t`): `0` = OK, `8100–8199` = errors. `iox2_cif_topic_key` returns a `uint64_t` hash.
- Handles are generation-counted `uint64_t`. `0` is invalid.
- Flags (`use_backpressure`, `is_iox2_cif_array`, `timed_out`, `has`) are `int32_t` 0 or 1, not C `_Bool`.
- Payload is pointer + count. Strings are UTF-8 C strings.
- One process-wide iceoryx2 Node (created lazily or via `iox2_cif_init`). Call `iox2_cif_shutdown` before a clean process exit if you want SHM torn down promptly.

## Blackboard

```
iox2_cif_bb_create(name, max_readers, &bb)
  iox2_cif_bb_writer_create(bb, topic, type_name, size, align, max_count, initial, n, desc, &w)
iox2_cif_bb_start(bb)
  iox2_cif_bb_write(w, elems, count)
iox2_cif_bb_destroy(bb, &readers_left)
```

Reader (any process):

```
iox2_cif_bb_reader_create(name, topic, type_name, size, align, is_iox2_cif_array, timeout_ms, &r)
  iox2_cif_bb_read(r, elems, cap, &actual, &max)
iox2_cif_bb_destroy_reader(r)
```

Type + JSON (sidecar, not iceoryx2): `iox2_cif_bb_topic_info`. Raw bytes: `iox2_cif_bb_read_raw` / `iox2_cif_bb_read_raw_at`.

## Pub/sub

```
iox2_cif_ps_writer_create(name, topic, type, size, align, max_count, desc,
                          be_max_subscribers, bp_max_subscribers,
                          be_subscriber_buffer, bp_subscriber_buffer, &w)
  iox2_cif_ps_write(w, elems, count, timeout_ms, &timed_out)
  iox2_cif_ps_writer_subscriber_count(w, &best_effort, &backpressure)
iox2_cif_ps_destroy_writer(w)
```

```
iox2_cif_ps_sub_create(name, topic, type, size, align, is_iox2_cif_array,
                       use_backpressure, timeout_ms, &s)
  iox2_cif_ps_receive(s, elems, cap, timeout_ms, &actual, &max, &timed_out)
  iox2_cif_ps_has_sample(s, &has)
iox2_cif_ps_destroy_sub(s)
```

Probe (raw or pcap): `iox2_cif_ps_info(name, topic, timeout_ms, type, cap, &len, &max_count, &be_subscriber_buffer, &bp_subscriber_buffer, &value_size)`. `bp_subscriber_buffer` is 0 when there is no BP service.

`be_max_subscribers` / `bp_max_subscribers` and `be_subscriber_buffer` / `bp_subscriber_buffer` are independent per queue. A typical setup is several BE ports and one shallow BP port. `0` for a max skips that publisher. Both maxes `0` is **8100**. A created side needs queue depth >= 1.

Payload chunks are shared. Best-effort reserves `be_subscriber_buffer + be_max_subscribers + 1` chunks. Backpressure reserves `bp_subscriber_buffer + 2`. A publish copies the payload once into one chunk.

Write sends the backpressure queue first when that publisher exists. If that wait expires, `timed_out=1` and best-effort is not sent. Receive sets `timed_out=1` whenever there is no valid sample (including `timeout_ms` 0). `timed_out=0` means the payload is valid. Receive timeouts are not a status error. `ps_sub_create` waiting for the stream still returns **8109**. The subscriber queue depth is taken from the existing service (`subscriber_max_buffer_size`), including publishers not created by this library. Raw create rejects `iox2_cif.pcap` / `iox2_cif.pcap.*` (**8110**).

Do not use the same `(name, topic)` for both a blackboard and a pub/sub stream.

## CIF-pcap pub/sub

Sample = 48-byte `iox2_cif_pcap_hdr_t` + pcap records (no global header). Type is `iox2_cif.pcap.<linktype>` or universal `iox2_cif.pcap` (`IOX2_CIF_PCAP_LINKTYPE_ANY` on writer create).

```
iox2_cif_pcap_writer_create(name, topic, linktype, max_pcap_bytes, desc,
                            be_max_subscribers, bp_max_subscribers,
                            be_subscriber_buffer, bp_subscriber_buffer, &w)
  iox2_cif_pcap_write(w, header, pcap, offset, length, timeout_ms, &timed_out, &samples, &timeouts)
  iox2_cif_pcap_writer_stats / writer_stats_reset / writer_subscriber_count
iox2_cif_pcap_destroy_writer(w)
```

```
iox2_cif_pcap_sub_create(name, topic, linktype, use_backpressure, timeout_ms, &s)
iox2_cif_pcap_sub_open(name, topic, use_backpressure, timeout_ms, &s)
  iox2_cif_pcap_receive(s, header, pcap, cap, timeout_ms, &actual, &max, &timed_out,
                        &samples, &dropped, &timeouts)
  iox2_cif_pcap_stats / stats_reset / flush / has_sample
iox2_cif_pcap_destroy_sub(s)
```

Write stamps `length` and `ctr` only (caller supplies `t0` / `t_out` / `t_tx` / `clock_id`). BP timeout does not increment `ctr` or send BE. Flush drains this subscriber queue only.

## On-wire names

These strings are the IPC contract. All languages must match.

- Array type: `iox2_cif.array.<elem>` (8-byte slot header, then elements)
- Pcap type: `iox2_cif.pcap.<linktype>` or universal `iox2_cif.pcap`
- Pub/sub services: `{name}/{topic}.iox2_cif.ps.be` and `.iox2_cif.ps.bp`
- Sidecar: `C:\Temp\iox2_cif\meta` (Windows), `/tmp/iox2_cif/meta` (Linux)
- Optional attribute: `iox2_cif.abi`

Primitive `type_name` values: `i8` `i16` `i32` `i64` `u8` `u16` `u32` `u64` `f32` `f64` `bool`. Clusters use a caller-supplied name.

## Status (common)

| Code | Meaning |
|---|---|
| 0 | OK |
| 8100 | Invalid argument |
| 8101 | Invalid / stale handle |
| 8105 | Writer slot still held |
| 8106 | Schema mismatch |
| 8109 | Create timed out waiting for the service |
| 8110 | Type rejected (e.g. raw API given an `iox2_cif.pcap` type) |
| 8115 | Unsupported (subscriber side was not created on that writer) |
| 8116 | Internal — `iox2_cif_status_text` |
| 8117 | Wrapper lock timeout |
| 8118 | Maximum number of subscribers already attached |

Full list is in the header enum `iox2_cif_status`.
