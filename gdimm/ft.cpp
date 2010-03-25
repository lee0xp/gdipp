#include "stdafx.h"
#include "ft.h"
#include "font_man.h"
#include "gdimm.h"

FT_Library ft_lib;
FTC_Manager ft_cache_man;
FTC_ImageCache ft_glyph_cache;

void initialize_freetype()
{
	FT_Error ft_error;

	FT_UInt cache_max_faces = 2;
	wcs_convert(setting_instance.get_gdimm_setting("freetype/cache_max_faces", L""), cache_max_faces);
	FT_UInt cache_max_sizes = 4;
	wcs_convert(setting_instance.get_gdimm_setting("freetype/cache_max_sizes", L""), cache_max_sizes);
	FT_ULong cache_max_bytes = 200000;
	wcs_convert(setting_instance.get_gdimm_setting("freetype/cache_max_bytes", L""), cache_max_bytes);
	FT_LcdFilter lcd_filter = FT_LCD_FILTER_DEFAULT;
	wcs_convert(setting_instance.get_gdimm_setting("freetype/lcd_filter", L""), *((int*) &lcd_filter));

	ft_error = FT_Init_FreeType(&ft_lib);
	assert(ft_error == 0);

	ft_error = FT_Library_SetLcdFilter(ft_lib, lcd_filter);
	assert(ft_error == 0);

	ft_error = FTC_Manager_New(ft_lib, cache_max_faces, cache_max_sizes, cache_max_bytes, face_requester, NULL, &ft_cache_man);
	assert(ft_error == 0);

	ft_error = FTC_ImageCache_New(ft_cache_man, &ft_glyph_cache);
	assert(ft_error == 0);
}

void destroy_freetype()
{
	FT_Error ft_error;

	FTC_Manager_Done(ft_cache_man);
	
	ft_error = FT_Done_FreeType(ft_lib);
	assert(ft_error == 0);
}

FT_Error face_requester(FTC_FaceID face_id, FT_Library library, FT_Pointer request_data, FT_Face *aface)
{
	FT_Open_Args args;
	args.flags = FT_OPEN_STREAM;
	args.stream = font_man_instance.get_font_stream((long) face_id);
	
	return FT_Open_Face(library, &args, 0, aface);
}