// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <chiaki/videoreceiver.h>
#include <chiaki/time.h>
#include "../include/chiaki/session.h"

#include <string.h>

// While waiting for a requested IDR frame, request it again this often, in case the request
// or the IDR frame itself was lost; otherwise the picture would stay frozen
#define VIDEO_IDR_RETRY_MIN_MS 400
#define VIDEO_IDR_RETRY_MAX_MS 1000

static ChiakiErrorCode chiaki_video_receiver_flush_frame(ChiakiVideoReceiver *video_receiver);

static void add_ref_frame(ChiakiVideoReceiver *video_receiver, int32_t frame)
{
	if(video_receiver->reference_frames[0] != -1)
	{
		memmove(&video_receiver->reference_frames[1], &video_receiver->reference_frames[0], sizeof(int32_t) * 15);
		video_receiver->reference_frames[0] = frame;
		return;
	}
	for(int i=15; i>=0; i--)
	{
		if(video_receiver->reference_frames[i] == -1)
		{
			video_receiver->reference_frames[i] = frame;
			return;
		}
	}
}

static bool have_ref_frame(ChiakiVideoReceiver *video_receiver, int32_t frame)
{
	for(int i=0; i<16; i++)
		if(video_receiver->reference_frames[i] == frame)
			return true;
	return false;
}

CHIAKI_EXPORT void chiaki_video_receiver_init(ChiakiVideoReceiver *video_receiver, struct chiaki_session_t *session, ChiakiPacketStats *packet_stats)
{
	video_receiver->session = session;
	video_receiver->log = session->log;
	memset(video_receiver->profiles, 0, sizeof(video_receiver->profiles));
	video_receiver->profiles_count = 0;
	video_receiver->profile_cur = -1;

	video_receiver->frame_index_cur = -1;
	video_receiver->frame_index_prev = -1;
	video_receiver->frame_follows_gap = false;

	chiaki_frame_processor_init(&video_receiver->frame_processor, video_receiver->log);
	chiaki_frame_processor_init(&video_receiver->next_frame_processor, video_receiver->log);
	video_receiver->next_frame_index = -1;
	video_receiver->next_frame_profile = -1;
	video_receiver->next_frame_saw_last_unit = false;
	video_receiver->packet_stats = packet_stats;

	video_receiver->frames_lost = 0;
	video_receiver->frames_lost_total = 0;
	video_receiver->reorder_deadline_ms = 0;
	memset(&video_receiver->recovery_stats, 0, sizeof(video_receiver->recovery_stats));
	memset(video_receiver->reference_frames, -1, sizeof(video_receiver->reference_frames));
	chiaki_bitstream_init(&video_receiver->bitstream, video_receiver->log, video_receiver->session->connect_info.video_profile.codec);
	chiaki_mutex_init(&video_receiver->waiting_for_idr_mutex, false);
	video_receiver->waiting_for_idr = false;
	video_receiver->idr_request_last_ms = 0;
	chiaki_mutex_init(&video_receiver->frames_lost_mutex, false);
}

CHIAKI_EXPORT void chiaki_video_receiver_fini(ChiakiVideoReceiver *video_receiver)
{
	for(size_t i=0; i<video_receiver->profiles_count; i++)
		free(video_receiver->profiles[i].header);
	chiaki_mutex_fini(&video_receiver->waiting_for_idr_mutex);
	chiaki_mutex_fini(&video_receiver->frames_lost_mutex);
	chiaki_frame_processor_fini(&video_receiver->frame_processor);
	chiaki_frame_processor_fini(&video_receiver->next_frame_processor);
}

CHIAKI_EXPORT void chiaki_video_receiver_set_waiting_for_idr(ChiakiVideoReceiver *video_receiver, bool waiting_for_idr)
{
	chiaki_mutex_lock(&video_receiver->waiting_for_idr_mutex);
	video_receiver->waiting_for_idr = waiting_for_idr;
	if(waiting_for_idr)
		video_receiver->idr_request_last_ms = chiaki_time_now_monotonic_ms();
	chiaki_mutex_unlock(&video_receiver->waiting_for_idr_mutex);
}

