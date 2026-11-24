// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <chiaki/session.h>
#include <chiaki/fec.h>
#include <chiaki/time.h>
#include <chiaki/videoreceiver.h>
#include <munit.h>
#include <string.h>

static unsigned corrupt_messages;
static unsigned idr_requests;
static bool reject_frame;
static bool reject_idr_request;
static uint64_t clock_ms;

uint64_t __wrap_chiaki_time_now_monotonic_us(void)
{
    return clock_ms * 1000;
}

ChiakiErrorCode __wrap_stream_connection_send_corrupt_frame(ChiakiStreamConnection *connection,
    ChiakiSeqNum16 start, ChiakiSeqNum16 end)
{
    (void)connection;
    (void)start;
    (void)end;
    ++corrupt_messages;
    return CHIAKI_ERR_SUCCESS;
}

ChiakiErrorCode __wrap_stream_connection_send_idr_request(ChiakiStreamConnection *connection)
{
    (void)connection;
    ++idr_requests;
    return reject_idr_request ? CHIAKI_ERR_NETWORK : CHIAKI_ERR_SUCCESS;
}

static const uint8_t header[] = {
    0x00, 0x00, 0x00, 0x01, 0x40, 0x01, 0x0c, 0x01, 0xff, 0xff, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00,
    0xb0, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x96, 0x0a, 0xc0, 0x90, 0x00, 0x00, 0x00, 0x01,
    0x42, 0x01, 0x01, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 0xb0, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03,
    0x00, 0x96, 0xa0, 0x03, 0xc0, 0x80, 0x11, 0x07, 0xcb, 0xc2, 0xb9, 0x24, 0x29, 0x52, 0x70, 0x16,
    0xa0, 0x20, 0x20, 0x20, 0x80, 0x00, 0x07, 0xd2, 0x00, 0x01, 0xd4, 0xc0, 0x20, 0xe5, 0xa1, 0xe3,
    0xd0, 0x00, 0x00, 0x00, 0x01, 0x44, 0x01, 0xc0, 0xf3, 0xc0, 0x4c, 0x90,
};

static const uint8_t idr[] = {
    0x00, 0x00, 0x00, 0x01, 0x28, 0x01, 0xac, 0x25, 0xcf, 0x83, 0xff, 0x23, 0x54, 0xab, 0x5c, 0xf5,
    0x7a, 0x06, 0x7c, 0x3f, 0x31, 0x9b, 0xe6, 0x10, 0x57, 0xe8, 0x0e, 0xcf, 0xdd, 0xda, 0xdb, 0x3f, 0x80,
};

static bool expect_frame(uint8_t *bytes, size_t size, int32_t lost, bool repaired, void *user)
{
    (void)lost;
    (void)repaired;
    if(size == sizeof(header))
        return memcmp(bytes, header, size) == 0;
    munit_assert_size(size, ==, sizeof(idr));
    munit_assert_memory_equal(size, bytes, idr);
    if(reject_frame)
        return false;
    ++*(unsigned *)user;
    return true;
}

static void send_unit_data(ChiakiVideoReceiver *receiver, uint16_t frame, uint16_t index, const uint8_t *sample)
{
    enum { UNIT_SIZE = sizeof(idr) / 3 };
    uint8_t data[2 + UNIT_SIZE] = {0};
    memcpy(data + 2, sample + index * UNIT_SIZE, UNIT_SIZE);
    ChiakiTakionAVPacket packet = {
        .is_video = true, .unit_index = index, .frame_index = frame,
        .units_in_frame_total = 3, .data = data, .data_size = sizeof(data),
    };
    chiaki_video_receiver_av_packet(receiver, &packet);
}

static void send_frame(ChiakiVideoReceiver *receiver, uint16_t frame, const uint8_t *sample)
{
    for(uint16_t index = 0; index < 3; index++)
        send_unit_data(receiver, frame, index, sample);
}

static void send_unit(ChiakiVideoReceiver *receiver, uint16_t frame, uint16_t index)
{
    send_unit_data(receiver, frame, index, idr);
}

static void send_large_unit(ChiakiVideoReceiver *receiver, uint16_t frame, uint16_t index)
{
    enum { LARGE_UNITS = 11, UNIT_SIZE = sizeof(idr) / LARGE_UNITS };
    uint8_t data[2 + UNIT_SIZE] = {0};
    memcpy(data + 2, idr + index * UNIT_SIZE, UNIT_SIZE);
    ChiakiTakionAVPacket packet = {
        .is_video = true, .unit_index = index, .frame_index = frame,
        .units_in_frame_total = LARGE_UNITS, .data = data, .data_size = sizeof(data),
    };
    chiaki_video_receiver_av_packet(receiver, &packet);
}

