#include "stdafx.h"
#include "text.h"
#include "ft.h"
#include "font_man.h"
#include "font_link.h"
#include FT_INTERNAL_OBJECTS_H	// for FT_PAD_CEIL
#include <cmath>
#include <vector>
using namespace std;

const FT_Glyph_Class *glyph_clazz = NULL;
const double pi = 4 * atan(1.0);

// convert 16.16 fixed float type to 26.6 format
inline FT_F26Dot6 to_26dot6(const FIXED &fixed)
{
	return *((FT_F26Dot6*) &fixed) >> 10;
}

inline FT_F26Dot6 to_26dot6(int i)
{
	return i << 6;
}

// convert integer to 16.16 fixed float type
inline FT_Pos to_16dot16(int i)
{
	return i << 16;
}

// convert 16.16 fixed float type to integer
inline int from_16dot16(FT_Pos fixed)
{
	return fixed >> 16;
}

// for given bitmap width and bit count, compute the bitmap pitch
inline int get_pitch(int width, WORD bpp)
{
	return FT_PAD_CEIL((int) ceil((double)(width * bpp) / 8), sizeof(LONG));
}

_gdimm_text::_gdimm_text()
{
	_metric_buf = NULL;
}

_gdimm_text::~_gdimm_text()
{
	if (_metric_buf != NULL)
		delete[] _metric_buf;
}

int _gdimm_text::get_ft_bmp_width(const FT_Bitmap &bitmap)
{
	switch (bitmap.pixel_mode)
	{
	case FT_PIXEL_MODE_LCD:
		return bitmap.width / 3;
	default:
		return bitmap.width;
	}
}

// for given DC bitmap bit count, return the corresponding FT_Glyph_To_Bitmap render mode
FT_Render_Mode _gdimm_text::get_render_mode(WORD dc_bpp) const
{
	// non-antialiased font
	// draw with monochrome mode
	if (_font_attr.lfQuality == NONANTIALIASED_QUALITY)
		return FT_RENDER_MODE_MONO;

	switch (dc_bpp)
	{
	case 1:
		return FT_RENDER_MODE_MONO;
	case 8:
		return FT_RENDER_MODE_NORMAL;
	case 24:
	case 32:
		return FT_RENDER_MODE_LCD;
	default:
		return FT_RENDER_MODE_NORMAL;
	}
}

void _gdimm_text::draw_background(HDC hdc, const RECT *bg_rect, COLORREF bg_color)
{
	assert(bg_color != CLR_INVALID);

	int i_ret;

	const HBRUSH bg_brush = CreateSolidBrush(bg_color);
	assert(bg_brush != NULL);

	i_ret = FillRect(hdc, bg_rect, bg_brush);
	assert(i_ret != 0);

	DeleteObject(bg_brush);
}

int _gdimm_text::get_dc_bpp() const
{
	int i_ret;

	const HBITMAP dc_bitmap = (HBITMAP) GetCurrentObject(_hdc_text, OBJ_BITMAP);
	assert(dc_bitmap != NULL);

	BITMAPINFO bmi = {0};
	bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	i_ret = GetDIBits(_hdc_text, dc_bitmap, 0, 0, NULL, &bmi, DIB_RGB_COLORS);
	assert(i_ret != 0);

	return bmi.bmiHeader.biBitCount;
}

// get various metrics of the DC
bool _gdimm_text::get_dc_metrics()
{
	// get outline metrics of the DC, which also include the text metrics
	int metric_size = GetOutlineTextMetrics(_hdc_text, 0, NULL);
	if (metric_size == 0)
		return false;

	if (_metric_buf != NULL)
		delete[] _metric_buf;

	_metric_buf = new BYTE[metric_size];
	_outline_metrics = (OUTLINETEXTMETRIC*) _metric_buf;
	metric_size = GetOutlineTextMetrics(_hdc_text, metric_size, _outline_metrics);
	assert(metric_size != 0);

	// get LOGFONT 
	HFONT h_font = (HFONT) GetCurrentObject(_hdc_text, OBJ_FONT);
	GetObject(h_font, sizeof(LOGFONT), &_font_attr);

	return true;
}