static bool video_receiver_request_idr(ChiakiVideoReceiver *receiver)
{
    uint64_t rtt_us = receiver->session->rtt_us;
    const uint64_t live_rtt = chiaki_takion_send_buffer_get_rtt_us(&receiver->session->stream_connection.takion.send_buffer);
    if(live_rtt > 0)
        rtt_us = live_rtt;
    uint64_t interval_ms = rtt_us / 500;
    if(interval_ms < VIDEO_IDR_RETRY_MIN_MS)
        interval_ms = VIDEO_IDR_RETRY_MIN_MS;
    if(interval_ms > VIDEO_IDR_RETRY_MAX_MS)
        interval_ms = VIDEO_IDR_RETRY_MAX_MS;

    chiaki_mutex_lock(&receiver->waiting_for_idr_mutex);
    const uint64_t now_ms = chiaki_time_now_monotonic_ms();
    const bool waiting = receiver->waiting_for_idr;
    const bool due = receiver->idr_request_last_ms == 0
        || now_ms - receiver->idr_request_last_ms >= interval_ms;
    if(due)
        receiver->idr_request_last_ms = now_ms;
    chiaki_mutex_unlock(&receiver->waiting_for_idr_mutex);
    if(!due)
        return waiting;

    ChiakiErrorCode err = stream_connection_send_idr_request(&receiver->session->stream_connection);
    if(err != CHIAKI_ERR_SUCCESS)
    {
        CHIAKI_LOGW(receiver->log, "Could not request IDR frame: %s", chiaki_error_string(err));
        return waiting;
    }

    chiaki_video_receiver_set_waiting_for_idr(receiver, true);
    chiaki_mutex_lock(&receiver->frames_lost_mutex);
    receiver->recovery_stats.idr_requests++;
    chiaki_mutex_unlock(&receiver->frames_lost_mutex);
    return true;
}

CHIAKI_EXPORT bool chiaki_video_receiver_get_waiting_for_idr(ChiakiVideoReceiver *video_receiver)
{
	bool waiting_for_idr;
	chiaki_mutex_lock(&video_receiver->waiting_for_idr_mutex);
	waiting_for_idr = video_receiver->waiting_for_idr;
	chiaki_mutex_unlock(&video_receiver->waiting_for_idr_mutex);
	return waiting_for_idr;
}

CHIAKI_EXPORT int32_t chiaki_video_receiver_get_frames_lost_total(ChiakiVideoReceiver *video_receiver)
{
	int32_t total;
	chiaki_mutex_lock(&video_receiver->frames_lost_mutex);
	total = video_receiver->frames_lost_total;
	chiaki_mutex_unlock(&video_receiver->frames_lost_mutex);
	return total;
}

static void video_receiver_mark_loss(ChiakiVideoReceiver *video_receiver)
{
    chiaki_mutex_lock(&video_receiver->frames_lost_mutex);
    if(!video_receiver->recovery_stats.loss_started_ms)
        video_receiver->recovery_stats.loss_started_ms = chiaki_time_now_monotonic_ms();
    chiaki_mutex_unlock(&video_receiver->frames_lost_mutex);
}

static void video_receiver_add_lost_frames(ChiakiVideoReceiver *receiver, uint16_t count)
{
    chiaki_mutex_lock(&receiver->frames_lost_mutex);
    receiver->frames_lost += count;
    receiver->frames_lost_total += count;
    chiaki_mutex_unlock(&receiver->frames_lost_mutex);
}

CHIAKI_EXPORT ChiakiVideoRecoveryStats chiaki_video_receiver_get_recovery_stats(ChiakiVideoReceiver *video_receiver)
{
    chiaki_mutex_lock(&video_receiver->frames_lost_mutex);
    ChiakiVideoRecoveryStats stats = video_receiver->recovery_stats;
    chiaki_mutex_unlock(&video_receiver->frames_lost_mutex);
    return stats;
}

