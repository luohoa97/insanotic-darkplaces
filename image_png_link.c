/*
	The libpng entry points image_png.c uses, for builds that link libpng
	instead of loading it (LINK_TO_LIBPNG: the WebAssembly build). Kept apart
	because it includes libpng's own header, which image_png.c must not (it
	declares the few things it needs itself).
*/

#ifdef __EMSCRIPTEN__

#include <png.h>

const struct { const char *name; void *func; } png_link_table[] =
{
	{"png_set_sig_bytes", (void *)png_set_sig_bytes},
	{"png_sig_cmp", (void *)png_sig_cmp},
	{"png_create_read_struct", (void *)png_create_read_struct},
	{"png_create_write_struct", (void *)png_create_write_struct},
	{"png_create_info_struct", (void *)png_create_info_struct},
	{"png_read_info", (void *)png_read_info},
	{"png_set_compression_level", (void *)png_set_compression_level},
	{"png_set_filter", (void *)png_set_filter},
	{"png_set_expand", (void *)png_set_expand},
	{"png_set_palette_to_rgb", (void *)png_set_palette_to_rgb},
	{"png_set_tRNS_to_alpha", (void *)png_set_tRNS_to_alpha},
	{"png_set_gray_to_rgb", (void *)png_set_gray_to_rgb},
	{"png_set_filler", (void *)png_set_filler},
	{"png_set_IHDR", (void *)png_set_IHDR},
	{"png_set_packing", (void *)png_set_packing},
	{"png_set_bgr", (void *)png_set_bgr},
	{"png_set_interlace_handling", (void *)png_set_interlace_handling},
	{"png_read_update_info", (void *)png_read_update_info},
	{"png_read_image", (void *)png_read_image},
	{"png_read_end", (void *)png_read_end},
	{"png_destroy_read_struct", (void *)png_destroy_read_struct},
	{"png_destroy_write_struct", (void *)png_destroy_write_struct},
	{"png_set_read_fn", (void *)png_set_read_fn},
	{"png_set_write_fn", (void *)png_set_write_fn},
	{"png_get_valid", (void *)png_get_valid},
	{"png_get_rowbytes", (void *)png_get_rowbytes},
	{"png_get_channels", (void *)png_get_channels},
	{"png_get_bit_depth", (void *)png_get_bit_depth},
	{"png_get_IHDR", (void *)png_get_IHDR},
	{"png_access_version_number", (void *)png_access_version_number},
	{"png_write_info", (void *)png_write_info},
	{"png_write_row", (void *)png_write_row},
	{"png_write_end", (void *)png_write_end},
	{"png_set_longjmp_fn", (void *)png_set_longjmp_fn},
	{NULL, NULL}
};

#endif
