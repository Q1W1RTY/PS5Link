// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_VIDEORECEIVER_H
#define CHIAKI_VIDEORECEIVER_H

#include "common.h"
#include "log.h"
#include "video.h"
#include "takion.h"
#include "frameprocessor.h"
#include "bitstream.h"
#include "thread.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CHIAKI_VIDEO_PROFILES_MAX 8

typedef struct chiaki_video_recovery_stats_t
{
    uint64_t loss_started_ms;
    uint64_t last_recovery_ms;
    uint64_t fec_recovered_frames;
    uint64_t reordered_frames;
    uint64_t idr_requests;
    uint64_t frames_held_for_keyframe;
    uint64_t assembled_video_bytes;
    bool has_recovered;
} ChiakiVideoRecoveryStats;

typedef struct chiaki_video_receiver_t
{
	struct chiaki_session_t *session;
	ChiakiLog *log;
	ChiakiVideoProfile profiles[CHIAKI_VIDEO_PROFILES_MAX];
	size_t profiles_count;
	int profile_cur; // < 1 if no profile selected yet, else index in profiles

	int32_t frame_index_cur; // frame that is currently being filled
	int32_t frame_index_prev; // last frame that has been at least partially decoded
	ChiakiFrameProcessor frame_processor;
	ChiakiFrameProcessor next_frame_processor; // 2nd assembly slot for cross-frame UDP reordering
	int32_t next_frame_index; // -1 if empty, else (frame_index_cur + 1)
	int next_frame_profile; // adaptive_stream_index for next_frame_index, or -1
	bool next_frame_saw_last_unit;
	ChiakiPacketStats *packet_stats;

	int32_t frames_lost;
	int32_t frames_lost_total;
	int32_t reference_frames[16];
	ChiakiBitstream bitstream;
	ChiakiMutex waiting_for_idr_mutex;
	bool waiting_for_idr;
	uint64_t idr_request_last_ms; // when the IDR being waited for was last requested
	ChiakiMutex frames_lost_mutex;
	ChiakiVideoRecoveryStats recovery_stats;
	uint64_t reorder_deadline_ms;
	bool frame_follows_gap;
} ChiakiVideoReceiver;

CHIAKI_EXPORT void chiaki_video_receiver_init(ChiakiVideoReceiver *video_receiver, struct chiaki_session_t *session, ChiakiPacketStats *packet_stats);
CHIAKI_EXPORT void chiaki_video_receiver_fini(ChiakiVideoReceiver *video_receiver);

/**
 * Called after receiving the Stream Info Packet.
 *
 * @param video_receiver
 * @param profiles Array of profiles. Ownership of the contained header buffers will be transferred to the ChiakiVideoReceiver!
 * @param profiles_count must be <= CHIAKI_VIDEO_PROFILES_MAX
 */
CHIAKI_EXPORT void chiaki_video_receiver_stream_info(ChiakiVideoReceiver *video_receiver, ChiakiVideoProfile *profiles, size_t profiles_count);

CHIAKI_EXPORT void chiaki_video_receiver_av_packet(ChiakiVideoReceiver *video_receiver, ChiakiTakionAVPacket *packet);
CHIAKI_EXPORT void chiaki_video_receiver_set_waiting_for_idr(ChiakiVideoReceiver *video_receiver, bool waiting_for_idr);
CHIAKI_EXPORT bool chiaki_video_receiver_get_waiting_for_idr(ChiakiVideoReceiver *video_receiver);
CHIAKI_EXPORT int32_t chiaki_video_receiver_get_frames_lost_total(ChiakiVideoReceiver *video_receiver);

CHIAKI_EXPORT ChiakiVideoRecoveryStats chiaki_video_receiver_get_recovery_stats(ChiakiVideoReceiver *video_receiver);

static inline ChiakiVideoReceiver *chiaki_video_receiver_new(struct chiaki_session_t *session, ChiakiPacketStats *packet_stats)
{
	ChiakiVideoReceiver *video_receiver = CHIAKI_NEW(ChiakiVideoReceiver);
	if(!video_receiver)
		return NULL;
	chiaki_video_receiver_init(video_receiver, session, packet_stats);
	return video_receiver;
}

static inline void chiaki_video_receiver_free(ChiakiVideoReceiver *video_receiver)
{
	if(!video_receiver)
		return;
	chiaki_video_receiver_fini(video_receiver);
	free(video_receiver);
}

#ifdef __cplusplus
}
#endif

#endif // CHIAKI_VIDEORECEIVER_H
