/* The desktop core links FFmpeg, libusb and libhackrf for features this first Android release does not have (video and audio decoding of
 * broadcast streams, USB radios). These stand-ins keep the library loadable: every call fails cleanly (NULL / -1). Phase 2 (see
 * PLAN-android.md) replaces the libusb ones with the real library; the FFmpeg ones stay until a decoder is built for Android.
 * Generated from the linker's list of undefined symbols; do not edit by hand. */
#include <stddef.h>
#include <stdint.h>
#define KEEP __attribute__((visibility("default"), used))
KEEP long av_bsf_free(void) { return -1; }
KEEP long av_bsf_init(void) { return -1; }
KEEP long av_bsf_list_parse_str(void) { return -1; }
KEEP long av_bsf_receive_packet(void) { return -1; }
KEEP long av_bsf_send_packet(void) { return -1; }
KEEP long av_buffer_unref(void) { return -1; }
KEEP long av_channel_layout_copy(void) { return -1; }
KEEP long av_channel_layout_default(void) { return -1; }
KEEP long av_channel_layout_uninit(void) { return -1; }
KEEP long av_dict_free(void) { return -1; }
KEEP void* av_dict_get(void) { return NULL; }
KEEP void* av_dict_iterate(void) { return NULL; }
KEEP long av_dict_set(void) { return -1; }
KEEP long av_find_best_stream(void) { return -1; }
KEEP void* av_find_input_format(void) { return NULL; }
KEEP void* av_frame_alloc(void) { return NULL; }
KEEP long av_frame_free(void) { return -1; }
KEEP long av_frame_get_buffer(void) { return -1; }
KEEP long av_frame_make_writable(void) { return -1; }
KEEP long av_frame_unref(void) { return -1; }
KEEP long av_free(void) { return -1; }
KEEP long av_freep(void) { return -1; }
KEEP int64_t av_guess_sample_aspect_ratio(void) { return 0; }
KEEP long av_interleaved_write_frame(void) { return -1; }
KEEP int64_t av_log_get_level(void) { return 0; }
KEEP long av_log_set_level(void) { return -1; }
KEEP void* av_malloc(void) { return NULL; }
KEEP int av_opt_set_int(void) { return -1; }
KEEP void* av_mallocz(void) { return NULL; }
KEEP long av_new_packet(void) { return -1; }
KEEP void* av_new_program(void) { return NULL; }
KEEP void* av_packet_alloc(void) { return NULL; }
KEEP void* av_packet_clone(void) { return NULL; }
KEEP long av_packet_free(void) { return -1; }
KEEP long av_packet_ref(void) { return -1; }
KEEP long av_packet_rescale_ts(void) { return -1; }
KEEP long av_packet_unref(void) { return -1; }
KEEP long av_program_add_stream_index(void) { return -1; }
KEEP long av_read_frame(void) { return -1; }
KEEP int64_t av_rescale(void) { return 0; }
KEEP int64_t av_rescale_q(void) { return 0; }
KEEP long av_seek_frame(void) { return -1; }
KEEP long av_strerror(void) { return -1; }
KEEP long av_write_frame(void) { return -1; }
KEEP long av_write_trailer(void) { return -1; }
KEEP void* avcodec_alloc_context3(void) { return NULL; }
KEEP long avcodec_decode_subtitle2(void) { return -1; }
KEEP void* avcodec_find_decoder(void) { return NULL; }
KEEP void* avcodec_find_encoder(void) { return NULL; }
KEEP void* avcodec_find_encoder_by_name(void) { return NULL; }
KEEP long avcodec_flush_buffers(void) { return -1; }
KEEP long avcodec_free_context(void) { return -1; }
KEEP const char* avcodec_get_name(void) { return "unavailable"; }
KEEP long avcodec_open2(void) { return -1; }
KEEP void* avcodec_parameters_alloc(void) { return NULL; }
KEEP long avcodec_parameters_copy(void) { return -1; }
KEEP long avcodec_parameters_free(void) { return -1; }
KEEP long avcodec_parameters_from_context(void) { return -1; }
KEEP long avcodec_parameters_to_context(void) { return -1; }
KEEP long avcodec_receive_frame(void) { return -1; }
KEEP long avcodec_receive_packet(void) { return -1; }
KEEP long avcodec_send_frame(void) { return -1; }
KEEP long avcodec_send_packet(void) { return -1; }
KEEP void* avformat_alloc_context(void) { return NULL; }
KEEP long avformat_alloc_output_context2(void) { return -1; }
KEEP long avformat_close_input(void) { return -1; }
KEEP long avformat_find_stream_info(void) { return -1; }
KEEP long av_guess_frame_rate(void) { return 0; }
KEEP long avformat_free_context(void) { return -1; }
KEEP long avformat_network_init(void) { return -1; }
KEEP void* avformat_new_stream(void) { return NULL; }
KEEP long avformat_open_input(void) { return -1; }
KEEP long avformat_write_header(void) { return -1; }
KEEP void* avio_alloc_context(void) { return NULL; }
KEEP long avio_close_dyn_buf(void) { return -1; }
KEEP long avio_closep(void) { return -1; }
KEEP long avio_context_free(void) { return -1; }
KEEP void* avio_enum_protocols(void) { return NULL; }
KEEP const char* avformat_configuration(void) { return ""; }
KEEP long avio_flush(void) { return -1; }
KEEP long avio_open(void) { return -1; }
KEEP long avio_open_dyn_buf(void) { return -1; }
KEEP long avsubtitle_free(void) { return -1; }
KEEP long hackrf_board_id_read(void) { return -1; }
KEEP long hackrf_close(void) { return -1; }
KEEP const char* hackrf_board_id_name(void) { return "unavailable"; }
KEEP void* hackrf_device_list(void) { return 0; }
KEEP void hackrf_device_list_free(void) { }
KEEP const char* hackrf_error_name(void) { return "unavailable"; }
KEEP const char* hackrf_usb_board_id_name(void) { return "unavailable"; }
KEEP long hackrf_exit(void) { return -1; }
KEEP long hackrf_init(void) { return -1; }
KEEP long hackrf_init_sweep(void) { return -1; }
KEEP long hackrf_open(void) { return -1; }
KEEP long hackrf_open_by_serial(void) { return -1; }
KEEP long hackrf_set_amp_enable(void) { return -1; }
KEEP long hackrf_set_antenna_enable(void) { return -1; }
KEEP long hackrf_set_baseband_filter_bandwidth(void) { return -1; }
KEEP long hackrf_set_freq(void) { return -1; }
KEEP long hackrf_set_lna_gain(void) { return -1; }
KEEP long hackrf_set_sample_rate(void) { return -1; }
KEEP long hackrf_set_vga_gain(void) { return -1; }
KEEP long hackrf_start_rx(void) { return -1; }
KEEP long hackrf_start_rx_sweep(void) { return -1; }
KEEP long hackrf_stop_rx(void) { return -1; }
#ifndef ONAIR_REAL_LIBUSB
/* libusb: replaced by the real library when native/CMakeLists.txt is given LIBUSB_SRC (android/docs/USB.md). Without it a HackRF is listed
 * from the app's descriptor but cannot be opened (libusb_wrap_sys_device answers LIBUSB_ERROR_NOT_SUPPORTED). The two entry points the
 * descriptor path uses (libusb_set_option, libusb_wrap_sys_device) were added by hand. */