static void initialize_receiver(ChiakiVideoReceiver *receiver, ChiakiSession *session,
    ChiakiLog *log, unsigned *frames, uint8_t wait_ms)
{
    corrupt_messages = 0;
    idr_requests = 0;
    reject_idr_request = false;
    reject_frame = false;
    clock_ms = 2000;
    chiaki_log_init(log, 0, NULL, NULL);
    session->log = log;
    session->connect_info.video_profile.codec = CHIAKI_CODEC_H265;
    session->connect_info.video_reorder_window_ms = wait_ms;
    session->connect_info.enable_idr_on_fec_failure = true;
    session->video_sample_cb = expect_frame;
    session->video_sample_cb_user = frames;

    chiaki_video_receiver_init(receiver, session, NULL);
    ChiakiVideoProfile profile = {0};
    profile.header = malloc(sizeof(header));
    munit_assert_not_null(profile.header);
    memcpy(profile.header, header, sizeof(header));
    profile.header_sz = sizeof(header);
    chiaki_video_receiver_stream_info(receiver, &profile, 1);

}

static void exercise_reorder(uint8_t wait_ms, uint64_t arrival_delay_ms)
{
    ChiakiSession session = {0};
    ChiakiVideoReceiver receiver;
    ChiakiLog log;
    unsigned frames = 0;
    initialize_receiver(&receiver, &session, &log, &frames, wait_ms);

    send_unit(&receiver, 1, 0);
    send_unit(&receiver, 1, 2);
    clock_ms += arrival_delay_ms;
    send_unit(&receiver, 1, 1);

    if(wait_ms > arrival_delay_ms)
    {
        munit_assert_uint(frames, ==, 1);
        munit_assert_uint(idr_requests, ==, 0);
        munit_assert_uint(corrupt_messages, ==, 0);
    }
    else
    {
        munit_assert_uint(frames, ==, 0);
        munit_assert_uint(idr_requests, ==, 1);
        munit_assert_true(chiaki_video_receiver_get_waiting_for_idr(&receiver));

        // The requested keyframe also loses a unit after the retry interval.
        clock_ms += 400;
        send_unit(&receiver, 2, 0);
        send_unit(&receiver, 2, 2);
        clock_ms += wait_ms;
        send_unit(&receiver, 2, 1);
        munit_assert_uint(idr_requests, ==, 2);
        munit_assert_uint(frames, ==, 0);

        send_unit(&receiver, 3, 0);
        send_unit(&receiver, 3, 1);
        send_unit(&receiver, 3, 2);
        munit_assert_uint(frames, ==, 1);
        munit_assert_false(chiaki_video_receiver_get_waiting_for_idr(&receiver));
        ChiakiVideoRecoveryStats stats = chiaki_video_receiver_get_recovery_stats(&receiver);
        munit_assert_true(stats.has_recovered);
        munit_assert_uint64(stats.loss_started_ms, ==, 0);
    }
    chiaki_video_receiver_fini(&receiver);
}

static MunitResult test_reorder(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    exercise_reorder(0, 3);
    exercise_reorder(8, 7);
    exercise_reorder(8, 9);
    exercise_reorder(16, 9);
    exercise_reorder(16, 16);
    return MUNIT_OK;
}

static MunitResult test_fec_repaired(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    ChiakiSession session = {0};
    ChiakiVideoReceiver receiver;
    ChiakiLog log;
    unsigned frames = 0;
    initialize_receiver(&receiver, &session, &log, &frames, 0);

    enum { UNIT_SIZE = 16, SOURCE_SIZE = sizeof(idr) / 3 + 2 };
    uint8_t encoded[4][UNIT_SIZE] = {{0}};
    for(unsigned i = 0; i < 3; ++i)
    {
        encoded[i][1] = UNIT_SIZE - SOURCE_SIZE;
        memcpy(encoded[i] + 2, idr + i * (SOURCE_SIZE - 2), SOURCE_SIZE - 2);
    }
    munit_assert_int(chiaki_fec_encode(&encoded[0][0], UNIT_SIZE, UNIT_SIZE, 3, 1), ==, CHIAKI_ERR_SUCCESS);
    const uint16_t delivered[] = {0, 2, 3};
    for(unsigned i = 0; i < 3; ++i)
    {
        const uint16_t index = delivered[i];
        ChiakiTakionAVPacket packet = {
            .is_video = true, .unit_index = index, .frame_index = 1,
            .units_in_frame_total = 4, .units_in_frame_fec = 1, .data = encoded[index],
            .data_size = index == 3 ? UNIT_SIZE : SOURCE_SIZE,
        };
        chiaki_video_receiver_av_packet(&receiver, &packet);
    }

    const ChiakiVideoRecoveryStats stats = chiaki_video_receiver_get_recovery_stats(&receiver);
    munit_assert_uint(frames, ==, 1);
    munit_assert_uint(idr_requests, ==, 0);
    munit_assert_uint(corrupt_messages, ==, 0);
    munit_assert_uint64(stats.fec_recovered_frames, ==, 1);
    munit_assert_uint64(stats.assembled_video_bytes, ==, sizeof(idr));
    munit_assert_uint64(stats.loss_started_ms, ==, 0);
    chiaki_video_receiver_fini(&receiver);
    return MUNIT_OK;
}

