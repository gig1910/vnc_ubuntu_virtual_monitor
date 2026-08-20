            (const char *)&copy,
            sz_rfbCopyRect
        ) > 0;
}

static int
copy_write_jpeg(
    rfbClientPtr cl,
    const EncodedRect *encoded)
{
    rfbFramebufferUpdateRectHeader header;
    memset(&header, 0, sizeof(header));

    header.r.x = Swap16IfLE((uint16_t)encoded->rect.x1);
    header.r.y = Swap16IfLE((uint16_t)encoded->rect.y1);
    header.r.w =
        Swap16IfLE((uint16_t)(encoded->rect.x2 - encoded->rect.x1));
    header.r.h =
        Swap16IfLE((uint16_t)(encoded->rect.y2 - encoded->rect.y1));
    header.encoding = Swap32IfLE(ADAPTIVE_RFB_JPEG_ENCODING);

    return
        rfbWriteExact(
            cl,
            (const char *)&header,
            sz_rfbFramebufferUpdateRectHeader
        ) > 0 &&
        rfbWriteExact(
            cl,
            (const char *)encoded->data,
            (int)encoded->size
        ) > 0;
}

static int
copy_send_update(
    rfbClientPtr cl,
    const CopyPlan *plan,
    unsigned long *jpeg_size)
{
    EncodedRect encoded[4];
    memset(encoded, 0, sizeof(encoded));

    if (
        copy_encode_residuals(
            cl->screen,
            plan,
            encoded,
            jpeg_size
        ) < 0
    ) {
        fprintf(stderr, "[COPY] residual JPEG encode failed\n");
        return -1;
    }

    rfbFramebufferUpdateMsg update;
    memset(&update, 0, sizeof(update));
    update.type = rfbFramebufferUpdate;
    update.nRects =
        Swap16IfLE((uint16_t)(1 + plan->jpeg_count));

    int ok =
        rfbWriteExact(
            cl,
            (const char *)&update,
            sz_rfbFramebufferUpdateMsg
        ) > 0 &&
        copy_write_copyrect(cl, plan);

    for (int i = 0; ok && i < plan->jpeg_count; i++)
        ok = copy_write_jpeg(cl, &encoded[i]);

    copy_free_encoded(encoded, plan->jpeg_count);
    return ok ? 0 : -1;
}

AdaptiveRfbState *
adaptive_rfb_create(
    rfbScreenInfoPtr screen,
    int width,
    int height)
{
    AdaptiveRfbState *state =
        adaptive_rfb_create_legacy(screen, width, height);

    if (!state)
        return NULL;

    if (copy_sidecar_add(state) < 0) {
        fprintf(stderr, "[COPY] failed to allocate CopyRect reference frame\n");
        adaptive_rfb_destroy_legacy(state);
        return NULL;
    }

    fprintf(
        stderr,
        "[COPY] experimental CopyRect detector enabled: radius=%dpx "
        "verify=%dpx min=%llu px (%.0f%% of pending bbox)\n",
        COPY_SEARCH_RADIUS,
        COPY_VERIFY_TILE,
        (unsigned long long)COPY_MIN_PIXELS,
        COPY_MIN_SHARE_PERCENT
    );

    return state;
}

void
adaptive_rfb_destroy(AdaptiveRfbState *state)
{
    if (!state)
        return;

    copy_sidecar_remove(state);
    adaptive_rfb_destroy_legacy(state);
}

int
adaptive_rfb_try_send_pending(
    AdaptiveRfbState *state,
    int transport_ready,
    double now)
{
    CopySidecar *sidecar = copy_sidecar_find(state);

    if (!state || !sidecar || !transport_ready)
        return adaptive_rfb_try_send_pending_legacy(
            state,
            transport_ready,
            now
        );

    CopyRectBox pending;
    uint64_t sequence;
    uint64_t coalesced_frames;
    double changed_percent;

    pthread_mutex_lock(&state->mutex);

    if (
        !state->pending_jpeg ||
        !state->jpeg_supported ||
        !state->jpeg_client
    ) {
        pthread_mutex_unlock(&state->mutex);
        return 0;
    }

    pending.x1 = state->pending_x1;
    pending.y1 = state->pending_y1;
    pending.x2 = state->pending_x2;
    pending.y2 = state->pending_y2;
    sequence = state->pending_sequence;
    coalesced_frames = state->pending_source_frames;
    changed_percent = state->pending_changed_percent;

    pthread_mutex_unlock(&state->mutex);

    rfbClientPtr cl = get_jpeg_client_ref(state);

    if (!cl)
        return 0;

    int may_try_copy = 0;

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