KEEP long libusb_set_option(void) { return -1; }
KEEP long libusb_set_interface_alt_setting(void) { return -1; }   /* the Mirics driver (mirics_usb.cpp) */
KEEP long libusb_wrap_sys_device(void) { return -12; }   /* LIBUSB_ERROR_NOT_SUPPORTED */
KEEP void* libusb_alloc_transfer(void) { return NULL; }
KEEP long libusb_cancel_transfer(void) { return -1; }
KEEP long libusb_claim_interface(void) { return -1; }
KEEP long libusb_close(void) { return -1; }
KEEP long libusb_control_transfer(void) { return -1; }
KEEP long libusb_detach_kernel_driver(void) { return -1; }
KEEP const char* libusb_error_name(void) { return "unavailable"; }
KEEP long libusb_exit(void) { return -1; }
KEEP long libusb_free_config_descriptor(void) { return -1; }
KEEP long libusb_free_device_list(void) { return -1; }
KEEP long libusb_free_transfer(void) { return -1; }
KEEP long libusb_get_active_config_descriptor(void) { return -1; }
KEEP long libusb_get_bus_number(void) { return -1; }
KEEP long libusb_get_device_address(void) { return -1; }
KEEP long libusb_get_device_descriptor(void) { return -1; }
KEEP long libusb_get_device_list(void) { return -1; }
KEEP long libusb_get_device_speed(void) { return -1; }
KEEP long libusb_get_string_descriptor_ascii(void) { return -1; }
KEEP long libusb_handle_events_timeout_completed(void) { return -1; }
KEEP long libusb_init(void) { return -1; }
KEEP long libusb_interrupt_event_handler(void) { return -1; }
KEEP long libusb_kernel_driver_active(void) { return -1; }
KEEP long libusb_open(void) { return -1; }
KEEP long libusb_release_interface(void) { return -1; }
KEEP long libusb_submit_transfer(void) { return -1; }
#endif
KEEP long swr_alloc_set_opts2(void) { return -1; }
KEEP long swr_convert(void) { return -1; }
KEEP long swr_free(void) { return -1; }
KEEP long swr_get_out_samples(void) { return -1; }
KEEP long swr_init(void) { return -1; }
KEEP long sws_freeContext(void) { return -1; }
KEEP void* sws_getCachedContext(void) { return NULL; }
KEEP void* sws_getCoefficients(void) { return NULL; }
KEEP void* sws_getContext(void) { return NULL; }
KEEP long sws_scale(void) { return -1; }
KEEP long sws_setColorspaceDetails(void) { return -1; }
KEEP void* av_frame_get_side_data(void* frame, int type) { (void)frame; (void)type; return NULL; }
