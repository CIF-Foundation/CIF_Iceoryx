/* C smoke test: I32 blackboard write/read in one process. */
#include "iox2_cif.h"

#include <stdio.h>
#include <string.h>

static int test_conversion(void);
static int test_pubsub(void);
static int test_pcap(void);
static int test_meta_cleanup(void);

static int fail(const char* where, iox2_cif_status st) {
    char text[128];
    iox2_cif_status_text(st, text, sizeof(text), NULL);
    fprintf(stderr, "FAIL %s: %d (%s)\n", where, (int)st, text);
    return 1;
}

int main(void) {
    uint64_t bb = 0;
    uint64_t writer = 0;
    uint64_t reader = 0;
    int32_t initial = 7;
    int32_t value = 42;
    int32_t got = 0;
    uint32_t actual = 0;
    uint32_t maxv = 0;
    iox2_cif_status st = IOX2_CIF_OK;
    uint64_t pose_key = iox2_cif_topic_key("pose");
    char text[64];

    if (pose_key == 0) {
        return fail("topic_key", IOX2_CIF_ERR_KEY_RESERVED);
    }

    st = iox2_cif_init("iox2_cif_smoke");
    if (st != IOX2_CIF_OK) {
        return fail("init", st);
    }

    st = iox2_cif_bb_create("iox2_cif_smoke_bb", 8, &bb);
    if (st != IOX2_CIF_OK) {
        return fail("bb_create", st);
    }

    st = iox2_cif_bb_writer_create(
        bb,
        "pose",
        IOX2_CIF_TYPE_I32,
        (uint32_t)sizeof(int32_t),
        (uint32_t)sizeof(int32_t),
        1,
        &initial,
        1,
        "{\"kind\":\"scalar\"}",
        &writer);
    if (st != IOX2_CIF_OK) {
        return fail("writer_create", st);
    }

    st = iox2_cif_bb_start(bb);
    if (st != IOX2_CIF_OK) {
        return fail("start", st);
    }

    st = iox2_cif_bb_reader_create("iox2_cif_smoke_bb", "pose", IOX2_CIF_TYPE_I32, (uint32_t)sizeof(int32_t), (uint32_t)sizeof(int32_t), 0, 2000, &reader);
    if (st != IOX2_CIF_OK) {
        return fail("reader_create", st);
    }

    {
        char type_buf[64];
        char desc_buf[64];
        uint32_t type_n = 0;
        uint32_t desc_n = 0;
        st = iox2_cif_bb_topic_info(
            "iox2_cif_smoke_bb", "pose", 2000, type_buf, sizeof(type_buf), &type_n, desc_buf, sizeof(desc_buf), &desc_n);
        if (st != IOX2_CIF_OK) {
            return fail("topic_info", st);
        }
        if (strcmp(type_buf, IOX2_CIF_TYPE_I32) != 0 || strcmp(desc_buf, "{\"kind\":\"scalar\"}") != 0) {
            fprintf(stderr, "FAIL topic_info type='%s' desc='%s'\n", type_buf, desc_buf);
            return 1;
        }
        st = iox2_cif_bb_topic_info(
            "iox2_cif_smoke_bb", "missing", 0, type_buf, sizeof(type_buf), &type_n, desc_buf, sizeof(desc_buf), &desc_n);
        if (st != IOX2_CIF_ERR_TOPIC_NOT_FOUND) {
            return fail("topic_info missing", st);
        }
    }

    st = iox2_cif_bb_read(reader, &got, 1, &actual, &maxv);
    if (st != IOX2_CIF_OK) {
        return fail("read_initial", st);
    }
    if (got != initial || actual != 1 || maxv != 1) {
        fprintf(stderr, "FAIL initial value got=%d actual=%u max=%u\n", got, actual, maxv);
        return 1;
    }

    st = iox2_cif_bb_write(writer, &value, 1);
    if (st != IOX2_CIF_OK) {
        return fail("write", st);
    }

    got = 0;
    st = iox2_cif_bb_read(reader, &got, 1, &actual, &maxv);
    if (st != IOX2_CIF_OK) {
        return fail("read", st);
    }
    if (got != value) {
        fprintf(stderr, "FAIL readback got=%d expected=%d\n", got, value);
        return 1;
    }

    iox2_cif_status_text(IOX2_CIF_OK, text, sizeof(text), NULL);
    iox2_cif_status_text(IOX2_CIF_ERR_LOCK_TIMEOUT, text, sizeof(text), NULL);
    if (strcmp(text, "lock timeout") != 0) {
        fprintf(stderr, "FAIL lock timeout text: %s\n", text);
        return 1;
    }
    iox2_cif_status_text(IOX2_CIF_OK, text, sizeof(text), NULL);
    printf("smoke ok: pose key=%llu wrote %d read %d (%s)\n", (unsigned long long)pose_key, value, got, text);

    iox2_cif_bb_destroy_reader(reader);
    iox2_cif_bb_destroy(bb, NULL);

    {
        uint64_t bb2 = 0;
        uint64_t w2 = 0;
        uint64_t r2 = 0;
        double initv[3] = {0.0, 0.0, 0.0};
        double src[3] = {1.25, -2.5, 3.0};
        double dst[3] = {0.0, 0.0, 0.0};
        uint32_t a = 0;
        uint32_t m = 0;
        st = iox2_cif_bb_create("iox2_cif_smoke_f64", 8, &bb2);
        if (st != IOX2_CIF_OK) {
            return fail("f64 bb_create", st);
        }
        st = iox2_cif_bb_writer_create(
            bb2, "samples", IOX2_CIF_TYPE_F64, 8, 8, 3, initv, 3, "", &w2);
        if (st != IOX2_CIF_OK) {
            return fail("f64 writer_create", st);
        }
        st = iox2_cif_bb_start(bb2);
        if (st != IOX2_CIF_OK) {
            return fail("f64 start", st);
        }
        {
            char type_buf[64];
            char desc_buf[8];
            uint32_t type_n = 0;
            uint32_t desc_n = 99;
            st = iox2_cif_bb_topic_info(
                "iox2_cif_smoke_f64",
                "samples",
                2000,
                type_buf,
                sizeof(type_buf),
                &type_n,
                desc_buf,
                sizeof(desc_buf),
                &desc_n);
            if (st != IOX2_CIF_OK) {
                return fail("f64 topic_info", st);
            }
            if (strcmp(type_buf, "iox2_cif.array.f64") != 0 || desc_n != 0 || desc_buf[0] != 0) {
                fprintf(stderr, "FAIL f64 topic_info type='%s' desc_n=%u\n", type_buf, desc_n);
                return 1;
            }
        }
        st = iox2_cif_bb_reader_create(
            "iox2_cif_smoke_f64", "samples", IOX2_CIF_TYPE_F64, 8, 8, 1, 2000, &r2);
        if (st != IOX2_CIF_OK) {
            return fail("f64 reader_create", st);
        }
        st = iox2_cif_bb_write(w2, src, 3);
        if (st != IOX2_CIF_OK) {
            return fail("f64 write", st);
        }
        st = iox2_cif_bb_read(r2, dst, 3, &a, &m);
        if (st != IOX2_CIF_OK) {
            return fail("f64 read", st);
        }
        if (a != 3 || m != 3 || dst[0] != src[0] || dst[1] != src[1] || dst[2] != src[2]) {
            fprintf(stderr, "FAIL f64 array actual=%u max=%u got=%g %g %g\n", a, m, dst[0], dst[1], dst[2]);
            return 1;
        }
        {
            double slice[3] = {0.0, 0.0, 0.0};
            uint32_t ab = 0;
            uint32_t mb = 0;
            st = iox2_cif_bb_read_raw_at(r2, slice, IOX2_CIF_SLOT_HEADER_SIZE, 16, &ab, &mb);
            if (st != IOX2_CIF_OK) {
                return fail("f64 read_raw_at", st);
            }
            if (ab != 16 || slice[0] != src[0] || slice[1] != src[1]) {
                fprintf(stderr, "FAIL read_raw_at mid actual=%u got=%g %g\n", ab, slice[0], slice[1]);
                return 1;
            }
            memset(slice, 0, sizeof(slice));
            st = iox2_cif_bb_read_raw_at(r2, slice, IOX2_CIF_SLOT_HEADER_SIZE, 100, &ab, &mb);
            if (st != IOX2_CIF_OK || ab != 24 || slice[0] != src[0] || slice[1] != src[1]
                || slice[2] != src[2]) {
                fprintf(stderr, "FAIL read_raw_at clamp actual=%u got=%g %g %g\n",
                    ab, slice[0], slice[1], slice[2]);
                return 1;
            }
            st = iox2_cif_bb_read_raw_at(r2, slice, 40, 16, &ab, &mb);
            if (st != IOX2_CIF_OK || ab != 0) {
                fprintf(stderr, "FAIL read_raw_at past end st=%d actual=%u\n", (int)st, ab);
                return 1;
            }
        }
        printf("smoke ok: f64[3] wrote/read 1.25 -2.5 3\n");
        iox2_cif_bb_destroy_reader(r2);
        iox2_cif_bb_destroy(bb2, NULL);
    }

    if (test_conversion() != 0) {
        iox2_cif_shutdown();
        return 1;
    }
    if (test_pubsub() != 0) {
        iox2_cif_shutdown();
        return 1;
    }
    if (test_pcap() != 0) {
        iox2_cif_shutdown();
        return 1;
    }
    if (test_meta_cleanup() != 0) {
        iox2_cif_shutdown();
        return 1;
    }

    iox2_cif_shutdown();
    return 0;
}