void _gdimm_text::get_glyph_clazz()
{
	/*
	glyph clazz is a private field, cannot be constructed through FreeType API
	instead, we load the glyph of the default character from the current font
	use the clazz for all subsequent FT_OutlineGlyph
	*/

	const TCHAR *font_full_name = get_full_name();
	unsigned int font_index = gdimm_font_man::instance().lookup_index(_hdc_text, font_full_name);

	FT_Error ft_error;
	const FTC_FaceID face_id = (FTC_FaceID) font_index;

	FT_Face font_face;
	ft_error = FTC_Manager_LookupFace(ft_cache_man, face_id, &font_face);
	assert(ft_error == 0);

	FTC_ScalerRec cache_scale = {face_id, 0, 0, 1, 0, 0};
	FT_Size font_size;
	ft_error = FTC_Manager_LookupSize(ft_cache_man, &cache_scale, &font_size);
	assert(ft_error == 0);

	ft_error = FT_Load_Char(font_face, _outline_metrics->otmTextMetrics.tmDefaultChar, FT_LOAD_NO_BITMAP);
	assert(ft_error == 0);

	FT_Glyph useless;
	FT_Get_Glyph(font_face->glyph, &useless);
	glyph_clazz = useless->clazz;
	FT_Done_Glyph(useless);
}

void _gdimm_text::set_bmp_bits_mono(const FT_Bitmap &src_bitmap,
	int x_in_dest, int y_in_dest,
	BYTE *dest_bits,
	int dest_width, int dest_height,
	bool is_dest_up,
	WORD dest_bpp) const
{
	// the source bitmap is 1-bit, 8 pixels per byte, in most-significant order
	// the destination is an non antialiased bitmap, in ANY bit count
	// the source bitmap is not blended with the destination bitmap

	const LONG src_width = src_bitmap.width;
	const LONG src_height = src_bitmap.rows;
	const int src_pitch = abs(src_bitmap.pitch);
	const int dest_pitch = get_pitch(dest_width, dest_bpp);
	const bool use_zero_color = (*(DWORD*) &_fg_rgb == 0);

	for (int j = 0; j < src_height; j++)
	{
		for (int i = 0; i < src_width; i++)
		{
			const int src_ptr = j * src_pitch + i / 8;
			const BYTE src_bit_pos = 7 - i % 8;
			const bool is_bit_set = ((src_bitmap.buffer[src_ptr] & (1 << src_bit_pos)) != 0);

			if (is_bit_set)
			{
				int dest_ptr = (x_in_dest + i) * dest_bpp / 8;
				if (is_dest_up == (src_bitmap.pitch > 0))
					dest_ptr += max(dest_height - y_in_dest - j - 1, 0) * dest_pitch;
				else
					dest_ptr += min(y_in_dest + j, dest_height) * dest_pitch;

				if (dest_bpp == 1)
				{
					// source bit position might differ from destination bit position
					const BYTE dest_bit_pos = 7 - (x_in_dest + i) % 8;

					if (use_zero_color)
						dest_bits[dest_ptr] &= ~(1 << dest_bit_pos);
					else
						dest_bits[dest_ptr] |= (1 << dest_bit_pos);
				}
				else
					memcpy(dest_bits + dest_ptr, &_fg_rgb, dest_bpp / 8);
			}
		}
	}
}