static MunitResult test_gap_and_rejected_keyframe(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    ChiakiSession session = {0};
    ChiakiVideoReceiver receiver;
    ChiakiLog log;
    unsigned frames = 0;
    initialize_receiver(&receiver, &session, &log, &frames, 8);
    send_frame(&receiver, 65534, idr);
    munit_assert_uint(corrupt_messages, ==, 0);

    // Frame 65535 disappears completely. Frame 0 is intact at the packet
    // layer, but has no recognizable slice header or safe reference.
    uint8_t unreadable[sizeof(idr)] = {0};
    send_frame(&receiver, 0, unreadable);
    munit_assert_uint(corrupt_messages, ==, 1);
    munit_assert_uint(idr_requests, ==, 1);
    munit_assert_int(chiaki_video_receiver_get_frames_lost_total(&receiver), ==, 1);
    munit_assert_true(chiaki_video_receiver_get_waiting_for_idr(&receiver));

    // Reject a received keyframe just before the retry deadline. The
    // decoder has not recovered yet and cannot consume predicted frames.
    clock_ms += 399;
    reject_frame = true;
    send_frame(&receiver, 1, idr);
    munit_assert_uint(idr_requests, ==, 1);
    munit_assert_true(chiaki_video_receiver_get_waiting_for_idr(&receiver));
    munit_assert_false(chiaki_video_receiver_get_recovery_stats(&receiver).has_recovered);
    clock_ms++;
    send_frame(&receiver, 2, unreadable);
    munit_assert_uint(idr_requests, ==, 2);

    reject_frame = false;
    send_frame(&receiver, 3, idr);
    munit_assert_uint(frames, ==, 2);
    munit_assert_false(chiaki_video_receiver_get_waiting_for_idr(&receiver));
    const ChiakiVideoRecoveryStats stats = chiaki_video_receiver_get_recovery_stats(&receiver);
    munit_assert_true(stats.has_recovered);
    munit_assert_uint64(stats.last_recovery_ms, ==, 400);
    munit_assert_uint64(stats.idr_requests, ==, 2);
    munit_assert_uint64(stats.frames_held_for_keyframe, ==, 2);
    chiaki_video_receiver_fini(&receiver);
    return MUNIT_OK;
}

static MunitResult test_gap_followed_by_clean_keyframe(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    ChiakiSession session = {0};
    ChiakiVideoReceiver receiver;
    ChiakiLog log;
    unsigned frames = 0;
    initialize_receiver(&receiver, &session, &log, &frames, 8);
    send_frame(&receiver, 7, idr);
    send_frame(&receiver, 10, idr);
    munit_assert_uint(frames, ==, 2);
    munit_assert_uint(corrupt_messages, ==, 1);
    munit_assert_uint(idr_requests, ==, 0);
    munit_assert_int(chiaki_video_receiver_get_frames_lost_total(&receiver), ==, 2);
    chiaki_video_receiver_fini(&receiver);
    return MUNIT_OK;
}

static MunitResult test_late_unit_after_deadline(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    ChiakiSession session = {0};
    ChiakiVideoReceiver receiver;
    ChiakiLog log;
    unsigned frames = 0;
    initialize_receiver(&receiver, &session, &log, &frames, 8);

    // Frame 1 misses unit 1, arms 8 ms reorder deadline on last unit (2).
    send_unit(&receiver, 1, 0);
    send_unit(&receiver, 1, 2);
    clock_ms += 10;
    // Unit 1 arrives past deadline: frame 1 flushes (fails) and stale unit is ignored.
    send_unit(&receiver, 1, 1);
    munit_assert_uint(frames, ==, 0);
    munit_assert_uint(idr_requests, ==, 1);
    // Another duplicate late unit for the already-flushed frame must not break state.
    send_unit(&receiver, 1, 1);
    munit_assert_uint(frames, ==, 0);
    // Next clean keyframe recovers cleanly.
    send_frame(&receiver, 2, idr);
    munit_assert_uint(frames, ==, 1);
    munit_assert_false(chiaki_video_receiver_get_waiting_for_idr(&receiver));
    chiaki_video_receiver_fini(&receiver);
    return MUNIT_OK;
}

static bool accept_any_frame(uint8_t *bytes, size_t size, int32_t lost, bool repaired, void *user)
{
    (void)lost;
    (void)repaired;
    if(size == sizeof(header))
        return memcmp(bytes, header, size) == 0;
    if(reject_frame)
        return false;
    ++*(unsigned *)user;
    return true;
}

