{
    adaptive_rfb_print_summary_legacy(state);

    CopySidecar *sidecar = copy_sidecar_find(state);
    if (!sidecar)
        return;

    fprintf(
        stderr,
        "[COPY][SUMMARY] updates=%llu copy-pixels=%llu "
        "residual-pixels=%llu residual-jpeg-bytes=%llu\n",
        (unsigned long long)sidecar->copy_updates,
        (unsigned long long)sidecar->copy_pixels,
        (unsigned long long)sidecar->residual_pixels,
        (unsigned long long)sidecar->residual_jpeg_bytes
    );
}