void _gdimm_text::set_bmp_bits_gray(const FT_Bitmap &src_bitmap,
	int x_in_dest, int y_in_dest,
	BYTE *dest_bits,
	int dest_width, int dest_height,
	bool is_dest_up) const
{
	// both bitmaps are 8-bit, 1 byte per pixel
	// the source bitmap is blended with the existing destination bitmap

	const LONG src_width = src_bitmap.width;
	const LONG src_height = src_bitmap.rows;
	const int src_pitch = abs(src_bitmap.pitch);
	const int dest_pitch = get_pitch(dest_width, 8);

	for (int j = 0; j < src_height; j++)
	{
		for (int i = 0; i < src_width; i++)
		{
			const int src_ptr = j * src_pitch + i;
			int dest_ptr = x_in_dest + i;
			if (is_dest_up == (src_bitmap.pitch > 0))
				dest_ptr += max(dest_height - y_in_dest - j - 1, 0) * dest_pitch;
			else
				dest_ptr += min(y_in_dest + j, dest_height - 1) * dest_pitch;

			const BYTE gray_level = src_bitmap.buffer[src_ptr];
			dest_bits[dest_ptr] = (gray_level * gray_level + (255 - gray_level) * dest_bits[dest_ptr]) / 255;
		}
	}
}

void _gdimm_text::set_bmp_bits_lcd(const FT_Bitmap &src_bitmap,
	int x_in_dest, int y_in_dest,
	BYTE *dest_bits,
	int dest_width, int dest_height,
	bool is_dest_up,
	WORD dest_bpp) const
{
	// the source bitmap is 24-bit, 3 bytes per pixel, in order of R, G, B channels
	// the destination bitmaps is 24- or 32-bit, 3(4) bytes per pixel, in order of B, G, R, (A) channels
	// each row is aligned to DWORD

	assert(dest_bpp >= 8);

	const WORD src_byte_per_px = 3;
	const LONG src_width = src_bitmap.width / src_byte_per_px;
	const LONG src_height = src_bitmap.rows;
	const int src_pitch = abs(src_bitmap.pitch);
	const WORD dest_byte_per_px = dest_bpp / 8;
	const int dest_pitch = get_pitch(dest_width, dest_bpp);

	// rows about to be copied
	for (int j = 0; j < src_height; j++)
	{
		// width about to be copied
		for (int i = 0; i < src_width; i++)
		{
			// source byte, always treat as down flow
			const int src_ptr = j * src_pitch + i * src_byte_per_px;

			// destination byte, compute according to two flow directions
			int dest_ptr = (x_in_dest + i) * dest_byte_per_px;
			if (is_dest_up == (src_bitmap.pitch > 0))
				dest_ptr += max(dest_height - y_in_dest - j - 1, 0) * dest_pitch;
			else
				dest_ptr += min(y_in_dest + j, dest_height - 1) * dest_pitch;

			const BYTE bmp_r = src_bitmap.buffer[src_ptr];
			const BYTE bmp_g = src_bitmap.buffer[src_ptr+1];
			const BYTE bmp_b = src_bitmap.buffer[src_ptr+2];
			
			dest_bits[dest_ptr] = (bmp_b * _fg_rgb.rgbBlue + (255 - bmp_b) * dest_bits[dest_ptr]) / 255;
			dest_bits[dest_ptr+1] = (bmp_g * _fg_rgb.rgbGreen + (255 - bmp_g) * dest_bits[dest_ptr+1]) / 255;
			dest_bits[dest_ptr+2] = (bmp_r * _fg_rgb.rgbRed + (255 - bmp_r) * dest_bits[dest_ptr+2]) / 255;
			memset(dest_bits + dest_ptr + 3, 0, dest_byte_per_px - src_byte_per_px);
		}
	}
}