static MunitResult test_clean_ref_p_recovers_without_idr(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    ChiakiSession session = {0};
    ChiakiVideoReceiver receiver;
    ChiakiLog log;
    unsigned frames = 0;
    initialize_receiver(&receiver, &session, &log, &frames, 0);
    session.video_sample_cb = accept_any_frame;

    // Frame 10 is a clean IDR -> parses SPS/PPS, sends header + IDR, adds frame 10 to reference_frames.
    send_frame(&receiver, 10, idr);
    munit_assert_uint(frames, ==, 1);

    // Valid HEVC P-slice (NAL type 1, first_slice=1, pps=0, slice_type=1 (P),
    // poc_lsb = (log2_max_pic_order_cnt_lsb_minus4 + 4) one bits,
    // short_term_ref_pic_set_sps_flag=0, num_neg=3, num_pos=0).
    // Read poc_bits AFTER frame 10 triggered SPS parsing.
    const unsigned poc_bits = receiver.bitstream.h265.sps.log2_max_pic_order_cnt_lsb_minus4 + 4;
    uint8_t p_ref0[sizeof(idr)] = {0x00, 0x00, 0x00, 0x01, 0x02, 0x01};
    uint8_t p_ref2[sizeof(idr)] = {0x00, 0x00, 0x00, 0x01, 0x02, 0x01};
    unsigned pos = 0;
    uint8_t *dst = p_ref0 + 6;
    #define PUT_BIT(b) do { if(b) dst[pos >> 3] |= (uint8_t)(0x80u >> (pos & 7)); pos++; } while(0)
    // p_ref0: first_slice=1, pps(ue:0)=1, slice_type(ue:1)=010, poc_lsb=all 1s, st_rps_sps=0, num_neg(ue:3)=00100, num_pos(ue:0)=1, delta[0](ue:0)=1, used[0]=1
    PUT_BIT(1); PUT_BIT(1); PUT_BIT(0); PUT_BIT(1); PUT_BIT(0);
    for(unsigned i = 0; i < poc_bits; ++i) PUT_BIT(1);
    PUT_BIT(0); PUT_BIT(0); PUT_BIT(0); PUT_BIT(1); PUT_BIT(0); PUT_BIT(0); PUT_BIT(1);
    PUT_BIT(1); PUT_BIT(1);

    // p_ref2: same prefix, then delta[0]=0 used[0]=0, delta[1]=0 used[1]=0, delta[2]=0 used[2]=1 -> reference_frame=2
    pos = 0; dst = p_ref2 + 6;
    PUT_BIT(1); PUT_BIT(1); PUT_BIT(0); PUT_BIT(1); PUT_BIT(0);
    for(unsigned i = 0; i < poc_bits; ++i) PUT_BIT(1);
    PUT_BIT(0); PUT_BIT(0); PUT_BIT(0); PUT_BIT(1); PUT_BIT(0); PUT_BIT(0); PUT_BIT(1);
    PUT_BIT(1); PUT_BIT(0);
    PUT_BIT(1); PUT_BIT(0);
    PUT_BIT(1); PUT_BIT(1);
    #undef PUT_BIT

    // Frame 11 fails FEC -> waiting_for_idr becomes true, IDR requested.
    send_unit(&receiver, 11, 0);
    send_unit(&receiver, 11, 2);
    munit_assert_true(chiaki_video_receiver_get_waiting_for_idr(&receiver));
    munit_assert_uint(idr_requests, ==, 1);

    // Frame 12 references frame 11 (missing!) -> must be held, NOT decoded.
    clock_ms += 17;
    send_frame(&receiver, 12, p_ref0);
    munit_assert_uint(frames, ==, 1);
    munit_assert_true(chiaki_video_receiver_get_waiting_for_idr(&receiver));

    // Frame 13 references frame 10 (13 - 2 - 1 = 10, INTACT!) -> recovers immediately without waiting for IDR!
    clock_ms += 33;
    send_frame(&receiver, 13, p_ref2);
    munit_assert_uint(frames, ==, 2);
    munit_assert_false(chiaki_video_receiver_get_waiting_for_idr(&receiver));
    const ChiakiVideoRecoveryStats stats = chiaki_video_receiver_get_recovery_stats(&receiver);
    munit_assert_true(stats.has_recovered);
    munit_assert_uint64(stats.loss_started_ms, ==, 0);
    munit_assert_uint64(stats.last_recovery_ms, ==, 50);
    chiaki_video_receiver_fini(&receiver);
    return MUNIT_OK;
}

