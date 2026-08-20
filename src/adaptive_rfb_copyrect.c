/* Experimental CopyRect transport wrapper for the 0.0.25 test branch. */
#define adaptive_rfb_create adaptive_rfb_create_legacy
#define adaptive_rfb_destroy adaptive_rfb_destroy_legacy
#define adaptive_rfb_try_send_pending adaptive_rfb_try_send_pending_legacy
#define adaptive_rfb_print_summary adaptive_rfb_print_summary_legacy
#include "adaptive_rfb.c"
#undef adaptive_rfb_create
#undef adaptive_rfb_destroy
#undef adaptive_rfb_try_send_pending
#undef adaptive_rfb_print_summary

#include "adaptive_rfb_copyrect_00.inc"
#include "adaptive_rfb_copyrect_01.inc"
#include "adaptive_rfb_copyrect_02.inc"
#include "adaptive_rfb_copyrect_03.inc"
#include "adaptive_rfb_copyrect_04.inc"
#include "adaptive_rfb_copyrect_05.inc"
#include "adaptive_rfb_copyrect_06.inc"