/*
1. set up metrics
1. create canvas DC and bitmap, capture the bitmap of the physical DC by calling BitBlt
3. draw the glyph bitmap over the canvas bitmap
4. copy the canvas bitmap back to DC, apply clipping if necessary
return true if successfully draw the bitmap, otherwise return false
*/
bool _gdimm_text::draw_bitmap(const FT_BitmapGlyph bmp_glyph,
	 POINT &src_origin,
	CONST RECT *lprect,
	int dc_bpp) const
{
	BOOL b_ret;

	// 1.

	// height of the text from GDI. the source bitmap's height should be close to it
	const int cell_height = _outline_metrics->otmTextMetrics.tmHeight;

	// actual width of the source bitmap
	const int bmp_width = get_ft_bmp_width(bmp_glyph->bitmap);
	
	// width available for the bitmap to offset or scale
	const int bmp_spare_width = _cursor.x - (src_origin.x + bmp_width);

	// movement of the bitmap in y direction when generating. 0 for non-rotated text
	const int bmp_y_offset = abs(_cursor.y - src_origin.y);

	// height of the bitmap that will be drawn
	const int bmp_height = max(bmp_glyph->bitmap.rows + bmp_y_offset, cell_height);

	// the part of actual ascent higher than the cell ascent
	const int bmp_extra_ascent = max(bmp_glyph->top - _outline_metrics->otmTextMetrics.tmAscent, 0);

	// ascent that will be applied when drawing the bitmap
	const int bmp_ascent = _outline_metrics->otmTextMetrics.tmAscent + bmp_extra_ascent;

	// position where the bitmap will be finally drawn if no clipping is needed
	POINT dest_origin = src_origin;
	dest_origin.x += bmp_glyph->left;
	dest_origin.y -= bmp_extra_ascent;

	switch ((TA_LEFT | TA_RIGHT | TA_CENTER) & _text_alignment)
	{
	case TA_LEFT:
		break;
	case TA_RIGHT:
		dest_origin.x -= bmp_width;
		break;
	case TA_CENTER:
		dest_origin.x -= bmp_width / 2;
		break;
	}
	switch ((TA_TOP | TA_BOTTOM | TA_BASELINE) & _text_alignment)
	{
	case TA_TOP:
		break;
	case TA_BOTTOM:
		dest_origin.y -= bmp_height;
		break;
	case TA_BASELINE:
		dest_origin.y -= bmp_ascent;
		break;
	}

	// 2.

	BITMAPINFO bmi = {0};
	bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biWidth = bmp_width;
	bmi.bmiHeader.biHeight = bmp_height;
	bmi.bmiHeader.biPlanes = GetDeviceCaps(_hdc_text, PLANES);
	bmi.bmiHeader.biBitCount = dc_bpp;
	bmi.bmiHeader.biCompression = BI_RGB;

	BYTE *dest_bits;
	const HBITMAP dest_bitmap = CreateDIBSection(_hdc_text, &bmi, DIB_RGB_COLORS, (VOID**) &dest_bits, NULL, 0);
	assert(dest_bitmap != NULL);

	const HDC hdc_canvas = CreateCompatibleDC(_hdc_text);
	assert(hdc_canvas != NULL);
	SelectObject(hdc_canvas, dest_bitmap);

	/*
	both ETO_OPAQUE and OPAQUE background mode need background filled
	for ETO_OPAQUE, direct FillRect to the physical DC
	for OPAQUE background mode, draw the background on canvas DC (it might be clipped eventually)
	*/

	if (_eto_options & ETO_OPAQUE)
		draw_background(_hdc_text, lprect, _bg_color);

	if (GetBkMode(_hdc_text) == OPAQUE)
	{
		RECT bk_rect = {0, 0, bmp_width, bmp_height};
		draw_background(hdc_canvas, &bk_rect, _bg_color);
	}
	else
	{
		b_ret = BitBlt(hdc_canvas, 0, 0, bmp_width, bmp_height, _hdc_text, dest_origin.x, dest_origin.y, SRCCOPY);

		// "If a rotation or shear transformation is in effect in the source device context, BitBlt returns an error"
		if (!b_ret)	
		{
			DeleteObject(dest_bitmap);
			DeleteDC(hdc_canvas);
			return false;
		}
	}

	// 3.

	/*
	Windows DIB and FreeType Bitmap have different ways to indicate bitmap direction
	biHeight > 0 means the Windows DIB is bottom-up
	biHeight < 0 means the Windows DIB is top-down
	pitch > 0 means the FreeType bitmap is down flow
	pitch > 0 means the FreeType bitmap is up flow
	*/
	const int x_in_dest = 0;
	const int y_in_dest = max(bmp_ascent - bmp_glyph->top, 0);

	switch (bmp_glyph->bitmap.pixel_mode)
	{
	case FT_PIXEL_MODE_MONO:
		set_bmp_bits_mono(bmp_glyph->bitmap,
			x_in_dest, y_in_dest,
			dest_bits,
			bmp_width, bmp_height,
			bmp_height > 0,
			dc_bpp);
		break;
	case FT_PIXEL_MODE_GRAY:
		set_bmp_bits_gray(bmp_glyph->bitmap,
			x_in_dest, y_in_dest,
			dest_bits,
			bmp_width, bmp_height,
			bmp_height > 0);
		break;
	case FT_PIXEL_MODE_LCD:
		set_bmp_bits_lcd(bmp_glyph->bitmap,
			x_in_dest, y_in_dest,
			dest_bits,
			bmp_width, bmp_height,
			bmp_height > 0,
			dc_bpp);
		break;
	}

	// 4.

	if (_eto_options & ETO_CLIPPED)
	{
		RECT dest_rect = {dest_origin.x, dest_origin.y, dest_origin.x + bmp_width, dest_origin.y + bmp_height};
		IntersectRect(&dest_rect, &dest_rect, lprect);
		const LONG dest_width = dest_rect.right - dest_rect.left;
		const LONG dest_height = dest_rect.bottom - dest_rect.top;
		const LONG dest_x = dest_rect.left - dest_origin.x;
		const LONG dest_y = dest_rect.top - dest_origin.y;

		b_ret = BitBlt(_hdc_text,
			dest_rect.left,
			dest_rect.top,
			dest_width,
			dest_height,
			hdc_canvas,
			dest_x,
			dest_y,
			SRCCOPY);
		assert(b_ret);
	}
	else
	{
		b_ret = BitBlt(_hdc_text, dest_origin.x, dest_origin.y, bmp_width, bmp_height, hdc_canvas, 0, 0, SRCCOPY);
		assert(b_ret);
	}

	DeleteObject(dest_bitmap);
	DeleteDC(hdc_canvas);
	return true;
}