static MunitResult test_cross_frame_reordering_recovers_frame(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    ChiakiSession session = {0};
    ChiakiVideoReceiver receiver;
    ChiakiLog log;
    unsigned frames = 0;
    initialize_receiver(&receiver, &session, &log, &frames, 8);

    // Frame 10 arrives missing unit 1; then frame 11 units 0 & 1 arrive early (t + 2ms).
    send_unit(&receiver, 10, 0);
    send_unit(&receiver, 10, 2);
    clock_ms += 2;
    send_unit(&receiver, 11, 0);
    send_unit(&receiver, 11, 1);
    munit_assert_uint(frames, ==, 0);
    munit_assert_uint(idr_requests, ==, 0);

    // Late unit 1 of frame 10 arrives at t + 4ms (< 8ms window) -> frame 10 completes cleanly!
    clock_ms += 2;
    send_unit(&receiver, 10, 1);
    munit_assert_uint(frames, ==, 1);
    munit_assert_uint(idr_requests, ==, 0);
    munit_assert_false(chiaki_video_receiver_get_waiting_for_idr(&receiver));

    // Final unit 2 of frame 11 arrives -> frame 11 completes cleanly too, zero loss!
    clock_ms += 1;
    send_unit(&receiver, 11, 2);
    munit_assert_uint(frames, ==, 2);
    munit_assert_uint(idr_requests, ==, 0);
    munit_assert_int(chiaki_video_receiver_get_frames_lost_total(&receiver), ==, 0);
    chiaki_video_receiver_fini(&receiver);
    return MUNIT_OK;
}

static MunitResult test_always_request_idr_on_gap(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    ChiakiSession session = {0};
    ChiakiVideoReceiver receiver;
    ChiakiLog log;
    unsigned frames = 0;
    initialize_receiver(&receiver, &session, &log, &frames, 0);
    session.connect_info.enable_idr_on_fec_failure = false;
    session.connect_info.always_request_idr_on_loss = true;

    // Frame 10 arrives cleanly.
    send_frame(&receiver, 10, idr);
    munit_assert_uint(frames, ==, 1);
    munit_assert_uint(idr_requests, ==, 0);

    // Frame 11 is completely lost in the network; frame 12 arrives (gap = 1 frame).
    // With always_request_idr_on_loss enabled, IDR must be requested immediately upon detecting the gap.
    clock_ms += 33;
    send_unit(&receiver, 12, 0);
    munit_assert_uint(idr_requests, ==, 1);
    chiaki_video_receiver_fini(&receiver);
    return MUNIT_OK;
}

static MunitResult test_next_frame_alloc_failure_leaves_slot_clean(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    ChiakiSession session = {0};
    ChiakiVideoReceiver receiver;
    ChiakiLog log;
    unsigned frames = 0;
    initialize_receiver(&receiver, &session, &log, &frames, 8);

    // Frame 10 starts incomplete (missing unit 1).
    send_unit(&receiver, 10, 0);
    send_unit(&receiver, 10, 2);

    // Malformed packet for frame 11 (units_in_frame_total < units_in_frame_fec) fails next_frame alloc.
    uint8_t bad_data[8] = {0};
    ChiakiTakionAVPacket bad_pkt = {
        .is_video = true, .unit_index = 0, .frame_index = 11,
        .units_in_frame_total = 1, .units_in_frame_fec = 3,
        .data = bad_data, .data_size = sizeof(bad_data),
    };
    chiaki_video_receiver_av_packet(&receiver, &bad_pkt);
    munit_assert_int(receiver.next_frame_index, ==, -1);

    // Late unit 1 for frame 10 arrives within window -> frame 10 still completes cleanly!
    clock_ms += 2;
    send_unit(&receiver, 10, 1);
    munit_assert_uint(frames, ==, 1);
    munit_assert_uint(idr_requests, ==, 0);
    chiaki_video_receiver_fini(&receiver);
    return MUNIT_OK;
}

typedef struct profile_switch_trace_t
{
    int events[8]; // 0 = header0, 1 = header1, 10 = frame
    unsigned count;
} ProfileSwitchTrace;

static bool trace_profile_switch_cb(uint8_t *bytes, size_t size, int32_t lost, bool repaired, void *user)
{
    (void)lost;
    (void)repaired;
    ProfileSwitchTrace *trace = (ProfileSwitchTrace *)user;
    munit_assert_uint(trace->count, <, 8);
    if(size == sizeof(header))
        trace->events[trace->count++] = (bytes[sizeof(header) - 1] == 0x91) ? 1 : 0;
    else
    {
        munit_assert_size(size, ==, sizeof(idr));
        trace->events[trace->count++] = 10;
    }
    return true;
}