CHIAKI_EXPORT void chiaki_video_receiver_stream_info(ChiakiVideoReceiver *video_receiver, ChiakiVideoProfile *profiles, size_t profiles_count)
{
	if(video_receiver->profiles_count > 0)
	{
		CHIAKI_LOGE(video_receiver->log, "Video Receiver profiles already set");
		return;
	}

	memcpy(video_receiver->profiles, profiles, profiles_count * sizeof(ChiakiVideoProfile));
	video_receiver->profiles_count = profiles_count;

	CHIAKI_LOGI(video_receiver->log, "Video Profiles:");
	for(size_t i=0; i<video_receiver->profiles_count; i++)
	{
		ChiakiVideoProfile *profile = &video_receiver->profiles[i];
		CHIAKI_LOGI(video_receiver->log, "  %zu: %ux%u", i, profile->width, profile->height);
		//chiaki_log_hexdump(video_receiver->log, CHIAKI_LOG_DEBUG, profile->header, profile->header_sz);
	}
}

static bool video_receiver_apply_profile(ChiakiVideoReceiver *video_receiver, int profile_index)
{
	if(profile_index < 0 || (size_t)profile_index >= video_receiver->profiles_count)
	{
		CHIAKI_LOGE(video_receiver->log, "Packet has invalid adaptive stream index %d >= %u",
			profile_index, (unsigned int)video_receiver->profiles_count);
		return false;
	}
	if(video_receiver->profile_cur == profile_index)
		return true;

	video_receiver->profile_cur = profile_index;
	ChiakiVideoProfile *profile = video_receiver->profiles + video_receiver->profile_cur;
	CHIAKI_LOGI(video_receiver->log, "Switched to profile %d, resolution: %ux%u",
		video_receiver->profile_cur, profile->width, profile->height);
	if(video_receiver->session->video_sample_cb)
		video_receiver->session->video_sample_cb(profile->header, profile->header_sz, 0, false,
			video_receiver->session->video_sample_cb_user);
	if(!chiaki_bitstream_header(&video_receiver->bitstream, profile->header, profile->header_sz))
		CHIAKI_LOGW(video_receiver->log, "Failed to parse video header");
	return true;
}

static void video_receiver_promote_next_frame(ChiakiVideoReceiver *video_receiver, uint64_t now_ms, uint8_t reorder_window)
{
	if(video_receiver->next_frame_index < 0)
		return;
	if(video_receiver->packet_stats)
		chiaki_frame_processor_report_packet_stats(&video_receiver->frame_processor, video_receiver->packet_stats);
	ChiakiFrameProcessor tmp = video_receiver->frame_processor;
	video_receiver->frame_processor = video_receiver->next_frame_processor;
	video_receiver->next_frame_processor = tmp;
	video_receiver->frame_index_cur = video_receiver->next_frame_index;
	video_receiver->next_frame_index = -1;
	const int promoted_profile = video_receiver->next_frame_profile;
	video_receiver->next_frame_profile = -1;
	const bool saw_last = video_receiver->next_frame_saw_last_unit;
	video_receiver->next_frame_saw_last_unit = false;
	video_receiver->frame_follows_gap = false;
	video_receiver->reorder_deadline_ms = 0;

	if(promoted_profile >= 0)
		video_receiver_apply_profile(video_receiver, promoted_profile);

	if(chiaki_frame_processor_flush_possible(&video_receiver->frame_processor) || (saw_last && !reorder_window))
	{
		ChiakiErrorCode err = chiaki_video_receiver_flush_frame(video_receiver);
		if(err != CHIAKI_ERR_SUCCESS)
			CHIAKI_LOGW(video_receiver->log, "Video receiver could not flush promoted frame.");
	}
	else if(saw_last && reorder_window)
		video_receiver->reorder_deadline_ms = now_ms + reorder_window;
}