const TCHAR *_gdimm_text::get_family_name() const
{
	return (const TCHAR*)(_metric_buf + (UINT) _outline_metrics->otmpFamilyName);
}

const TCHAR *_gdimm_text::get_full_name() const
{
	return (const TCHAR*)(_metric_buf + (UINT) _outline_metrics->otmpFullName);
}

bool _gdimm_text::init(HDC hdc, int x, int y, UINT options)
{
	_hdc_text = hdc;

	if (!get_dc_metrics())
		return false;
	
	// since we only deal with fonts with outlines, the glyph clazz must be ft_outline_glyph_class
	// therefore this function is called only once
	if (glyph_clazz == NULL)
		get_glyph_clazz();

	_eto_options = options;

	// get foreground color
	COLORREF fg_color = GetTextColor(_hdc_text);
	assert(fg_color != CLR_INVALID);
	_fg_rgb.rgbBlue = GetBValue(fg_color);
	_fg_rgb.rgbGreen = GetGValue(fg_color);
	_fg_rgb.rgbRed = GetRValue(fg_color);

	// notice that not all DC has background color
	_bg_color = GetBkColor(hdc);

	_char_extra = GetTextCharacterExtra(_hdc_text);
	assert(_char_extra != 0x8000000);
	
	_text_alignment = GetTextAlign(_hdc_text);
	assert(_text_alignment != GDI_ERROR);
	
	if (((TA_NOUPDATECP | TA_UPDATECP) & _text_alignment) == TA_UPDATECP)
	{
		POINT cp;
		GetCurrentPositionEx(_hdc_text, &cp);
		_cursor.x = cp.x;
		_cursor.y = cp.y;
		_update_cursor = true;
	}
	else
	{
		_cursor.x = x;
		_cursor.y = y;
		_update_cursor = false;
	}

	return true;
}