static MunitResult test_reordered_profile_switch_emits_header_in_frame_order(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    ChiakiSession session = {0};
    ChiakiVideoReceiver receiver;
    ChiakiLog log;
    ProfileSwitchTrace trace = {{0}, 0};
    corrupt_messages = 0;
    idr_requests = 0;
    reject_frame = false;
    clock_ms = 2000;
    chiaki_log_init(&log, 0, NULL, NULL);
    session.log = &log;
    session.connect_info.video_profile.codec = CHIAKI_CODEC_H265;
    session.connect_info.video_reorder_window_ms = 8;
    session.connect_info.enable_idr_on_fec_failure = true;
    session.video_sample_cb = trace_profile_switch_cb;
    session.video_sample_cb_user = &trace;

    chiaki_video_receiver_init(&receiver, &session, NULL);
    ChiakiVideoProfile profiles[2] = {{0}, {0}};
    for(int p = 0; p < 2; ++p)
    {
        profiles[p].header = malloc(sizeof(header));
        munit_assert_not_null(profiles[p].header);
        memcpy(profiles[p].header, header, sizeof(header));
        if(p == 1)
            profiles[p].header[sizeof(header) - 1] = 0x91;
        profiles[p].header_sz = sizeof(header);
        profiles[p].width = p == 0 ? 1280 : 1920;
        profiles[p].height = p == 0 ? 720 : 1080;
    }
    chiaki_video_receiver_stream_info(&receiver, profiles, 2);

    // Frame 10 starts on profile 0 (missing unit 1).
    send_unit(&receiver, 10, 0);
    send_unit(&receiver, 10, 2);
    munit_assert_uint(trace.count, ==, 1);
    munit_assert_int(trace.events[0], ==, 0); // header 0 emitted once

    // Early units 0..2 of frame 11 arrive on profile 1 BEFORE late unit 1 of frame 10!
    enum { UNIT_SIZE = sizeof(idr) / 3 };
    for(uint16_t u = 0; u < 3; ++u)
    {
        uint8_t pkt_data[2 + UNIT_SIZE] = {0};
        memcpy(pkt_data + 2, idr + u * UNIT_SIZE, UNIT_SIZE);
        ChiakiTakionAVPacket pkt = {
            .is_video = true, .unit_index = u, .frame_index = 11,
            .units_in_frame_total = 3, .adaptive_stream_index = 1,
            .data = pkt_data, .data_size = sizeof(pkt_data),
        };
        chiaki_video_receiver_av_packet(&receiver, &pkt);
    }
    // Header 1 must NOT be emitted before frame 10 finishes!
    munit_assert_uint(trace.count, ==, 1);
    munit_assert_int(receiver.profile_cur, ==, 0);

    // Late unit 1 of frame 10 arrives on profile 0 -> flushes frame 10, then promotes frame 11,
    // emits header 1, and flushes frame 11!
    clock_ms += 2;
    send_unit(&receiver, 10, 1);
    munit_assert_uint(trace.count, ==, 4);
    munit_assert_int(trace.events[0], ==, 0);  // header 0
    munit_assert_int(trace.events[1], ==, 10); // frame 10
    munit_assert_int(trace.events[2], ==, 1);  // header 1
    munit_assert_int(trace.events[3], ==, 10); // frame 11
    munit_assert_int(receiver.profile_cur, ==, 1);

    chiaki_video_receiver_fini(&receiver);
    return MUNIT_OK;
}

static MunitResult test_idr_after_recovery_and_send_failure(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    ChiakiSession session = {0};
    ChiakiVideoReceiver receiver;
    ChiakiLog log;
    unsigned frames = 0;
    initialize_receiver(&receiver, &session, &log, &frames, 0);

    send_unit(&receiver, 1, 0);
    send_unit(&receiver, 1, 2);
    munit_assert_uint(idr_requests, ==, 1);
    send_frame(&receiver, 2, idr);
    munit_assert_false(chiaki_video_receiver_get_waiting_for_idr(&receiver));

    clock_ms += 17;
    reject_idr_request = true;
    send_unit(&receiver, 3, 0);
    send_unit(&receiver, 3, 2);
    munit_assert_uint(idr_requests, ==, 2);
    munit_assert_true(chiaki_video_receiver_get_waiting_for_idr(&receiver));

    clock_ms += 17;
    reject_idr_request = false;
    send_unit(&receiver, 4, 0);
    send_unit(&receiver, 4, 2);
    munit_assert_uint(idr_requests, ==, 3);
    munit_assert_uint64(chiaki_video_receiver_get_recovery_stats(&receiver).idr_requests, ==, 2);
    chiaki_video_receiver_fini(&receiver);
    return MUNIT_OK;
}