CHIAKI_EXPORT void chiaki_video_receiver_av_packet(ChiakiVideoReceiver *video_receiver, ChiakiTakionAVPacket *packet)
{
	// old frame?
	ChiakiSeqNum16 frame_index = packet->frame_index;
	ChiakiErrorCode err = CHIAKI_ERR_SUCCESS;
	if(video_receiver->frame_index_cur >= 0
		&& chiaki_seq_num_16_lt(frame_index, (ChiakiSeqNum16)video_receiver->frame_index_cur))
	{
		CHIAKI_LOGW(video_receiver->log, "Video Receiver received old frame packet");
		return;
	}

	if(packet->adaptive_stream_index >= video_receiver->profiles_count)
	{
		CHIAKI_LOGE(video_receiver->log, "Packet has invalid adaptive stream index %u >= %u",
				(unsigned int)packet->adaptive_stream_index,
				(unsigned int)video_receiver->profiles_count);
		return;
	}

	const uint8_t reorder_window = video_receiver->session->connect_info.video_reorder_window_ms;
	const uint64_t now_ms = reorder_window ? chiaki_time_now_monotonic_ms() : 0;

	// Cross-frame reordering: if current frame N is still incomplete and within reorder_window,
	// buffer early packets of frame N+1 in next_frame_processor instead of prematurely killing frame N.
	if(reorder_window > 0
		&& video_receiver->frame_index_cur >= 0
		&& video_receiver->frame_index_prev != video_receiver->frame_index_cur
		&& frame_index == (ChiakiSeqNum16)(video_receiver->frame_index_cur + 1))
	{
		const bool expired = video_receiver->reorder_deadline_ms && now_ms >= video_receiver->reorder_deadline_ms;
		if(!expired)
		{
			if(!video_receiver->reorder_deadline_ms)
				video_receiver->reorder_deadline_ms = now_ms + reorder_window;
			if(video_receiver->next_frame_index < 0)
			{
				err = chiaki_frame_processor_alloc_frame(&video_receiver->next_frame_processor, packet);
				if(err != CHIAKI_ERR_SUCCESS)
				{
					CHIAKI_LOGW(video_receiver->log, "Video receiver could not allocate next frame for packet.");
					return;
				}
				video_receiver->next_frame_index = frame_index;
				video_receiver->next_frame_saw_last_unit = false;
			}
			video_receiver->next_frame_profile = packet->adaptive_stream_index;
			err = chiaki_frame_processor_put_unit(&video_receiver->next_frame_processor, packet);
			if(err != CHIAKI_ERR_SUCCESS)
				CHIAKI_LOGW(video_receiver->log, "Video receiver could not put unit into next frame.");
			if(packet->unit_index == packet->units_in_frame_total - 1)
				video_receiver->next_frame_saw_last_unit = true;
			return;
		}
		// Deadline expired: flush frame N and promote N+1 (if already buffered) before handling this packet.
		err = chiaki_video_receiver_flush_frame(video_receiver);
		if(err != CHIAKI_ERR_SUCCESS)
			CHIAKI_LOGW(video_receiver->log, "Video receiver could not flush expired frame.");
		if(video_receiver->next_frame_index == frame_index)
			video_receiver_promote_next_frame(video_receiver, now_ms, reorder_window);
	}

	// next frame?
	if(video_receiver->frame_index_cur < 0 ||
		chiaki_seq_num_16_gt(frame_index, (ChiakiSeqNum16)video_receiver->frame_index_cur))
	{
		if(video_receiver->next_frame_index >= 0)
		{
			if(video_receiver->frame_index_cur >= 0 && video_receiver->frame_index_prev != video_receiver->frame_index_cur)
				chiaki_video_receiver_flush_frame(video_receiver);
			video_receiver_promote_next_frame(video_receiver, now_ms, reorder_window);
		}
		if(video_receiver->frame_index_cur < 0 ||
			chiaki_seq_num_16_gt(frame_index, (ChiakiSeqNum16)video_receiver->frame_index_cur))
		{
			if(video_receiver->packet_stats)
				chiaki_frame_processor_report_packet_stats(&video_receiver->frame_processor, video_receiver->packet_stats);

			// last frame not flushed yet?
			if(video_receiver->frame_index_cur >= 0 && video_receiver->frame_index_prev != video_receiver->frame_index_cur)
				err = chiaki_video_receiver_flush_frame(video_receiver);

			if(err != CHIAKI_ERR_SUCCESS)
				CHIAKI_LOGW(video_receiver->log, "Video receiver could not flush frame.");

			const ChiakiSeqNum16 next_frame_expected = (ChiakiSeqNum16)(video_receiver->frame_index_cur + 1);
			video_receiver->frame_follows_gap = video_receiver->frame_index_cur >= 0 && frame_index != next_frame_expected;
			if(video_receiver->frame_follows_gap)
			{
				const ChiakiSeqNum16 last_missing = (ChiakiSeqNum16)(frame_index - 1);
				video_receiver_mark_loss(video_receiver);
				video_receiver_add_lost_frames(video_receiver, (ChiakiSeqNum16)(frame_index - next_frame_expected));
				CHIAKI_LOGW(video_receiver->log, "Missing video frame(s) from %u to %u",
					(unsigned)next_frame_expected, (unsigned)last_missing);
				err = stream_connection_send_corrupt_frame(&video_receiver->session->stream_connection, next_frame_expected, last_missing);
				if(err != CHIAKI_ERR_SUCCESS)
					CHIAKI_LOGW(video_receiver->log, "Error sending corrupt frame.");
				if(video_receiver->session->connect_info.always_request_idr_on_loss)
					video_receiver_request_idr(video_receiver);
			}

			video_receiver->frame_index_cur = frame_index;
			video_receiver->reorder_deadline_ms = 0;
			err = chiaki_frame_processor_alloc_frame(&video_receiver->frame_processor, packet);
			if(err != CHIAKI_ERR_SUCCESS)
				CHIAKI_LOGW(video_receiver->log, "Video receiver could not allocate frame for packet.");
		}
	}

	video_receiver_apply_profile(video_receiver, packet->adaptive_stream_index);

	if(video_receiver->reorder_deadline_ms && now_ms >= video_receiver->reorder_deadline_ms
		&& video_receiver->frame_index_cur != video_receiver->frame_index_prev)
	{
		chiaki_video_receiver_flush_frame(video_receiver);
		if(video_receiver->next_frame_index >= 0)
			video_receiver_promote_next_frame(video_receiver, now_ms, reorder_window);
		return;
	}

	err = chiaki_frame_processor_put_unit(&video_receiver->frame_processor, packet);
	if(err != CHIAKI_ERR_SUCCESS)
		CHIAKI_LOGW(video_receiver->log, "Video receiver could not put unit.");

	// if we are currently building up a frame
	if(video_receiver->frame_index_cur != video_receiver->frame_index_prev)
	{
		// if we already have enough for the whole frame, flush it already
		const bool last_unit = packet->unit_index == packet->units_in_frame_total - 1;
		if(chiaki_frame_processor_flush_possible(&video_receiver->frame_processor) || (last_unit && !reorder_window))
		{
			err = chiaki_video_receiver_flush_frame(video_receiver);
			if(video_receiver->next_frame_index >= 0)
				video_receiver_promote_next_frame(video_receiver, now_ms, reorder_window);
		}
		else if(last_unit && !video_receiver->reorder_deadline_ms)
			video_receiver->reorder_deadline_ms = now_ms + reorder_window;
		if(err != CHIAKI_ERR_SUCCESS)
			CHIAKI_LOGW(video_receiver->log, "Video receiver could not flush frame.");
	}
}