static int destroy_bb(uint64_t reader, uint64_t bb) {
    if (reader != 0) {
        iox2_cif_bb_destroy_reader(reader);
    }
    if (bb != 0) {
        iox2_cif_bb_destroy(bb, NULL);
    }
    return 0;
}

static int start_topic(
    const char* bb_name,
    const char* topic,
    const char* type_name,
    uint32_t elem_size,
    uint32_t elem_align,
    uint32_t max_count,
    const void* initial,
    uint32_t initial_count,
    const char* descriptor,
    uint64_t* bb,
    uint64_t* writer) {
    iox2_cif_status st = iox2_cif_bb_create(bb_name, 8, bb);
    if (st != IOX2_CIF_OK) {
        return fail(bb_name, st);
    }
    st = iox2_cif_bb_writer_create(
        *bb, topic, type_name, elem_size, elem_align, max_count, initial, initial_count, descriptor, writer);
    if (st != IOX2_CIF_OK) {
        destroy_bb(0, *bb);
        *bb = 0;
        return fail(topic, st);
    }
    st = iox2_cif_bb_start(*bb);
    if (st != IOX2_CIF_OK) {
        destroy_bb(0, *bb);
        *bb = 0;
        return fail("start", st);
    }
    return 0;
}

static int test_conversion(void) {
    uint64_t bb = 0;
    uint64_t writer = 0;
    uint64_t reader = 0;
    iox2_cif_status st = IOX2_CIF_OK;
    int32_t i32v = 42;
    double f64v = 0.0;
    uint32_t actual = 0;
    uint32_t maxv = 0;

    if (start_topic("iox2_cif_conv_i32", "n", IOX2_CIF_TYPE_I32, 4, 4, 1, &i32v, 1, "", &bb, &writer) != 0) {
        return 1;
    }
    st = iox2_cif_bb_reader_create("iox2_cif_conv_i32", "n", IOX2_CIF_TYPE_F64, 8, 8, 0, 2000, &reader);
    if (st != IOX2_CIF_OK) {
        destroy_bb(0, bb);
        return fail("i32 to f64 reader", st);
    }
    st = iox2_cif_bb_read(reader, &f64v, 1, &actual, &maxv);
    if (st != IOX2_CIF_OK || f64v != 42.0 || actual != 1) {
        fprintf(stderr, "FAIL i32 to f64 got=%g st=%d\n", f64v, (int)st);
        destroy_bb(reader, bb);
        return 1;
    }
    {
        uint8_t raw[8];
        uint32_t used = 0;
        uint32_t maxb = 0;
        memset(raw, 0, sizeof(raw));
        st = iox2_cif_bb_read_raw(reader, raw, sizeof(raw), &used, &maxb);
        if (st != IOX2_CIF_OK || used != 4 || memcmp(raw, &i32v, 4) != 0) {
            fprintf(stderr, "FAIL raw i32 bytes used=%u\n", used);
            destroy_bb(reader, bb);
            return 1;
        }
    }
    destroy_bb(reader, bb);
    reader = 0;
    bb = 0;

    f64v = 1.5;
    if (start_topic("iox2_cif_conv_f64", "n", IOX2_CIF_TYPE_F64, 8, 8, 1, &f64v, 1, NULL, &bb, &writer) != 0) {
        return 1;
    }
    st = iox2_cif_bb_reader_create("iox2_cif_conv_f64", "n", IOX2_CIF_TYPE_I32, 4, 4, 0, 2000, &reader);
    if (st != IOX2_CIF_OK) {
        destroy_bb(0, bb);
        return fail("f64 to i32 reader", st);
    }
    i32v = 0;
    st = iox2_cif_bb_read(reader, &i32v, 1, &actual, &maxv);
    if (st != IOX2_CIF_OK || i32v != 2) {
        fprintf(stderr, "FAIL 1.5 to i32 got=%d\n", i32v);
        destroy_bb(reader, bb);
        return 1;
    }
    f64v = 2.5;
    st = iox2_cif_bb_write(writer, &f64v, 1);
    i32v = 0;
    st = iox2_cif_bb_read(reader, &i32v, 1, &actual, &maxv);
    if (st != IOX2_CIF_OK || i32v != 2) {
        fprintf(stderr, "FAIL 2.5 to i32 got=%d\n", i32v);
        destroy_bb(reader, bb);
        return 1;
    }
    {
        uint64_t nan_bits = 0x7ff8000000000001ull;
        memcpy(&f64v, &nan_bits, sizeof(f64v));
        st = iox2_cif_bb_write(writer, &f64v, 1);
        i32v = 7;
        st = iox2_cif_bb_read(reader, &i32v, 1, &actual, &maxv);
        if (st != IOX2_CIF_OK || i32v != 0) {
            fprintf(stderr, "FAIL nan to i32 got=%d\n", i32v);
            destroy_bb(reader, bb);
            return 1;
        }
    }
    destroy_bb(reader, bb);
    reader = 0;
    bb = 0;

    f64v = 200.0;
    if (start_topic("iox2_cif_conv_sat", "n", IOX2_CIF_TYPE_F64, 8, 8, 1, &f64v, 1, "", &bb, &writer) != 0) {
        return 1;
    }
    st = iox2_cif_bb_reader_create("iox2_cif_conv_sat", "n", IOX2_CIF_TYPE_I8, 1, 1, 0, 2000, &reader);
    if (st != IOX2_CIF_OK) {
        destroy_bb(0, bb);
        return fail("f64 to i8 reader", st);
    }
    {
        int8_t got8 = 0;
        st = iox2_cif_bb_read(reader, &got8, 1, &actual, &maxv);
        if (st != IOX2_CIF_OK || got8 != 127) {
            fprintf(stderr, "FAIL 200 to i8 got=%d\n", (int)got8);
            destroy_bb(reader, bb);
            return 1;
        }
    }
    destroy_bb(reader, bb);
    reader = 0;
    bb = 0;

    i32v = 0;
    if (start_topic("iox2_cif_conv_bool", "n", IOX2_CIF_TYPE_I32, 4, 4, 1, &i32v, 1, "", &bb, &writer) != 0) {
        return 1;
    }
    st = iox2_cif_bb_reader_create("iox2_cif_conv_bool", "n", IOX2_CIF_TYPE_BOOL, 1, 1, 0, 2000, &reader);
    if (st != IOX2_CIF_OK) {
        destroy_bb(0, bb);
        return fail("i32 to bool reader", st);
    }
    {
        uint8_t b = 9;
        st = iox2_cif_bb_read(reader, &b, 1, &actual, &maxv);
        if (st != IOX2_CIF_OK || b != 0) {
            fprintf(stderr, "FAIL 0 to bool got=%u\n", (unsigned)b);
            destroy_bb(reader, bb);
            return 1;
        }
        i32v = 2;
        st = iox2_cif_bb_write(writer, &i32v, 1);
        b = 0;
        st = iox2_cif_bb_read(reader, &b, 1, &actual, &maxv);
        if (st != IOX2_CIF_OK || b != 1) {
            fprintf(stderr, "FAIL 2 to bool got=%u\n", (unsigned)b);
            destroy_bb(reader, bb);
            return 1;
        }
    }
    destroy_bb(reader, bb);
    reader = 0;
    bb = 0;

    {
        uint8_t one = 1;
        uint8_t gotu = 0;
        if (start_topic("iox2_cif_conv_bool_u8", "n", IOX2_CIF_TYPE_BOOL, 1, 1, 1, &one, 1, "", &bb, &writer) != 0) {
            return 1;
        }
        st = iox2_cif_bb_reader_create("iox2_cif_conv_bool_u8", "n", IOX2_CIF_TYPE_U8, 1, 1, 0, 2000, &reader);
        if (st != IOX2_CIF_OK) {
            destroy_bb(0, bb);
            return fail("bool to u8 reader", st);
        }
        st = iox2_cif_bb_read(reader, &gotu, 1, &actual, &maxv);
        if (st != IOX2_CIF_OK || gotu != 1) {
            fprintf(stderr, "FAIL bool to u8 got=%u\n", (unsigned)gotu);
            destroy_bb(reader, bb);
            return 1;
        }
        destroy_bb(reader, bb);
        reader = 0;
        bb = 0;
    }

    {
        int32_t init_i[2] = {1, 2};
        if (start_topic("iox2_cif_conv_arr", "samples", IOX2_CIF_TYPE_I32, 4, 4, 2, init_i, 2, "", &bb, &writer) != 0) {
            return 1;
        }
        st = iox2_cif_bb_reader_create("iox2_cif_conv_arr", "samples", IOX2_CIF_TYPE_F64, 8, 8, 1, 2000, &reader);
        if (st != IOX2_CIF_ERR_SCHEMA_MISMATCH) {
            destroy_bb(reader, bb);
            return fail("array type mismatch", st);
        }
        destroy_bb(0, bb);
        bb = 0;
    }

    {
        uint8_t blob[4] = {0, 0, 0, 0};
        if (start_topic("iox2_cif_conv_empty", "pose", "RobotPose", 4, 4, 1, blob, 1, "", &bb, &writer) != 0) {
            return 1;
        }
        st = iox2_cif_bb_reader_create("iox2_cif_conv_empty", "pose.x", IOX2_CIF_TYPE_F64, 8, 8, 0, 2000, &reader);
        if (st != IOX2_CIF_ERR_SCHEMA_MISMATCH) {
            destroy_bb(reader, bb);
            return fail("empty descriptor field", st);
        }
        destroy_bb(0, bb);
        bb = 0;
    }

    {
        uint8_t blob[12];
        int32_t x = 42;
        double y = 1.5;
        const char* desc =
            "{\"Endian\":\"little\",\"Array\":false,\"Element Size\":12,\"struct_elems\":["
            "{\"name\":\"x\",\"type\":\"i32\",\"size\":4,\"offset\":0},"
            "{\"name\":\"y\",\"type\":\"f64\",\"size\":8,\"offset\":4}]}";
        memset(blob, 0, sizeof(blob));
        memcpy(blob, &x, 4);
        memcpy(blob + 4, &y, 8);
        if (start_topic("iox2_cif_conv_pose", "pose", "RobotPose", 12, 4, 1, blob, 1, desc, &bb, &writer) != 0) {
            return 1;
        }
        st = iox2_cif_bb_reader_create("iox2_cif_conv_pose", "pose.x", IOX2_CIF_TYPE_F64, 8, 8, 0, 2000, &reader);
        if (st != IOX2_CIF_OK) {
            destroy_bb(0, bb);
            return fail("pose.x reader", st);
        }
        f64v = 0.0;
        st = iox2_cif_bb_read(reader, &f64v, 1, &actual, &maxv);
        if (st != IOX2_CIF_OK || f64v != 42.0) {
            fprintf(stderr, "FAIL pose.x got=%g st=%d\n", f64v, (int)st);
            destroy_bb(reader, bb);
            return 1;
        }
        iox2_cif_bb_destroy_reader(reader);
        reader = 0;
        st = iox2_cif_bb_reader_create("iox2_cif_conv_pose", "pose.y", IOX2_CIF_TYPE_I32, 4, 4, 0, 2000, &reader);
        if (st != IOX2_CIF_OK) {
            destroy_bb(0, bb);
            return fail("pose.y reader", st);
        }
        i32v = 0;
        st = iox2_cif_bb_read(reader, &i32v, 1, &actual, &maxv);
        if (st != IOX2_CIF_OK || i32v != 2) {
            fprintf(stderr, "FAIL pose.y got=%d st=%d\n", i32v, (int)st);
            destroy_bb(reader, bb);
            return 1;
        }
        destroy_bb(reader, bb);
        reader = 0;
        bb = 0;
    }

    {
        uint8_t blob[4] = {1, 0, 0, 0};
        const char* desc =
            "{\"Array\":true,\"struct_elems\":[{\"name\":\"x\",\"type\":\"i32\",\"size\":4,\"offset\":0}]}";
        if (start_topic("iox2_cif_conv_arrfield", "pose", "RobotPose", 4, 4, 1, blob, 1, desc, &bb, &writer) != 0) {
            return 1;
        }
        st = iox2_cif_bb_reader_create("iox2_cif_conv_arrfield", "pose.x", IOX2_CIF_TYPE_I32, 4, 4, 0, 2000, &reader);
        if (st != IOX2_CIF_ERR_SCHEMA_MISMATCH) {
            destroy_bb(reader, bb);
            return fail("array field", st);
        }
        destroy_bb(0, bb);
        bb = 0;
    }

    {
        uint8_t blob[4] = {0x00, 0x00, 0x00, 0x2A};
        const char* desc =
            "{\"Endian\":\"big\",\"Array\":false,\"struct_elems\":["
            "{\"name\":\"v\",\"type\":\"i32\",\"size\":4,\"offset\":0}]}";
        if (start_topic("iox2_cif_conv_be", "bei", "BeI32", 4, 4, 1, blob, 1, desc, &bb, &writer) != 0) {
            return 1;
        }
        st = iox2_cif_bb_reader_create("iox2_cif_conv_be", "bei.v", IOX2_CIF_TYPE_I32, 4, 4, 0, 2000, &reader);
        if (st != IOX2_CIF_OK) {
            destroy_bb(0, bb);
            return fail("big-endian reader", st);
        }
        i32v = 0;
        st = iox2_cif_bb_read(reader, &i32v, 1, &actual, &maxv);
        if (st != IOX2_CIF_OK || i32v != 42) {
            fprintf(stderr, "FAIL big-endian i32 got=%d st=%d\n", i32v, (int)st);
            destroy_bb(reader, bb);
            return 1;
        }
        destroy_bb(reader, bb);
    }

    printf("smoke ok: scalar conversion and cluster fields\n");
    return 0;
}

