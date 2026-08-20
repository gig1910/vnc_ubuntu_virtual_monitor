/*
 * Production adaptive transport translation unit.
 *
 * The core owns JPEG21, exactness tracking and progressive lossless repair.
 * The CopyRect extension wraps create/send/summary while reusing the core's
 * private helpers and state. Keeping both in one translation unit avoids
 * exporting internal RFB client synchronization details as public API.
 */
#define adaptive_rfb_create adaptive_rfb_create_legacy
#define adaptive_rfb_destroy adaptive_rfb_destroy_legacy
#define adaptive_rfb_try_send_pending adaptive_rfb_try_send_pending_legacy
#define adaptive_rfb_print_summary adaptive_rfb_print_summary_legacy
#include "adaptive_rfb.c"
#undef adaptive_rfb_create
#undef adaptive_rfb_destroy
#undef adaptive_rfb_try_send_pending
#undef adaptive_rfb_print_summary

#include "adaptive_rfb_copyrect.inc"