static MunitResult test_idle_deadline_flushes_and_promotes(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    ChiakiSession session = {0};
    ChiakiVideoReceiver receiver;
    ChiakiLog log;
    unsigned frames = 0;
    initialize_receiver(&receiver, &session, &log, &frames, 8);

    send_unit(&receiver, 1, 0); // The last unit never arrives.
    munit_assert_uint64(chiaki_video_receiver_poll_timeout(&receiver, 100), ==, 8);
    clock_ms += 2;
    send_frame(&receiver, 2, idr);
    munit_assert_uint(frames, ==, 0);
    munit_assert_uint64(chiaki_video_receiver_poll_timeout(&receiver, 100), ==, 6);

    clock_ms += 6;
    munit_assert_uint64(chiaki_video_receiver_poll_timeout(&receiver, 100), ==, 100);
    munit_assert_uint(idr_requests, ==, 1);
    munit_assert_uint(frames, ==, 1);
    chiaki_video_receiver_fini(&receiver);
    return MUNIT_OK;
}

static MunitResult test_hevc_sparse_reference(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    ChiakiBitstream bitstream;
    ChiakiLog log;
    chiaki_log_init(&log, 0, NULL, NULL);
    chiaki_bitstream_init(&bitstream, &log, CHIAKI_CODEC_H265);
    uint8_t profile_header[sizeof(header)];
    memcpy(profile_header, header, sizeof(header));
    munit_assert_true(chiaki_bitstream_header(&bitstream, profile_header, sizeof(profile_header)));

    uint8_t sample[sizeof(idr)] = {0, 0, 0, 1, 2, 1};
    unsigned pos = 48;
    #define PUT_BIT(b) do { if(b) sample[pos >> 3] |= (uint8_t)(0x80u >> (pos & 7)); pos++; } while(0)
    PUT_BIT(1); PUT_BIT(1); PUT_BIT(0); PUT_BIT(1); PUT_BIT(0); // First slice, PPS 0, P-slice.
    const unsigned poc_bits = bitstream.h265.sps.log2_max_pic_order_cnt_lsb_minus4 + 4;
    for(unsigned i = 0; i < poc_bits; ++i) PUT_BIT(1);
    PUT_BIT(0); PUT_BIT(0); PUT_BIT(1); PUT_BIT(1); PUT_BIT(1); // Inline RPS, two negative pics, zero positive.
    PUT_BIT(0); PUT_BIT(1); PUT_BIT(0); PUT_BIT(1); // delta 1, used: distance 2 (reference_frame 1).
    PUT_BIT(0); PUT_BIT(1); PUT_BIT(1); PUT_BIT(0); // delta 2, unused: cumulative distance 5 (reference_frame 4).
    PUT_BIT(1);
    #undef PUT_BIT

    ChiakiBitstreamSlice slice = {0};
    munit_assert_true(chiaki_bitstream_slice(&bitstream, sample, sizeof(sample), &slice));
    munit_assert_uint(slice.reference_frame, ==, 1);
    munit_assert_false(chiaki_bitstream_slice_set_reference_frame(&bitstream, sample, sizeof(sample), 2));
    munit_assert_true(chiaki_bitstream_slice_set_reference_frame(&bitstream, sample, sizeof(sample), 4));
    munit_assert_true(chiaki_bitstream_slice(&bitstream, sample, sizeof(sample), &slice));
    munit_assert_uint(slice.reference_frame, ==, 4);
    return MUNIT_OK;
}

static MunitResult test_large_frame_spanning_reorder_window(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    const uint8_t windows[] = {8, 16};
    const uint64_t step_ms[] = {2, 3}; // 10 * 2 = 20 ms (> 8 ms), 10 * 3 = 30 ms (> 16 ms)
    for(size_t w = 0; w < 2; ++w)
    {
        ChiakiSession session = {0};
        ChiakiVideoReceiver receiver;
        ChiakiLog log;
        unsigned frames = 0;
        initialize_receiver(&receiver, &session, &log, &frames, windows[w]);

        for(uint16_t u = 0; u < 11; ++u)
        {
            if(u > 0)
                clock_ms += step_ms[w];
            send_large_unit(&receiver, 1, u);
        }

        munit_assert_uint(frames, ==, 1);
        munit_assert_uint(idr_requests, ==, 0);
        munit_assert_uint(corrupt_messages, ==, 0);
        munit_assert_uint64(chiaki_video_receiver_get_recovery_stats(&receiver).reordered_frames, ==, 0);
        chiaki_video_receiver_fini(&receiver);
    }
    return MUNIT_OK;
}