static ChiakiErrorCode chiaki_video_receiver_flush_frame(ChiakiVideoReceiver *video_receiver)
{
    const bool waited_for_reorder = video_receiver->reorder_deadline_ms != 0;
    video_receiver->reorder_deadline_ms = 0;
    uint8_t *frame;
    size_t frame_size;
    const ChiakiFrameProcessorFlushResult flush_result = chiaki_frame_processor_flush(&video_receiver->frame_processor, &frame, &frame_size);
    const bool auto_idr = video_receiver->session->connect_info.enable_idr_on_fec_failure
        || video_receiver->session->connect_info.always_request_idr_on_loss;

    if(flush_result == CHIAKI_FRAME_PROCESSOR_FLUSH_RESULT_FAILED
        || flush_result == CHIAKI_FRAME_PROCESSOR_FLUSH_RESULT_FEC_FAILED)
    {
        video_receiver_mark_loss(video_receiver);
        video_receiver_add_lost_frames(video_receiver, 1);
        video_receiver->frame_index_prev = video_receiver->frame_index_cur;
        stream_connection_send_corrupt_frame(&video_receiver->session->stream_connection,
            video_receiver->frame_index_cur, video_receiver->frame_index_cur);
        if(auto_idr)
        {
            const bool idr_request_sent = video_receiver_request_idr(video_receiver);
            if(flush_result == CHIAKI_FRAME_PROCESSOR_FLUSH_RESULT_FEC_FAILED)
            {
                ChiakiEvent event = { 0 };
                event.type = CHIAKI_EVENT_VIDEO_FEC_FAILURE;
                event.video_fec_failure.frame_index = video_receiver->frame_index_cur;
                event.video_fec_failure.idr_request_sent = idr_request_sent;
                chiaki_session_send_event(video_receiver->session, &event);
            }
        }
        CHIAKI_LOGW(video_receiver->log, "Failed to complete frame %d", (int)video_receiver->frame_index_cur);
        return CHIAKI_ERR_UNKNOWN;
    }

    if(waited_for_reorder || flush_result == CHIAKI_FRAME_PROCESSOR_FLUSH_RESULT_FEC_SUCCESS)
    {
        chiaki_mutex_lock(&video_receiver->frames_lost_mutex);
        if(waited_for_reorder)
            video_receiver->recovery_stats.reordered_frames++;
        if(flush_result == CHIAKI_FRAME_PROCESSOR_FLUSH_RESULT_FEC_SUCCESS)
            video_receiver->recovery_stats.fec_recovered_frames++;
        chiaki_mutex_unlock(&video_receiver->frames_lost_mutex);
    }

    bool succ = true;
    bool recovered = false;
    ChiakiBitstreamSlice slice = {0};
    const bool slice_parsed = chiaki_bitstream_slice(&video_receiver->bitstream, frame, frame_size, &slice);
    const bool intra = slice_parsed && slice.slice_type == CHIAKI_BITSTREAM_SLICE_I;
    const bool clean_ref_p = slice_parsed && slice.slice_type == CHIAKI_BITSTREAM_SLICE_P
        && slice.reference_frame != 0xff
        && have_ref_frame(video_receiver, (ChiakiSeqNum16)(video_receiver->frame_index_cur - slice.reference_frame - 1));
    if(auto_idr && video_receiver->frame_follows_gap && !slice_parsed)
        video_receiver_request_idr(video_receiver);
    if(!intra && !clean_ref_p && chiaki_video_receiver_get_waiting_for_idr(video_receiver))
    {
        video_receiver_request_idr(video_receiver);
        video_receiver->frame_index_prev = video_receiver->frame_index_cur;
        chiaki_mutex_lock(&video_receiver->frames_lost_mutex);
        video_receiver->frames_lost++;
        video_receiver->recovery_stats.frames_held_for_keyframe++;
        chiaki_mutex_unlock(&video_receiver->frames_lost_mutex);
        return CHIAKI_ERR_SUCCESS;
    }

    if(slice_parsed)
    {
		if(slice.slice_type == CHIAKI_BITSTREAM_SLICE_P)
		{
			ChiakiSeqNum16 ref_frame_index = video_receiver->frame_index_cur - slice.reference_frame - 1;
			if(slice.reference_frame != 0xff && !have_ref_frame(video_receiver, ref_frame_index))
			{
				for(unsigned i=slice.reference_frame+1; i<16; i++)
				{
					ChiakiSeqNum16 ref_frame_index_new = (ChiakiSeqNum16)(video_receiver->frame_index_cur - i - 1);
					if(have_ref_frame(video_receiver, ref_frame_index_new))
					{
						if(chiaki_bitstream_slice_set_reference_frame(&video_receiver->bitstream, frame, frame_size, i))
						{
							recovered = true;
							if(video_receiver->session->connect_info.always_request_idr_on_loss)
								video_receiver_request_idr(video_receiver);
							CHIAKI_LOGW(video_receiver->log, "Missing reference frame %d for decoding frame %d -> changed to %d", (int)ref_frame_index, (int)video_receiver->frame_index_cur, (int)ref_frame_index_new);
						}
						break;
					}
				}
				if(!recovered)
				{
					succ = false;
					video_receiver_mark_loss(video_receiver);
                    video_receiver_add_lost_frames(video_receiver, 1);
                    stream_connection_send_corrupt_frame(&video_receiver->session->stream_connection,
                        video_receiver->frame_index_cur, video_receiver->frame_index_cur);
                    if(auto_idr)
                        video_receiver_request_idr(video_receiver);
					CHIAKI_LOGW(video_receiver->log, "Missing reference frame %d for decoding frame %d", (int)ref_frame_index, (int)video_receiver->frame_index_cur);
				}
			}
		}
	}

	if(succ && video_receiver->session->video_sample_cb)
	{
		bool cb_succ = video_receiver->session->video_sample_cb(frame, frame_size, video_receiver->frames_lost, recovered, video_receiver->session->video_sample_cb_user);
		chiaki_mutex_lock(&video_receiver->frames_lost_mutex);
		video_receiver->frames_lost = 0;
		chiaki_mutex_unlock(&video_receiver->frames_lost_mutex);
		if(!cb_succ)
		{
			video_receiver_mark_loss(video_receiver);
            video_receiver_add_lost_frames(video_receiver, 1);
            if(auto_idr || chiaki_video_receiver_get_waiting_for_idr(video_receiver))
                video_receiver_request_idr(video_receiver);
			CHIAKI_LOGW(video_receiver->log, "Video callback did not process frame successfully.");
		}
		else
		{
            if(intra || clean_ref_p)
                chiaki_video_receiver_set_waiting_for_idr(video_receiver, false);
            const bool recovered_clean = intra || clean_ref_p || (recovered && !chiaki_video_receiver_get_waiting_for_idr(video_receiver));
            chiaki_mutex_lock(&video_receiver->frames_lost_mutex);
            video_receiver->recovery_stats.assembled_video_bytes += frame_size;
            if(recovered_clean && video_receiver->recovery_stats.loss_started_ms)
            {
                video_receiver->recovery_stats.last_recovery_ms = chiaki_time_now_monotonic_ms()
                    - video_receiver->recovery_stats.loss_started_ms;
                video_receiver->recovery_stats.loss_started_ms = 0;
                video_receiver->recovery_stats.has_recovered = true;
            }
            chiaki_mutex_unlock(&video_receiver->frames_lost_mutex);
			add_ref_frame(video_receiver, video_receiver->frame_index_cur);
			CHIAKI_LOGV(video_receiver->log, "Added reference %c frame %d", slice.slice_type == CHIAKI_BITSTREAM_SLICE_I ? 'I' : 'P', (int)video_receiver->frame_index_cur);
		}
	}

	video_receiver->frame_index_prev = video_receiver->frame_index_cur;

	return CHIAKI_ERR_SUCCESS;
}
