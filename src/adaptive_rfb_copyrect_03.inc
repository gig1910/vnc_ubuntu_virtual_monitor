                    (size_t)state->screen->paddedWidthInBytes +
                (size_t)current_x * 4
            );

        uint32_t signature0 = current_first[0];
        uint32_t signature1 = current_first[patch - 1];
        uint32_t signature2 = current_last[0];
        uint32_t signature3 = current_last[patch - 1];

        for (int source_y = source_y1; source_y <= source_y2; source_y++) {
            for (int source_x = source_x1; source_x <= source_x2; source_x++) {
                int dx = current_x - source_x;
                int dy = current_y - source_y;

                if (dx == 0 && dy == 0)
                    continue;

                if (
                    copy_candidate_seen(
                        candidate_dxs,
                        candidate_dys,
                        candidate_count,
                        dx,
                        dy
                    )
                )
                    continue;

                const uint32_t *reference_first =
                    (const uint32_t *)(
                        sidecar->reference +
                        ((size_t)source_y *
                            (size_t)state->width +
                         (size_t)source_x) * 4
                    );
                const uint32_t *reference_last =
                    (const uint32_t *)(
                        sidecar->reference +
                        ((size_t)(source_y + patch - 1) *
                            (size_t)state->width +
                         (size_t)source_x) * 4
                    );

                if (
                    reference_first[0] != signature0 ||
                    reference_first[patch - 1] != signature1 ||
                    reference_last[0] != signature2 ||
                    reference_last[patch - 1] != signature3
                )
                    continue;

                if (
                    !copy_patch_matches(
                        state,
                        sidecar,
                        current_x,
                        current_y,
                        source_x,
                        source_y
                    )
                )
                    continue;

                if (candidate_count < COPY_MAX_CANDIDATES) {
                    candidate_dxs[candidate_count] = dx;
                    candidate_dys[candidate_count] = dy;
                    candidate_count++;
                }

                CopyRectBox copy;
                uint64_t copy_pixels = 0;

                if (
                    copy_largest_rect_for_shift(
                        state,
                        sidecar,
                        pending,
                        dx,
                        dy,
                        &copy,
                        &copy_pixels
                    )
                ) {
                    plan->dx = dx;
                    plan->dy = dy;
                    plan->copy = copy;
                    plan->copy_pixels = copy_pixels;
                    copy_build_residuals(plan, pending);
                    return 1;
                }

                if (candidate_count >= COPY_MAX_CANDIDATES)
                    return 0;
            }
        }
    }

    return 0;
}

static void
copy_free_encoded(
    EncodedRect *encoded,
    int count)
{
    for (int i = 0; i < count; i++) {
        free(encoded[i].data);
        encoded[i].data = NULL;
        encoded[i].size = 0;
    }
}

static int
copy_encode_residuals(
    rfbScreenInfoPtr screen,
    const CopyPlan *plan,
    EncodedRect *encoded,
    unsigned long *total_size)
{
    *total_size = 0;

    for (int i = 0; i < plan->jpeg_count; i++) {
        encoded[i].rect = plan->jpeg[i];
        encoded[i].data = NULL;
        encoded[i].size = 0;

        if (
            encode_jpeg_rect(
                screen,
                encoded[i].rect.x1,
                encoded[i].rect.y1,
                encoded[i].rect.x2 - encoded[i].rect.x1,
                encoded[i].rect.y2 - encoded[i].rect.y1,
                &encoded[i].data,
                &encoded[i].size
            ) < 0 ||
            encoded[i].size > (unsigned long)INT_MAX
        ) {
            copy_free_encoded(encoded, i + 1);
            return -1;
        }

        *total_size += encoded[i].size;
    }

    return 0;
}

static int
copy_write_copyrect(
    rfbClientPtr cl,
    const CopyPlan *plan)
{
    rfbFramebufferUpdateRectHeader header;
    rfbCopyRect copy;

    memset(&header, 0, sizeof(header));
    memset(&copy, 0, sizeof(copy));

    header.r.x = Swap16IfLE((uint16_t)plan->copy.x1);
    header.r.y = Swap16IfLE((uint16_t)plan->copy.y1);
    header.r.w =
        Swap16IfLE((uint16_t)(plan->copy.x2 - plan->copy.x1));
    header.r.h =
        Swap16IfLE((uint16_t)(plan->copy.y2 - plan->copy.y1));
    header.encoding = Swap32IfLE(rfbEncodingCopyRect);

    copy.srcX =
        Swap16IfLE((uint16_t)(plan->copy.x1 - plan->dx));
    copy.srcY =
        Swap16IfLE((uint16_t)(plan->copy.y1 - plan->dy));

    return
        rfbWriteExact(
            cl,
            (const char *)&header,
            sz_rfbFramebufferUpdateRectHeader
        ) > 0 &&
        rfbWriteExact(
            cl,