static MunitResult test_large_frame_reordered_unit_and_locked_deadline(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    ChiakiSession session = {0};
    ChiakiVideoReceiver receiver;
    ChiakiLog log;
    unsigned frames = 0;
    initialize_receiver(&receiver, &session, &log, &frames, 8);

    // Frame 1 spans 10 ms (> 8 ms window) and unit 3 is delayed 5 ms past last_unit.
    for(uint16_t u = 0; u < 11; ++u)
    {
        if(u > 0)
            clock_ms += 1;
        if(u == 3)
            continue;
        send_large_unit(&receiver, 1, u);
    }
    munit_assert_uint(frames, ==, 0);
    munit_assert_uint(idr_requests, ==, 0);
    clock_ms += 5;
    send_large_unit(&receiver, 1, 3);
    munit_assert_uint(frames, ==, 1);
    munit_assert_uint(idr_requests, ==, 0);
    munit_assert_uint64(chiaki_video_receiver_get_recovery_stats(&receiver).reordered_frames, ==, 1);

    // Frame 2 misses units 3 and 4. last_unit locks the 8 ms deadline; late unit 3 must NOT extend it.
    clock_ms += 5;
    for(uint16_t u = 0; u < 11; ++u)
    {
        if(u > 0)
            clock_ms += 1;
        if(u == 3 || u == 4)
            continue;
        send_large_unit(&receiver, 2, u);
    }
    munit_assert_uint64(chiaki_video_receiver_poll_timeout(&receiver, 100), ==, 8);
    clock_ms += 5;
    send_large_unit(&receiver, 2, 3); // Arrives during wait, unit 4 still missing.
    munit_assert_uint64(chiaki_video_receiver_poll_timeout(&receiver, 100), ==, 3);
    clock_ms += 3;
    munit_assert_uint64(chiaki_video_receiver_poll_timeout(&receiver, 100), ==, 100);
    munit_assert_uint(idr_requests, ==, 1);
    munit_assert_uint(frames, ==, 1);

    chiaki_video_receiver_fini(&receiver);
    return MUNIT_OK;
}

static MunitResult test_promoted_large_next_frame_keeps_sliding_deadline(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    ChiakiSession session = {0};
    ChiakiVideoReceiver receiver;
    ChiakiLog log;
    unsigned frames = 0;
    initialize_receiver(&receiver, &session, &log, &frames, 8);

    // Frame 1 stalls after unit 0 (deadline = t + 8).
    send_unit(&receiver, 1, 0);
    clock_ms += 2;

    // Large frame 2 streams units 0..5 into next_frame over t + 2 .. t + 7.
    for(uint16_t u = 0; u <= 5; ++u)
    {
        if(u > 0)
            clock_ms += 1;
        send_large_unit(&receiver, 2, u);
    }
    munit_assert_uint(frames, ==, 0);
    munit_assert_uint64(chiaki_video_receiver_poll_timeout(&receiver, 100), ==, 1);

    // At t + 8, frame 1 expires and frame 2 is promoted with refreshed deadline (t + 7 + 8 = t + 15).
    clock_ms += 1;
    munit_assert_uint64(chiaki_video_receiver_poll_timeout(&receiver, 100), ==, 7);
    munit_assert_uint(idr_requests, ==, 1);

    // Remaining units 6..10 arrive over t + 10 .. t + 18 (past the original t + 2 + 8 = t + 10!).
    for(uint16_t u = 6; u < 11; ++u)
    {
        clock_ms += 2;
        send_large_unit(&receiver, 2, u);
    }
    munit_assert_uint(frames, ==, 1);
    munit_assert_uint(idr_requests, ==, 1);
    munit_assert_false(chiaki_video_receiver_get_waiting_for_idr(&receiver));

    chiaki_video_receiver_fini(&receiver);
    return MUNIT_OK;
}

static MunitTest tests[] = {
    {"/reordered_hevc_units_and_lost_idr", test_reorder, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/fec_repair_needs_no_idr", test_fec_repaired, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/frame_gap_and_rejected_keyframe", test_gap_and_rejected_keyframe, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/frame_gap_followed_by_clean_keyframe", test_gap_followed_by_clean_keyframe, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/late_unit_after_deadline", test_late_unit_after_deadline, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/clean_ref_p_recovers_without_idr", test_clean_ref_p_recovers_without_idr, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/cross_frame_reordering_recovers_frame", test_cross_frame_reordering_recovers_frame, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/always_request_idr_on_gap", test_always_request_idr_on_gap, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/next_frame_alloc_failure_leaves_slot_clean", test_next_frame_alloc_failure_leaves_slot_clean, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/reordered_profile_switch_emits_header_in_frame_order", test_reordered_profile_switch_emits_header_in_frame_order, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/idr_after_recovery_and_send_failure", test_idr_after_recovery_and_send_failure, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/idle_deadline_flushes_and_promotes", test_idle_deadline_flushes_and_promotes, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/hevc_sparse_reference", test_hevc_sparse_reference, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/large_frame_spanning_reorder_window", test_large_frame_spanning_reorder_window, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/large_frame_reordered_unit_and_locked_deadline", test_large_frame_reordered_unit_and_locked_deadline, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/promoted_large_next_frame_keeps_sliding_deadline", test_promoted_large_next_frame_keeps_sliding_deadline, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
};

int main(int argc, char *argv[])
{
    const MunitSuite suite = {"/video_receiver", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE};
    return munit_suite_main(&suite, NULL, argc, argv);
}