static int ps_recv(
    uint64_t sub,
    int32_t* out,
    uint32_t timeout_ms,
    int32_t* timed_out,
    uint32_t* actual) {
    uint32_t maxv = 0;
    return iox2_cif_ps_receive(sub, out, 1, timeout_ms, actual, &maxv, timed_out);
}

static int test_pubsub(void) {
    uint64_t writer = 0;
    uint64_t be = 0;
    uint64_t bp = 0;
    uint32_t n_be = 99;
    uint32_t n_bp = 99;
    int32_t timed_out = 0;
    int32_t has = 0;
    int32_t value = 0;
    int32_t got = 0;
    uint32_t actual = 0;
    iox2_cif_status st = IOX2_CIF_OK;

    st = iox2_cif_ps_writer_create(
        "iox2_cif_smoke_ps",
        "samples",
        IOX2_CIF_TYPE_I32,
        4,
        4,
        1,
        "{\"kind\":\"scalar\"}",
        4,
        1,
        3,
        1,
        &writer);
    if (st != IOX2_CIF_OK) {
        return fail("ps writer_create", st);
    }

    st = iox2_cif_ps_writer_subscriber_count(writer, &n_be, &n_bp);
    if (st != IOX2_CIF_OK || n_be != 0 || n_bp != 0) {
        fprintf(stderr, "FAIL ps counts empty be=%u bp=%u st=%d\n", n_be, n_bp, (int)st);
        iox2_cif_ps_destroy_writer(writer);
        return 1;
    }

    st = iox2_cif_ps_sub_create(
        "iox2_cif_smoke_ps", "samples", IOX2_CIF_TYPE_I32, 4, 4, 0, 0, 2000, &be);
    if (st != IOX2_CIF_OK) {
        iox2_cif_ps_destroy_writer(writer);
        return fail("ps be sub", st);
    }
    st = iox2_cif_ps_sub_create(
        "iox2_cif_smoke_ps", "samples", IOX2_CIF_TYPE_I32, 4, 4, 0, 1, 2000, &bp);
    if (st != IOX2_CIF_OK) {
        iox2_cif_ps_destroy_sub(be);
        iox2_cif_ps_destroy_writer(writer);
        return fail("ps bp sub", st);
    }

    st = iox2_cif_ps_writer_subscriber_count(writer, &n_be, &n_bp);
    if (st != IOX2_CIF_OK || n_be != 1 || n_bp != 1) {
        fprintf(stderr, "FAIL ps counts 1/1 be=%u bp=%u st=%d\n", n_be, n_bp, (int)st);
        iox2_cif_ps_destroy_sub(be);
        iox2_cif_ps_destroy_sub(bp);
        iox2_cif_ps_destroy_writer(writer);
        return 1;
    }

    value = 100;
    timed_out = 1;
    st = iox2_cif_ps_write(writer, &value, 1, 200, &timed_out);
    if (st != IOX2_CIF_OK || timed_out != 0) {
        fprintf(stderr, "FAIL ps write 100 st=%d timed_out=%d\n", (int)st, (int)timed_out);
        iox2_cif_ps_destroy_sub(be);
        iox2_cif_ps_destroy_sub(bp);
        iox2_cif_ps_destroy_writer(writer);
        return 1;
    }
    got = 0;
    actual = 0;
    timed_out = 1;
    st = ps_recv(be, &got, 500, &timed_out, &actual);
    if (st != IOX2_CIF_OK || timed_out != 0 || actual != 1 || got != 100) {
        fprintf(stderr, "FAIL ps be recv 100 st=%d to=%d actual=%u got=%d\n", (int)st, (int)timed_out, actual, got);
        iox2_cif_ps_destroy_sub(be);
        iox2_cif_ps_destroy_sub(bp);
        iox2_cif_ps_destroy_writer(writer);
        return 1;
    }
    got = 0;
    st = ps_recv(bp, &got, 500, &timed_out, &actual);
    if (st != IOX2_CIF_OK || timed_out != 0 || got != 100) {
        fprintf(stderr, "FAIL ps bp recv 100 st=%d to=%d got=%d\n", (int)st, (int)timed_out, got);
        iox2_cif_ps_destroy_sub(be);
        iox2_cif_ps_destroy_sub(bp);
        iox2_cif_ps_destroy_writer(writer);
        return 1;
    }

    value = 200;
    timed_out = 1;
    st = iox2_cif_ps_write(writer, &value, 1, 200, &timed_out);
    if (st != IOX2_CIF_OK || timed_out != 0) {
        iox2_cif_ps_destroy_sub(be);
        iox2_cif_ps_destroy_sub(bp);
        iox2_cif_ps_destroy_writer(writer);
        return fail("ps write 200", st);
    }

    value = 300;
    timed_out = 0;
    st = iox2_cif_ps_write(writer, &value, 1, 80, &timed_out);
    if (st != IOX2_CIF_OK || timed_out != 1) {
        fprintf(stderr, "FAIL ps bp timeout write st=%d timed_out=%d\n", (int)st, (int)timed_out);
        iox2_cif_ps_destroy_sub(be);
        iox2_cif_ps_destroy_sub(bp);
        iox2_cif_ps_destroy_writer(writer);
        return 1;
    }

    got = 0;
    actual = 0;
    timed_out = 1;
    st = ps_recv(be, &got, 0, &timed_out, &actual);
    if (st != IOX2_CIF_OK || timed_out != 0 || actual != 1 || got != 200) {
        fprintf(stderr, "FAIL ps be after timeout got=%d actual=%u to=%d (want 200)\n", got, actual, (int)timed_out);
        iox2_cif_ps_destroy_sub(be);
        iox2_cif_ps_destroy_sub(bp);
        iox2_cif_ps_destroy_writer(writer);
        return 1;
    }
    got = 0;
    st = ps_recv(bp, &got, 0, &timed_out, &actual);
    if (st != IOX2_CIF_OK || timed_out != 0 || actual != 1 || got != 200) {
        fprintf(stderr, "FAIL ps bp after timeout got=%d actual=%u (want 200)\n", got, actual);
        iox2_cif_ps_destroy_sub(be);
        iox2_cif_ps_destroy_sub(bp);
        iox2_cif_ps_destroy_writer(writer);
        return 1;
    }

    st = iox2_cif_ps_has_sample(be, &has);
    if (st != IOX2_CIF_OK || has != 0) {
        fprintf(stderr, "FAIL ps has_sample after drain has=%d\n", (int)has);
        iox2_cif_ps_destroy_sub(be);
        iox2_cif_ps_destroy_sub(bp);
        iox2_cif_ps_destroy_writer(writer);
        return 1;
    }

    value = 400;
    timed_out = 1;
    st = iox2_cif_ps_write(writer, &value, 1, 200, &timed_out);
    if (st != IOX2_CIF_OK || timed_out != 0) {
        iox2_cif_ps_destroy_sub(be);
        iox2_cif_ps_destroy_sub(bp);
        iox2_cif_ps_destroy_writer(writer);
        return fail("ps write 400", st);
    }
    got = 0;
    st = ps_recv(be, &got, 500, &timed_out, &actual);
    if (st != IOX2_CIF_OK || got != 400) {
        fprintf(stderr, "FAIL ps be recv 400 got=%d\n", got);
        iox2_cif_ps_destroy_sub(be);
        iox2_cif_ps_destroy_sub(bp);
        iox2_cif_ps_destroy_writer(writer);
        return 1;
    }
    got = 0;
    st = ps_recv(bp, &got, 500, &timed_out, &actual);
    if (st != IOX2_CIF_OK || got != 400) {
        fprintf(stderr, "FAIL ps bp recv 400 got=%d\n", got);
        iox2_cif_ps_destroy_sub(be);
        iox2_cif_ps_destroy_sub(bp);
        iox2_cif_ps_destroy_writer(writer);
        return 1;
    }

    timed_out = 0;
    got = 7;
    actual = 9;
    st = ps_recv(be, &got, 40, &timed_out, &actual);
    if (st != IOX2_CIF_OK || timed_out != 1 || actual != 0) {
        fprintf(stderr, "FAIL ps recv timeout st=%d to=%d actual=%u\n", (int)st, (int)timed_out, actual);
        iox2_cif_ps_destroy_sub(be);
        iox2_cif_ps_destroy_sub(bp);
        iox2_cif_ps_destroy_writer(writer);
        return 1;
    }
    timed_out = 1;
    actual = 9;
    st = ps_recv(be, &got, 0, &timed_out, &actual);
    if (st != IOX2_CIF_OK || timed_out != 0 || actual != 0) {
        fprintf(stderr, "FAIL ps recv empty one-shot to=%d actual=%u\n", (int)timed_out, actual);
        iox2_cif_ps_destroy_sub(be);
        iox2_cif_ps_destroy_sub(bp);
        iox2_cif_ps_destroy_writer(writer);
        return 1;
    }

    iox2_cif_ps_destroy_sub(be);
    be = 0;
    st = iox2_cif_ps_writer_subscriber_count(writer, &n_be, &n_bp);
    if (st != IOX2_CIF_OK || n_be != 0 || n_bp != 1) {
        fprintf(stderr, "FAIL ps counts after be destroy be=%u bp=%u\n", n_be, n_bp);
        iox2_cif_ps_destroy_sub(bp);
        iox2_cif_ps_destroy_writer(writer);
        return 1;
    }
    iox2_cif_ps_destroy_sub(bp);
    bp = 0;
    st = iox2_cif_ps_writer_subscriber_count(writer, &n_be, &n_bp);
    if (st != IOX2_CIF_OK || n_be != 0 || n_bp != 0) {
        fprintf(stderr, "FAIL ps counts after both destroy be=%u bp=%u\n", n_be, n_bp);
        iox2_cif_ps_destroy_writer(writer);
        return 1;
    }

    {
        uint64_t be_only = 0;
        uint64_t bad = 0;
        st = iox2_cif_ps_sub_create(
            "iox2_cif_smoke_ps_beonly", "x", IOX2_CIF_TYPE_I32, 4, 4, 0, 1, 50, &bad);
        if (st != IOX2_CIF_ERR_TIMEOUT) {
            if (bad != 0) {
                iox2_cif_ps_destroy_sub(bad);
            }
            iox2_cif_ps_destroy_writer(writer);
            return fail("ps missing stream", st);
        }
        st = iox2_cif_ps_writer_create(
            "iox2_cif_smoke_ps_beonly",
            "x",
            IOX2_CIF_TYPE_I32,
            4,
            4,
            1,
            "",
            2,
            0,
            1,
            0,
            &be_only);
        if (st != IOX2_CIF_OK) {
            iox2_cif_ps_destroy_writer(writer);
            return fail("ps be-only writer", st);
        }
        st = iox2_cif_ps_sub_create(
            "iox2_cif_smoke_ps_beonly", "x", IOX2_CIF_TYPE_I32, 4, 4, 0, 1, 200, &bad);
        if (st != IOX2_CIF_ERR_UNSUPPORTED) {
            if (bad != 0) {
                iox2_cif_ps_destroy_sub(bad);
            }
            iox2_cif_ps_destroy_writer(be_only);
            iox2_cif_ps_destroy_writer(writer);
            return fail("ps bp on be-only", st);
        }
        iox2_cif_ps_destroy_writer(be_only);
    }

    {
        uint64_t none = 0;
        uint64_t bp_only = 0;
        uint64_t be_sub = 0;
        uint64_t bp_sub = 0;
        int32_t got_bp = 0;
        st = iox2_cif_ps_writer_create(
            "iox2_cif_smoke_ps_neither", "x", IOX2_CIF_TYPE_I32, 4, 4, 1, "", 0, 0, 0, 0, &none);
        if (st != IOX2_CIF_ERR_INVALID_ARGUMENT) {
            if (none != 0) {
                iox2_cif_ps_destroy_writer(none);
            }
            iox2_cif_ps_destroy_writer(writer);
            return fail("ps both max 0", st);
        }
        st = iox2_cif_ps_writer_create(
            "iox2_cif_smoke_ps_bponly", "x", IOX2_CIF_TYPE_I32, 4, 4, 1, "", 0, 1, 0, 1, &bp_only);
        if (st != IOX2_CIF_OK) {
            iox2_cif_ps_destroy_writer(writer);
            return fail("ps bp-only writer", st);
        }
        st = iox2_cif_ps_sub_create(
            "iox2_cif_smoke_ps_bponly", "x", IOX2_CIF_TYPE_I32, 4, 4, 0, 0, 200, &be_sub);
        if (st != IOX2_CIF_ERR_UNSUPPORTED) {
            if (be_sub != 0) {
                iox2_cif_ps_destroy_sub(be_sub);
            }
            iox2_cif_ps_destroy_writer(bp_only);
            iox2_cif_ps_destroy_writer(writer);
            return fail("ps be on bp-only", st);
        }
        st = iox2_cif_ps_sub_create(
            "iox2_cif_smoke_ps_bponly", "x", IOX2_CIF_TYPE_I32, 4, 4, 0, 1, 2000, &bp_sub);
        if (st != IOX2_CIF_OK) {
            iox2_cif_ps_destroy_writer(bp_only);
            iox2_cif_ps_destroy_writer(writer);
            return fail("ps bp-only sub", st);
        }
        value = 77;
        timed_out = 1;
        st = iox2_cif_ps_write(bp_only, &value, 1, 200, &timed_out);
        if (st != IOX2_CIF_OK || timed_out != 0) {
            iox2_cif_ps_destroy_sub(bp_sub);
            iox2_cif_ps_destroy_writer(bp_only);
            iox2_cif_ps_destroy_writer(writer);
            return fail("ps bp-only write", st);
        }
        st = ps_recv(bp_sub, &got_bp, 200, &timed_out, &actual);
        if (st != IOX2_CIF_OK || got_bp != 77) {
            fprintf(stderr, "FAIL ps bp-only recv got=%d st=%d\n", (int)got_bp, (int)st);
            iox2_cif_ps_destroy_sub(bp_sub);
            iox2_cif_ps_destroy_writer(bp_only);
            iox2_cif_ps_destroy_writer(writer);
            return 1;
        }
        iox2_cif_ps_destroy_sub(bp_sub);
        iox2_cif_ps_destroy_writer(bp_only);
    }

    {
        char type_buf[64];
        uint32_t type_n = 0;
        uint32_t maxc = 0;
        uint32_t be_buf = 0;
        uint32_t bp_buf = 0;
        uint32_t vsz = 0;
        st = iox2_cif_ps_info(
            "iox2_cif_smoke_ps",
            "samples",
            2000,
            type_buf,
            sizeof(type_buf),
            &type_n,
            &maxc,
            &be_buf,
            &bp_buf,
            &vsz);
        if (st != IOX2_CIF_OK || strcmp(type_buf, IOX2_CIF_TYPE_I32) != 0 || maxc != 1 || vsz != 4
            || be_buf != 3 || bp_buf != 1) {
            fprintf(stderr, "FAIL ps_info type='%s' max=%u vsz=%u be=%u bp=%u st=%d\n",
                type_buf, maxc, vsz, be_buf, bp_buf, (int)st);
            iox2_cif_ps_destroy_writer(writer);
            return 1;
        }
    }

    iox2_cif_ps_destroy_writer(writer);
    printf("smoke ok: pub/sub BE+BP counts and backpressure timeout\n");
    return 0;
}

