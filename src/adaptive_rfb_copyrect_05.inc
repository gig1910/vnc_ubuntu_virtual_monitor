    )
        may_try_copy = 1;

    unlock_client_update(cl);
    rfbDecrClientRef(cl);

    CopyPlan plan;
    memset(&plan, 0, sizeof(plan));

    if (
        !may_try_copy ||
        !copy_find_plan(
            state,
            sidecar,
            &pending,
            &plan
        )
    ) {
        int rc =
            adaptive_rfb_try_send_pending_legacy(
                state,
                transport_ready,
                now
            );

        if (rc > 0)
            copy_reference_snapshot(state, sidecar);

        return rc;
    }

    cl = get_jpeg_client_ref(state);
    if (!cl)
        return 0;

    int reserved_request = 0;

    lock_client_send(cl);
    lock_client_update(cl);

    if (
        cl->sock != RFB_INVALID_SOCKET &&
        cl->state == RFB_NORMAL &&
        !cl->onHold &&
        cl->useCopyRect &&
        !FB_UPDATE_PENDING(cl) &&
        requested_covers_rect_locked(
            cl,
            pending.x1,
            pending.y1,
            pending.x2,
            pending.y2
        )
    ) {
        sraRgnMakeEmpty(cl->requestedRegion);
        reserved_request = 1;
    }

    unlock_client_update(cl);

    if (!reserved_request) {
        unlock_client_send(cl);
        rfbDecrClientRef(cl);
        return 0;
    }

    unsigned long jpeg_size = 0;
    int send_rc =
        copy_send_update(
            cl,
            &plan,
            &jpeg_size
        );

    unlock_client_send(cl);

    if (send_rc < 0) {
        fprintf(
            stderr,
            "[COPY] update send failed; closing backend client "
            "to avoid RFB state ambiguity\n"
        );
        rfbCloseClient(cl);
        rfbDecrClientRef(cl);
        return -1;
    }

    rfbDecrClientRef(cl);

    uint64_t raw_bytes =
        copy_box_area(&pending) * 4ULL;
    size_t repair_remaining;

    pthread_mutex_lock(&state->mutex);

    if (
        state->pending_jpeg &&
        state->pending_sequence == sequence
    ) {
        state->pending_jpeg = 0;
        state->pending_source_frames = 0;
        state->pending_changed_percent = 0.0;

        repair_mark_rect_locked(
            state,
            pending.x1,
            pending.y1,
            pending.x2,
            pending.y2
        );

        state->last_lossy_at = now;
        state->jpeg_updates++;
        state->jpeg_bytes += jpeg_size;
        state->jpeg_raw_bytes += raw_bytes;
    }

    repair_remaining = state->repair_count;
    pthread_mutex_unlock(&state->mutex);

    copy_reference_snapshot(state, sidecar);

    sidecar->copy_updates++;
    sidecar->copy_pixels += plan.copy_pixels;
    sidecar->residual_pixels += plan.jpeg_pixels;
    sidecar->residual_jpeg_bytes += jpeg_size;

    uint64_t pending_pixels = copy_box_area(&pending);
    double copy_share =
        pending_pixels > 0
            ? (double)plan.copy_pixels * 100.0 /
                (double)pending_pixels
            : 0.0;
    double ratio =
        raw_bytes > 0
            ? (double)jpeg_size * 100.0 /
                (double)raw_bytes
            : 0.0;

    fprintf(
        stderr,
        "[COPY] seq=%llu shift=%+d,%+d copy=%d,%d %dx%d "
        "share=%.1f%% residuals=%d jpeg-pixels=%llu bytes=%lu\n",
        (unsigned long long)sequence,
        plan.dx,
        plan.dy,
        plan.copy.x1,
        plan.copy.y1,
        plan.copy.x2 - plan.copy.x1,
        plan.copy.y2 - plan.copy.y1,
        copy_share,
        plan.jpeg_count,
        (unsigned long long)plan.jpeg_pixels,
        jpeg_size
    );

    fprintf(
        stderr,
        "[ADAPT] JPEG21 seq=%llu rect=%d,%d %dx%d "
        "source-frames=%llu changed<=%.1f%% quality=%d "
        "bytes=%lu raw=%llu ratio=%.1f%% repair=%zu copy=yes\n",
        (unsigned long long)sequence,
        pending.x1,
        pending.y1,
        pending.x2 - pending.x1,
        pending.y2 - pending.y1,
        (unsigned long long)coalesced_frames,
        changed_percent,
        ADAPTIVE_RFB_JPEG_QUALITY,
        jpeg_size,
        (unsigned long long)raw_bytes,
        ratio,
        repair_remaining
    );

    return 1;
}

void
adaptive_rfb_print_summary(AdaptiveRfbState *state)