/*bool _gdimm_text::to_glyph_indices(const WCHAR *text, unsigned int count, WORD *glyph_indices)
{
	DWORD dw_ret;

	int fl_index = 0;
	while (true)
	{
		dw_ret = GetGlyphIndices(_hdc_text, text, count, glyph_indices, GGI_MARK_NONEXISTING_GLYPHS);
		assert(dw_ret != GDI_ERROR);

		if (glyph_indices[0] != 0xffff)
			break;

		if (!gdimm_font_link::instance().font_link(_hdc_text, get_family_name(), fl_index++))
			return false;
	}

	// redo DC font related procedures
	get_dc_metrics();

	gdimm_font_link::instance().restore_font(_hdc_text);
	return true;
}*/

/*
as its name
return true it processed the text 
return false if it cannot deal with the text (let original ExtTextOutW handles)x
*/
bool _gdimm_text::text_out(const WCHAR *string, unsigned int count, CONST RECT *lprect, CONST INT *lpDx)
{
	// for SetTextJustification
	if (string[0] == _outline_metrics->otmTextMetrics.tmBreakChar && count == 1)
		return false;

	if (_font_attr.lfEscapement % 3600 != 0)
		return false;

	BOOL b_ret;
	FT_Error ft_error;
	
	// is ETO_PDY is set, lpDx contains both x increment and y displacement
	const int dx_factor = ((_eto_options & ETO_PDY) ? 2 : 1);

	// identity matrix
	const MAT2 id_matrix = {{0, 1}, {0, 0}, {0, 0}, {0, 1}};
	UINT ggo_format = GGO_NATIVE | GGO_UNHINTED;
	if (_eto_options & ETO_GLYPH_INDEX)
		ggo_format |= GGO_GLYPH_INDEX;

	const int dc_bpp = get_dc_bpp();
	const FT_Render_Mode render_mode = get_render_mode(dc_bpp);

	// Windows renders monochrome bitmap better than FreeType
	if (render_mode == FT_RENDER_MODE_MONO)
		return false;

	POINT bmp_origin = _cursor;
	vector<FT_Vector> curve_points;
	vector<char> curve_tags;
	vector<short> contour_indices;
	POINT glyph_offset = {0};

	/*
	use GetGlyphOutline to get character outline,
	consider the whole run of text as a big outline
	each character's outline is a set of contours in the text outline
	apply offset on the text direction after each character
	convert the text outline to bitmap, and draw it to the DC
	*/

	for (unsigned int i = 0; i < count; i++)
	{
		// we do not care about non-printable characters
		if (!(_eto_options & ETO_GLYPH_INDEX) && !iswprint(string[i]))
			continue;

		GLYPHMETRICS glyph_metrics;

		// get glyph metrics
		DWORD outline_buf_len = GetGlyphOutline(_hdc_text, string[i], ggo_format, &glyph_metrics, 0, NULL, &id_matrix);
		assert(outline_buf_len != GDI_ERROR);

		// some character's glyph outline is empty (e.g. space), skip
		if (outline_buf_len > 0)
		{
			BYTE *outline_buf = new BYTE[outline_buf_len];
			outline_buf_len = GetGlyphOutline(_hdc_text, string[i], ggo_format, &glyph_metrics, outline_buf_len, outline_buf, &id_matrix);
			assert(outline_buf_len != GDI_ERROR);

			// parse outline coutours
			DWORD header_off = 0;
			do
			{
				const BYTE *header_ptr = outline_buf + header_off;
				const TTPOLYGONHEADER *header = (TTPOLYGONHEADER*) header_ptr;

				// FreeType uses 26.6 format, while Windows gives logical units
				const FT_Vector start_point = {to_26dot6(header->pfxStart.x) + glyph_offset.x, to_26dot6(header->pfxStart.y) + glyph_offset.y};

				DWORD curve_off = sizeof(TTPOLYGONHEADER);
				while (curve_off < header->cb)
				{
					// the starting point of each curve is the last point of the previous curve or the starting point of the contour
					if (curve_off == sizeof(TTPOLYGONHEADER))
						curve_points.push_back(start_point);
					else
						curve_points.push_back(curve_points[curve_points.size()-1]);
					// the first point is on the curve
					curve_tags.push_back(FT_CURVE_TAG_ON);

					const TTPOLYCURVE *curve = (TTPOLYCURVE*)(header_ptr + curve_off);
					char curr_tag;
					switch (curve->wType)
					{
						case TT_PRIM_LINE:
							curr_tag = FT_CURVE_TAG_ON;
							break;
						case TT_PRIM_QSPLINE:
							curr_tag = FT_CURVE_TAG_CONIC;
							break;
						case TT_PRIM_CSPLINE:
							curr_tag = FT_CURVE_TAG_CUBIC;
							break;
					}

					for (int j = 0; j < curve->cpfx; j++)
					{
						const FT_Vector curr_point = {to_26dot6(curve->apfx[j].x) + glyph_offset.x, to_26dot6(curve->apfx[j].y) + glyph_offset.y};
						curve_points.push_back(curr_point);
						curve_tags.push_back(curr_tag);
					}
					// the last point is on the curve
					curve_tags[curve_tags.size()-1] = FT_CURVE_TAG_ON;
					
					curve_off += sizeof(TTPOLYCURVE) + (curve->cpfx - 1) * sizeof(POINTFX);
				}

				contour_indices.push_back(curve_points.size() - 1);
				header_off += header->cb;
			} while (header_off < outline_buf_len);

			assert(curve_points.size() == curve_tags.size());
			delete[] outline_buf;
		}

		POINT glyph_advance = {0, glyph_metrics.gmCellIncY};
		const LONG char_advance = glyph_metrics.gmCellIncX + _char_extra;

		if (lpDx == NULL)
			glyph_advance.x = char_advance;
		else
		{
			glyph_advance.x = lpDx[i * dx_factor];

			// the last element in lpDx may be 0
			// in that case, we use the character advancement
			if (i == count - 1 && glyph_advance.x == 0)
				glyph_advance.x = char_advance;
		}

		glyph_offset.x += to_26dot6(glyph_advance.x);
		glyph_offset.y -= to_26dot6(glyph_advance.y);
		_cursor.x += glyph_advance.x;
		_cursor.y += glyph_advance.y;
	}

	if (!curve_points.empty())
	{
		assert(curve_points.size() <= FT_OUTLINE_POINTS_MAX);

		/*
		once in possess of FT_Outline, there are several way to get FT_Bitmap

		1. FT_Outline_Render: could pass a callback span function to directly draw scanlines to DC
		   unfortunately it only output 8-bit bitmap
		2. FT_Outline_Get_Bitmap: merely a wrapper of FT_Outline_Render
		3. FT_Glyph_To_Bitmap: first conglyph_indicesuct FT_OutlineGlyph from FT_Outline, then render glyph to get FT_Bitmap
		   when conglyph_indicesucting FreeType glyph, the private clazz field must be provided
		   support 24-bit bitmap

		we use method 3
		*/

		FT_OutlineGlyphRec outline_glyph = 
		{
			{
				ft_lib,
				glyph_clazz,
				FT_GLYPH_FORMAT_OUTLINE,
				0,
				0
			},
			{
				contour_indices.size(),
				curve_points.size(),
				&curve_points[0],
				&curve_tags[0],
				&contour_indices[0],
				FT_OUTLINE_NONE
			}
		};

		// convert outline to bitmap
		FT_Glyph generic_glyph = (FT_Glyph) &outline_glyph;
		ft_error = FT_Glyph_To_Bitmap(&generic_glyph, render_mode, NULL, FALSE);

		// the outline is too complex
		if  (ft_error != 0)
			return false;

		FT_BitmapGlyph bmp_glyph = (FT_BitmapGlyph) generic_glyph;

		bool draw_success = draw_bitmap(bmp_glyph, bmp_origin, lprect, dc_bpp);

		FT_Done_Glyph((FT_Glyph) bmp_glyph);
		// the FT_OutlineGlyph is manually constructed, no need to destroy it

		if  (!draw_success)
			return false;
	}
	
	// if TA_UPDATECP is set, update current position after text out
	if (_update_cursor)
	{
		b_ret = MoveToEx(_hdc_text, _cursor.x, _cursor.y, NULL);
		assert(b_ret);
	}
	
	return true;
}