static void pcap_cleanup(uint64_t writer, uint64_t a, uint64_t b) {
    if (a != 0) {
        iox2_cif_pcap_destroy_sub(a);
    }
    if (b != 0) {
        iox2_cif_pcap_destroy_sub(b);
    }
    if (writer != 0) {
        iox2_cif_pcap_destroy_writer(writer);
    }
}

static int test_pcap(void) {
    uint64_t writer = 0;
    uint64_t be = 0;
    uint64_t bp = 0;
    uint64_t extra = 0;
    iox2_cif_pcap_hdr_t in;
    iox2_cif_pcap_hdr_t out;
    uint8_t src[16];
    uint8_t got[16];
    uint32_t actual = 0;
    uint32_t maxv = 0;
    int32_t timed_out = 0;
    int32_t has = 0;
    uint64_t samples = 0;
    uint64_t dropped = 0;
    uint64_t timeouts = 0;
    uint64_t last_ctr = 0;
    uint64_t ws = 0;
    uint64_t wt = 0;
    uint64_t wctr = 0;
    uint32_t discarded = 0;
    uint32_t n_be = 0;
    uint32_t n_bp = 0;
    iox2_cif_status st = IOX2_CIF_OK;
    const int64_t tx = 123456789LL;

    memset(&in, 0, sizeof(in));
    in.t0 = 100;
    in.t_out = 200;
    in.t_tx = tx;
    in.clock_id = 7;
    memset(src, 0, sizeof(src));
    src[0] = 9;
    src[1] = 8;
    src[2] = 7;
    src[3] = 6;
    src[4] = 5;

    st = iox2_cif_ps_writer_create(
        "iox2_cif_smoke_pcap",
        "ether",
        "iox2_cif.pcap.1",
        1,
        1,
        1,
        "",
        2,
        0,
        1,
        0,
        &extra);
    if (st != IOX2_CIF_ERR_TYPE_REJECTED) {
        if (extra != 0) {
            iox2_cif_ps_destroy_writer(extra);
        }
        return fail("raw pcap type reject", st);
    }

    st = iox2_cif_pcap_sub_create(
        "iox2_cif_smoke_pcap", "ether", IOX2_CIF_PCAP_LINKTYPE_ANY, 0, 50, &extra);
    if (st != IOX2_CIF_ERR_INVALID_ARGUMENT) {
        pcap_cleanup(0, extra, 0);
        return fail("pcap sub ANY create", st);
    }

    st = iox2_cif_pcap_writer_create(
        "iox2_cif_smoke_pcap", "ether", 1, 64, "{\"kind\":\"pcap\"}", 4, 1, 4, 1, &writer);
    if (st != IOX2_CIF_OK) {
        return fail("pcap writer_create", st);
    }

    {
        char type_buf[64];
        uint32_t type_n = 0;
        uint32_t lt = 0;
        uint32_t maxb = 0;
        uint32_t be_buf = 0;
        uint32_t bp_buf = 0;
        st = iox2_cif_pcap_info(writer, type_buf, sizeof(type_buf), &type_n, &lt, &maxb, &be_buf, &bp_buf);
        if (st != IOX2_CIF_OK || strcmp(type_buf, "iox2_cif.pcap.1") != 0 || lt != 1 || maxb != 64
            || be_buf != 4 || bp_buf != 1) {
            fprintf(stderr, "FAIL pcap_info type='%s' lt=%u max=%u be=%u bp=%u st=%d\n",
                type_buf, lt, maxb, be_buf, bp_buf, (int)st);
            pcap_cleanup(writer, 0, 0);
            return 1;
        }
        st = iox2_cif_ps_info(
            "iox2_cif_smoke_pcap",
            "ether",
            2000,
            type_buf,
            sizeof(type_buf),
            &type_n,
            &maxb,
            &be_buf,
            &bp_buf,
            &lt);
        if (st != IOX2_CIF_OK || strcmp(type_buf, "iox2_cif.pcap.1") != 0 || be_buf != 4 || bp_buf != 1) {
            pcap_cleanup(writer, 0, 0);
            return fail("ps_info pcap", st);
        }
    }

    st = iox2_cif_pcap_sub_create("iox2_cif_smoke_pcap", "ether", 105, 0, 200, &extra);
    if (st != IOX2_CIF_ERR_SCHEMA_MISMATCH) {
        pcap_cleanup(writer, extra, 0);
        return fail("pcap typed mismatch", st);
    }

    st = iox2_cif_pcap_sub_create("iox2_cif_smoke_pcap", "ether", 1, 0, 2000, &be);
    if (st != IOX2_CIF_OK) {
        pcap_cleanup(writer, 0, 0);
        return fail("pcap be sub", st);
    }
    st = iox2_cif_pcap_sub_create("iox2_cif_smoke_pcap", "ether", 1, 1, 2000, &bp);
    if (st != IOX2_CIF_OK) {
        pcap_cleanup(writer, be, 0);
        return fail("pcap bp sub", st);
    }

    st = iox2_cif_pcap_writer_subscriber_count(writer, &n_be, &n_bp);
    if (st != IOX2_CIF_OK || n_be != 1 || n_bp != 1) {
        fprintf(stderr, "FAIL pcap counts be=%u bp=%u\n", n_be, n_bp);
        pcap_cleanup(writer, be, bp);
        return 1;
    }

    memset(&out, 0, sizeof(out));
    memset(got, 0, sizeof(got));
    timed_out = 1;
    st = iox2_cif_pcap_write(writer, &in, src, 2, 3, 200, &timed_out, &samples, &timeouts);
    if (st != IOX2_CIF_OK || timed_out != 0 || samples != 1 || timeouts != 0) {
        fprintf(stderr, "FAIL pcap write st=%d to=%d samples=%llu to=%llu\n",
            (int)st, (int)timed_out, (unsigned long long)samples, (unsigned long long)timeouts);
        pcap_cleanup(writer, be, bp);
        return 1;
    }
    actual = 0;
    st = iox2_cif_pcap_receive(
        be, &out, got, sizeof(got), 500, &actual, &maxv, &timed_out, &samples, &dropped, &timeouts);
    if (st != IOX2_CIF_OK || timed_out != 0 || actual != 3 || out.length != 51 || out.ctr != 0
        || out.t_tx != tx || out.clock_id != 7 || got[0] != 7 || got[1] != 6 || got[2] != 5
        || samples != 1 || dropped != 0) {
        fprintf(stderr, "FAIL pcap be recv len=%llu ctr=%llu actual=%u got=%u %u %u samples=%llu\n",
            (unsigned long long)out.length,
            (unsigned long long)out.ctr,
            actual,
            got[0],
            got[1],
            got[2],
            (unsigned long long)samples);
        pcap_cleanup(writer, be, bp);
        return 1;
    }
    memset(&out, 0, sizeof(out));
    st = iox2_cif_pcap_receive(
        bp, &out, got, sizeof(got), 500, &actual, &maxv, &timed_out, &samples, &dropped, &timeouts);
    if (st != IOX2_CIF_OK || out.ctr != 0 || actual != 3) {
        pcap_cleanup(writer, be, bp);
        return fail("pcap bp recv", st);
    }

    st = iox2_cif_pcap_writer_stats(writer, &ws, &wt, &wctr);
    if (st != IOX2_CIF_OK || ws != 1 || wt != 0 || wctr != 0) {
        fprintf(stderr, "FAIL pcap writer_stats ws=%llu wt=%llu ctr=%llu\n",
            (unsigned long long)ws, (unsigned long long)wt, (unsigned long long)wctr);
        pcap_cleanup(writer, be, bp);
        return 1;
    }

    timed_out = 1;
    st = iox2_cif_pcap_write(writer, &in, src, 0, 1, 200, &timed_out, &samples, &timeouts);
    if (st != IOX2_CIF_OK || timed_out != 0 || samples != 2) {
        fprintf(stderr, "FAIL pcap fill write st=%d to=%d samples=%llu\n",
            (int)st, (int)timed_out, (unsigned long long)samples);
        pcap_cleanup(writer, be, bp);
        return 1;
    }
    timed_out = 0;
    st = iox2_cif_pcap_write(writer, &in, src, 0, 1, 80, &timed_out, &samples, &timeouts);
    if (st != IOX2_CIF_OK || timed_out != 1 || samples != 2 || timeouts != 1) {
        fprintf(stderr, "FAIL pcap bp timeout write st=%d to=%d samples=%llu timeouts=%llu\n",
            (int)st, (int)timed_out, (unsigned long long)samples, (unsigned long long)timeouts);
        pcap_cleanup(writer, be, bp);
        return 1;
    }
    st = iox2_cif_pcap_has_sample(be, &has);
    if (st != IOX2_CIF_OK || has != 1) {
        fprintf(stderr, "FAIL pcap BE still holds last accepted sample has=%d\n", (int)has);
        pcap_cleanup(writer, be, bp);
        return 1;
    }

    discarded = 99;
    st = iox2_cif_pcap_flush(bp, &discarded);
    if (st != IOX2_CIF_OK || discarded != 1) {
        fprintf(stderr, "FAIL pcap flush discarded=%u st=%d\n", discarded, (int)st);
        pcap_cleanup(writer, be, bp);
        return 1;
    }
    discarded = 0;
    st = iox2_cif_pcap_flush(be, &discarded);
    if (st != IOX2_CIF_OK) {
        pcap_cleanup(writer, be, bp);
        return fail("pcap flush be after timeout", st);
    }
    timed_out = 0;
    actual = 9;
    st = iox2_cif_pcap_receive(
        bp, &out, got, sizeof(got), 40, &actual, &maxv, &timed_out, &samples, &dropped, &timeouts);
    if (st != IOX2_CIF_OK || timed_out != 1 || actual != 0 || timeouts != 1) {
        fprintf(stderr, "FAIL pcap flush then timeout to=%d actual=%u timeouts=%llu\n",
            (int)timed_out, actual, (unsigned long long)timeouts);
        pcap_cleanup(writer, be, bp);
        return 1;
    }

    st = iox2_cif_pcap_writer_stats_reset(writer);
    if (st != IOX2_CIF_OK) {
        pcap_cleanup(writer, be, bp);
        return fail("pcap writer_stats_reset", st);
    }
    st = iox2_cif_pcap_writer_stats(writer, &ws, &wt, &wctr);
    if (st != IOX2_CIF_OK || ws != 0 || wt != 0 || wctr != 0) {
        pcap_cleanup(writer, be, bp);
        return fail("pcap writer stats after reset", st);
    }

    timed_out = 1;
    st = iox2_cif_pcap_write(writer, &in, src, 0, 2, 200, &timed_out, &samples, &timeouts);
    if (st != IOX2_CIF_OK || timed_out != 0 || samples != 1) {
        pcap_cleanup(writer, be, bp);
        return fail("pcap write after reset", st);
    }
    st = iox2_cif_pcap_receive(
        be, &out, got, sizeof(got), 500, &actual, &maxv, &timed_out, &samples, &dropped, &timeouts);
    if (st != IOX2_CIF_OK || out.ctr != 2) {
        fprintf(stderr, "FAIL on-wire ctr after stats_reset ctr=%llu (want 2)\n", (unsigned long long)out.ctr);
        pcap_cleanup(writer, be, bp);
        return 1;
    }
    st = iox2_cif_pcap_receive(
        bp, &out, got, sizeof(got), 500, &actual, &maxv, &timed_out, &samples, &dropped, &timeouts);
    if (st != IOX2_CIF_OK || out.ctr != 2) {
        pcap_cleanup(writer, be, bp);
        return fail("pcap bp recv after reset", st);
    }

    st = iox2_cif_pcap_write(writer, &in, src, 0, 1, 200, &timed_out, &samples, &timeouts);
    if (st != IOX2_CIF_OK) {
        pcap_cleanup(writer, be, bp);
        return fail("pcap write 2", st);
    }
    st = iox2_cif_pcap_write(writer, &in, src, 0, 1, 80, &timed_out, &samples, &timeouts);
    if (st != IOX2_CIF_OK || timed_out != 1) {
        pcap_cleanup(writer, be, bp);
        return fail("pcap write overflow for drop", st);
    }
    st = iox2_cif_pcap_write(writer, &in, src, 0, 1, 80, &timed_out, &samples, &timeouts);
    (void)st;
    discarded = 0;
    st = iox2_cif_pcap_flush(be, &discarded);
    if (st != IOX2_CIF_OK) {
        pcap_cleanup(writer, be, bp);
        return fail("pcap flush be", st);
    }
    st = iox2_cif_pcap_flush(bp, &discarded);
    if (st != IOX2_CIF_OK) {
        pcap_cleanup(writer, be, bp);
        return fail("pcap flush bp", st);
    }

    st = iox2_cif_pcap_stats_reset(be);
    if (st != IOX2_CIF_OK) {
        pcap_cleanup(writer, be, bp);
        return fail("pcap stats_reset", st);
    }
    st = iox2_cif_pcap_write(writer, &in, src, 0, 1, 200, &timed_out, &samples, &timeouts);
    if (st != IOX2_CIF_OK || timed_out != 0) {
        pcap_cleanup(writer, be, bp);
        return fail("pcap write first after reset", st);
    }
    st = iox2_cif_pcap_receive(
        be, &out, got, sizeof(got), 500, &actual, &maxv, &timed_out, &samples, &dropped, &timeouts);
    if (st != IOX2_CIF_OK) {
        pcap_cleanup(writer, be, bp);
        return fail("pcap recv baseline", st);
    }
    {
        uint64_t first_ctr = out.ctr;
        st = iox2_cif_pcap_receive(
            bp, &out, got, sizeof(got), 500, &actual, &maxv, &timed_out, &samples, &dropped, &timeouts);
        if (st != IOX2_CIF_OK) {
            pcap_cleanup(writer, be, bp);
            return fail("pcap bp drain baseline", st);
        }
        /* BE queue is the advertised writer depth (4). Overflow it; drain BP so writes succeed. */
        for (int i = 0; i < 5; ++i) {
            st = iox2_cif_pcap_write(writer, &in, src, 0, 1, 200, &timed_out, &samples, &timeouts);
            if (st != IOX2_CIF_OK) {
                pcap_cleanup(writer, be, bp);
                return fail("pcap write gap", st);
            }
            st = iox2_cif_pcap_receive(
                bp, &out, got, sizeof(got), 500, &actual, &maxv, &timed_out, &samples, &dropped, &timeouts);
            if (st != IOX2_CIF_OK) {
                pcap_cleanup(writer, be, bp);
                return fail("pcap bp recv gap", st);
            }
        }
        st = iox2_cif_pcap_receive(
            be, &out, got, sizeof(got), 500, &actual, &maxv, &timed_out, &samples, &dropped, &timeouts);
        if (st != IOX2_CIF_OK || timed_out != 0 || dropped < 1 || out.ctr != first_ctr + 2) {
            fprintf(stderr, "FAIL pcap dropped st=%d to=%d dropped=%llu ctr=%llu want>=%llu\n",
                (int)st,
                (int)timed_out,
                (unsigned long long)dropped,
                (unsigned long long)out.ctr,
                (unsigned long long)(first_ctr + 2));
            pcap_cleanup(writer, be, bp);
            return 1;
        }
    }

    st = iox2_cif_pcap_stats(be, &samples, &dropped, &timeouts, &last_ctr);
    if (st != IOX2_CIF_OK || samples < 2) {
        pcap_cleanup(writer, be, bp);
        return fail("pcap_stats", st);
    }

    pcap_cleanup(writer, be, bp);
    writer = 0;
    be = 0;
    bp = 0;

    st = iox2_cif_pcap_writer_create(
        "iox2_cif_smoke_pcap_u", "any", IOX2_CIF_PCAP_LINKTYPE_ANY, 32, "", 2, 0, 1, 0, &writer);
    if (st != IOX2_CIF_OK) {
        return fail("universal writer", st);
    }
    {
        char type_buf[64];
        uint32_t type_n = 0;
        uint32_t lt = 0;
        uint32_t maxb = 0;
        uint32_t sbuf = 0;
        st = iox2_cif_pcap_info(writer, type_buf, sizeof(type_buf), &type_n, &lt, &maxb, &sbuf, &sbuf);
        if (st != IOX2_CIF_OK || strcmp(type_buf, IOX2_CIF_PCAP_TYPE) != 0 || lt != IOX2_CIF_PCAP_LINKTYPE_ANY) {
            pcap_cleanup(writer, 0, 0);
            return fail("universal pcap_info", st);
        }
    }
    st = iox2_cif_pcap_sub_create("iox2_cif_smoke_pcap_u", "any", 1, 0, 2000, &be);
    if (st != IOX2_CIF_OK) {
        pcap_cleanup(writer, 0, 0);
        return fail("typed sub on universal writer", st);
    }
    timed_out = 1;
    st = iox2_cif_pcap_write(writer, &in, src, 0, 1, 0, &timed_out, &samples, &timeouts);
    if (st != IOX2_CIF_OK || timed_out != 0) {
        pcap_cleanup(writer, be, 0);
        return fail("universal write", st);
    }
    st = iox2_cif_pcap_receive(
        be, &out, got, sizeof(got), 500, &actual, &maxv, &timed_out, &samples, &dropped, &timeouts);
    if (st != IOX2_CIF_OK || timed_out != 0 || actual != 1) {
        pcap_cleanup(writer, be, 0);
        return fail("typed sub recv universal", st);
    }
    pcap_cleanup(writer, be, 0);
    writer = 0;
    be = 0;

    st = iox2_cif_pcap_writer_create("iox2_cif_smoke_pcap_t", "wifi", 105, 16, "", 2, 0, 1, 0, &writer);
    if (st != IOX2_CIF_OK) {
        return fail("typed writer 105", st);
    }
    st = iox2_cif_pcap_sub_open("iox2_cif_smoke_pcap_t", "wifi", 0, 2000, &be);
    if (st != IOX2_CIF_OK) {
        pcap_cleanup(writer, 0, 0);
        return fail("universal sub on typed writer", st);
    }
    timed_out = 1;
    st = iox2_cif_pcap_write(writer, &in, NULL, 0, 0, 0, &timed_out, &samples, &timeouts);
    if (st != IOX2_CIF_OK || timed_out != 0) {
        pcap_cleanup(writer, be, 0);
        return fail("empty pcap write", st);
    }
    st = iox2_cif_pcap_receive(
        be, &out, got, sizeof(got), 500, &actual, &maxv, &timed_out, &samples, &dropped, &timeouts);
    if (st != IOX2_CIF_OK || actual != 0 || out.length != 48) {
        fprintf(stderr, "FAIL empty pcap actual=%u length=%llu\n", actual, (unsigned long long)out.length);
        pcap_cleanup(writer, be, 0);
        return 1;
    }
    pcap_cleanup(writer, be, 0);

    printf("smoke ok: cif-pcap header, universal ports, stats, flush\n");
    return 0;
}

static int meta_topic_info(
    const char* name, const char* topic, char* type_buf, uint32_t type_cap, uint32_t* type_n) {
    return iox2_cif_bb_topic_info(name, topic, 0, type_buf, type_cap, type_n, NULL, 0, NULL);
}

static iox2_cif_status meta_make_writer(uint64_t bb, const char* topic, uint64_t* writer) {
    int32_t initial = 1;
    return iox2_cif_bb_writer_create(
        bb,
        topic,
        IOX2_CIF_TYPE_I32,
        (uint32_t)sizeof(int32_t),
        (uint32_t)sizeof(int32_t),
        1,
        &initial,
        1,
        "{\"kind\":\"scalar\"}",
        writer);
}

static int test_meta_cleanup(void) {
    uint64_t bb = 0;
    uint64_t writer = 0;
    uint64_t reader = 0;
    char type_buf[64];
    uint32_t type_n = 0;
    iox2_cif_status st = IOX2_CIF_OK;

    st = iox2_cif_bb_create("iox2_cif_smoke_meta_pre", 4, &bb);
    if (st != IOX2_CIF_OK) {
        return fail("meta pre create", st);
    }
    st = meta_make_writer(bb, "pose", &writer);
    if (st != IOX2_CIF_OK) {
        iox2_cif_bb_destroy(bb, NULL);
        return fail("meta pre writer", st);
    }
    st = iox2_cif_bb_destroy(bb, NULL);
    if (st != IOX2_CIF_OK) {
        return fail("meta pre destroy", st);
    }
    st = meta_topic_info("iox2_cif_smoke_meta_pre", "pose", type_buf, sizeof(type_buf), &type_n);
    if (st != IOX2_CIF_ERR_TOPIC_NOT_FOUND) {
        return fail("meta pre topic_info", st);
    }

    bb = 0;
    writer = 0;
    st = iox2_cif_bb_create("iox2_cif_smoke_meta_none", 4, &bb);
    if (st != IOX2_CIF_OK) {
        return fail("meta none create", st);
    }
    st = meta_make_writer(bb, "pose", &writer);
    if (st != IOX2_CIF_OK) {
        iox2_cif_bb_destroy(bb, NULL);
        return fail("meta none writer", st);
    }
    st = iox2_cif_bb_start(bb);
    if (st != IOX2_CIF_OK) {
        iox2_cif_bb_destroy(bb, NULL);
        return fail("meta none start", st);
    }
    st = iox2_cif_bb_destroy(bb, NULL);
    if (st != IOX2_CIF_OK) {
        return fail("meta none destroy", st);
    }
    st = meta_topic_info("iox2_cif_smoke_meta_none", "pose", type_buf, sizeof(type_buf), &type_n);
    if (st != IOX2_CIF_ERR_TOPIC_NOT_FOUND) {
        return fail("meta none topic_info", st);
    }

    bb = 0;
    writer = 0;
    reader = 0;
    st = iox2_cif_bb_create("iox2_cif_smoke_meta_reader", 4, &bb);
    if (st != IOX2_CIF_OK) {
        return fail("meta reader create", st);
    }
    st = meta_make_writer(bb, "pose", &writer);
    if (st != IOX2_CIF_OK) {
        iox2_cif_bb_destroy(bb, NULL);
        return fail("meta reader writer", st);
    }
    st = iox2_cif_bb_start(bb);
    if (st != IOX2_CIF_OK) {
        iox2_cif_bb_destroy(bb, NULL);
        return fail("meta reader start", st);
    }
    st = iox2_cif_bb_reader_create(
        "iox2_cif_smoke_meta_reader",
        "pose",
        IOX2_CIF_TYPE_I32,
        (uint32_t)sizeof(int32_t),
        (uint32_t)sizeof(int32_t),
        0,
        2000,
        &reader);
    if (st != IOX2_CIF_OK) {
        iox2_cif_bb_destroy(bb, NULL);
        return fail("meta reader open", st);
    }
    st = iox2_cif_bb_destroy(bb, NULL);
    if (st != IOX2_CIF_OK) {
        iox2_cif_bb_destroy_reader(reader);
        return fail("meta reader destroy bb", st);
    }
    st = meta_topic_info("iox2_cif_smoke_meta_reader", "pose", type_buf, sizeof(type_buf), &type_n);
    if (st != IOX2_CIF_OK || strcmp(type_buf, IOX2_CIF_TYPE_I32) != 0) {
        iox2_cif_bb_destroy_reader(reader);
        return fail("meta reader topic_info", st);
    }
    iox2_cif_bb_destroy_reader(reader);

    printf("smoke ok: sidecar meta removed on destroy, kept while a reader remains\n");
    return 0;
}
