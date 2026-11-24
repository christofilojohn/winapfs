// winapfs - APFS for Windows
// Copyright (C) 2026 winapfs contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Dashboard.h"
#include "resource.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace winapfs {
namespace {

constexpr wchar_t kClassName[] = L"winapfs-dashboard";
constexpr DWORD kDwmUseImmersiveDarkMode = 20;
constexpr DWORD kDwmWindowCornerPreference = 33;
constexpr DWORD kDwmWcpRound = 2;

enum class BtnKind
{
	ToggleAllowWrites,
	Rescan,
	StartService,
	InstallService,
	About,
	CheckUpdates,
	InstallUpdate,
	OpenReleasePage,
	VolOpen,
	VolMount,
	VolUnmount,
	VolUnlock,
	VolEject,
	VolToggleRw,
	VolToggleAuto,
	VolToggleDotfiles,
	VolChangeLetter,
	VolVerify,
};

enum class BtnTone
{
	Neutral,
	Primary,
	Purple,
	Amber,
	ActiveGreen,
};

struct HitButton
{
	Gdiplus::RectF rect;
	Gdiplus::RectF hit_rect;
	BtnKind kind;
	int vol_index = -1;
	std::string vol_uuid;
	BtnTone tone = BtnTone::Neutral;
	std::wstring text;
	bool enabled = true;
};

struct Palette
{
	bool dark = false;
	Gdiplus::Color bg;
	Gdiplus::Color header_bg;
	Gdiplus::Color footer_bg;
	Gdiplus::Color card_bg;
	Gdiplus::Color card_hover;
	Gdiplus::Color card_border;
	Gdiplus::Color divider;
	Gdiplus::Color text_primary;
	Gdiplus::Color text_secondary;
	Gdiplus::Color text_muted;
	Gdiplus::Color btn_bg;
	Gdiplus::Color btn_hover;
	Gdiplus::Color btn_press;
	Gdiplus::Color btn_border;
	Gdiplus::Color accent;
	Gdiplus::Color accent_hover;
	Gdiplus::Color accent_press;
	Gdiplus::Color accent_text;
	Gdiplus::Color purple;
	Gdiplus::Color purple_hover;
	Gdiplus::Color purple_press;
	Gdiplus::Color purple_pill_bg;
	Gdiplus::Color purple_pill_fg;
	Gdiplus::Color green_pill_bg;
	Gdiplus::Color green_pill_fg;
	Gdiplus::Color amber_pill_bg;
	Gdiplus::Color amber_pill_fg;
	Gdiplus::Color red_pill_bg;
	Gdiplus::Color red_pill_fg;
	Gdiplus::Color blue_pill_bg;
	Gdiplus::Color blue_pill_fg;
	Gdiplus::Color track_bg;
	Gdiplus::Color scroll_thumb;
};

HINSTANCE g_inst = nullptr;
HWND g_hwnd = nullptr;
HWND g_tray_wnd = nullptr;
DashboardCallbacks g_cbs;
DashboardState g_state;
UINT g_dpi = 96;
bool g_dark = false;
int g_scroll_y = 0;
int g_max_scroll = 0;
int g_hover_btn = -1;
bool g_has_press = false;
BtnKind g_press_kind = BtnKind::About;
std::string g_press_uuid;
int g_hover_card = -1;
bool g_tracking_mouse = false;
std::vector<HitButton> g_buttons;
std::vector<Gdiplus::RectF> g_card_rects;

std::wstring ToWide(const std::string &s)
{
	if (s.empty())
		return L"";
	int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
	std::wstring w(static_cast<size_t>(n), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
	return w;
}

std::wstring FormatBytes(uint64_t bytes)
{
	static const wchar_t *kUnits[] = { L"B", L"KB", L"MB", L"GB", L"TB" };
	double val = static_cast<double>(bytes);
	size_t idx = 0;
	while (val >= 1024.0 && idx + 1 < _countof(kUnits))
	{
		val /= 1024.0;
		idx++;
	}
	wchar_t buf[64];
	if (idx == 0)
		_snwprintf_s(buf, _TRUNCATE, L"%llu %s", static_cast<unsigned long long>(bytes), kUnits[idx]);
	else
		_snwprintf_s(buf, _TRUNCATE, val >= 100.0 ? L"%.0f %s" : L"%.1f %s", val, kUnits[idx]);
	return buf;
}

std::wstring FormatSpeed(uint64_t bps)
{
	return FormatBytes(bps) + L"/s";
}

bool IsSystemDarkMode()
{
	DWORD light = 1;
	DWORD sz = sizeof(light);
	if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
		L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &sz) == ERROR_SUCCESS)
	{
		return light == 0;
	}
	return false;
}

Palette MakePalette(bool dark)
{
	Palette p;
	p.dark = dark;
	if (dark)
	{
		p.bg = Gdiplus::Color(255, 24, 26, 32);
		p.header_bg = Gdiplus::Color(255, 31, 34, 42);
		p.footer_bg = Gdiplus::Color(255, 28, 31, 38);
		p.card_bg = Gdiplus::Color(255, 37, 41, 51);
		p.card_hover = Gdiplus::Color(255, 43, 48, 60);
		p.card_border = Gdiplus::Color(255, 56, 62, 77);
		p.divider = Gdiplus::Color(255, 47, 52, 64);
		p.text_primary = Gdiplus::Color(255, 242, 245, 250);
		p.text_secondary = Gdiplus::Color(255, 168, 177, 192);
		p.text_muted = Gdiplus::Color(255, 120, 130, 148);
		p.btn_bg = Gdiplus::Color(255, 49, 55, 68);
		p.btn_hover = Gdiplus::Color(255, 60, 67, 82);
		p.btn_press = Gdiplus::Color(255, 41, 46, 57);
		p.btn_border = Gdiplus::Color(255, 69, 77, 94);
		p.accent = Gdiplus::Color(255, 76, 194, 255);
		p.accent_hover = Gdiplus::Color(255, 98, 205, 255);
		p.accent_press = Gdiplus::Color(255, 58, 174, 236);
		p.accent_text = Gdiplus::Color(255, 12, 20, 30);
		p.purple = Gdiplus::Color(255, 155, 114, 255);
		p.purple_hover = Gdiplus::Color(255, 170, 134, 255);
		p.purple_press = Gdiplus::Color(255, 135, 94, 238);
		p.purple_pill_bg = Gdiplus::Color(44, 176, 136, 255);
		p.purple_pill_fg = Gdiplus::Color(255, 214, 192, 255);
		p.green_pill_bg = Gdiplus::Color(40, 74, 222, 128);
		p.green_pill_fg = Gdiplus::Color(255, 134, 239, 172);
		p.amber_pill_bg = Gdiplus::Color(42, 251, 191, 36);
		p.amber_pill_fg = Gdiplus::Color(255, 253, 214, 120);
		p.red_pill_bg = Gdiplus::Color(42, 248, 113, 113);
		p.red_pill_fg = Gdiplus::Color(255, 252, 165, 165);
		p.blue_pill_bg = Gdiplus::Color(42, 96, 165, 250);
		p.blue_pill_fg = Gdiplus::Color(255, 147, 197, 253);
		p.track_bg = Gdiplus::Color(255, 55, 62, 76);
		p.scroll_thumb = Gdiplus::Color(140, 160, 170, 190);
	}
	else
	{
		p.bg = Gdiplus::Color(255, 243, 245, 249);
		p.header_bg = Gdiplus::Color(255, 255, 255, 255);
		p.footer_bg = Gdiplus::Color(255, 249, 250, 253);
		p.card_bg = Gdiplus::Color(255, 255, 255, 255);
		p.card_hover = Gdiplus::Color(255, 249, 251, 255);
		p.card_border = Gdiplus::Color(255, 223, 228, 236);
		p.divider = Gdiplus::Color(255, 229, 233, 240);
		p.text_primary = Gdiplus::Color(255, 24, 28, 36);
		p.text_secondary = Gdiplus::Color(255, 84, 94, 112);
		p.text_muted = Gdiplus::Color(255, 124, 134, 152);
		p.btn_bg = Gdiplus::Color(255, 239, 242, 247);
		p.btn_hover = Gdiplus::Color(255, 228, 233, 241);
		p.btn_press = Gdiplus::Color(255, 215, 222, 232);
		p.btn_border = Gdiplus::Color(255, 211, 217, 228);
		p.accent = Gdiplus::Color(255, 0, 103, 192);
		p.accent_hover = Gdiplus::Color(255, 0, 88, 168);
		p.accent_press = Gdiplus::Color(255, 0, 74, 144);
		p.accent_text = Gdiplus::Color(255, 255, 255, 255);
		p.purple = Gdiplus::Color(255, 110, 64, 201);
		p.purple_hover = Gdiplus::Color(255, 96, 52, 184);
		p.purple_press = Gdiplus::Color(255, 82, 42, 164);
		p.purple_pill_bg = Gdiplus::Color(28, 110, 64, 201);
		p.purple_pill_fg = Gdiplus::Color(255, 91, 46, 145);
		p.green_pill_bg = Gdiplus::Color(26, 22, 163, 74);
		p.green_pill_fg = Gdiplus::Color(255, 21, 128, 61);
		p.amber_pill_bg = Gdiplus::Color(30, 217, 119, 6);
		p.amber_pill_fg = Gdiplus::Color(255, 161, 68, 11);
		p.red_pill_bg = Gdiplus::Color(26, 220, 38, 38);
		p.red_pill_fg = Gdiplus::Color(255, 185, 28, 28);
		p.blue_pill_bg = Gdiplus::Color(26, 37, 99, 235);
		p.blue_pill_fg = Gdiplus::Color(255, 29, 78, 216);
		p.track_bg = Gdiplus::Color(255, 227, 232, 240);
		p.scroll_thumb = Gdiplus::Color(130, 130, 140, 158);
	}
	return p;
}

void ApplyDwmAttributes(HWND hwnd)
{
	BOOL dark = g_dark ? TRUE : FALSE;
	DwmSetWindowAttribute(hwnd, kDwmUseImmersiveDarkMode, &dark, sizeof(dark));
	DWORD corner = kDwmWcpRound;
	DwmSetWindowAttribute(hwnd, kDwmWindowCornerPreference, &corner, sizeof(corner));
}

void AddRoundedRectPath(Gdiplus::GraphicsPath &path, const Gdiplus::RectF &r, float radius)
{
	float d = (std::min)(radius * 2.0f, (std::min)(r.Width, r.Height));
	if (d <= 0.5f)
	{
		path.AddRectangle(r);
		return;
	}
	path.AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
	path.AddArc(r.X + r.Width - d, r.Y, d, d, 270.0f, 90.0f);
	path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0.0f, 90.0f);
	path.AddArc(r.X, r.Y + r.Height - d, d, d, 90.0f, 90.0f);
	path.CloseFigure();
}

void FillRoundRect(Gdiplus::Graphics &g, const Gdiplus::RectF &r, float radius, const Gdiplus::Color &fill)
{
	Gdiplus::GraphicsPath path;
	AddRoundedRectPath(path, r, radius);
	Gdiplus::SolidBrush br(fill);
	g.FillPath(&br, &path);
}

void StrokeRoundRect(Gdiplus::Graphics &g, const Gdiplus::RectF &r, float radius, const Gdiplus::Color &stroke, float width = 1.0f)
{
	Gdiplus::GraphicsPath path;
	AddRoundedRectPath(path, r, radius);
	Gdiplus::Pen pen(stroke, width);
	g.DrawPath(&pen, &path);
}

float MeasureTextWidth(Gdiplus::Graphics &g, const std::wstring &text, const Gdiplus::Font &font)
{
	if (text.empty())
		return 0.0f;
	Gdiplus::RectF box;
	Gdiplus::StringFormat fmt(Gdiplus::StringFormat::GenericTypographic());
	fmt.SetFormatFlags(Gdiplus::StringFormatFlagsMeasureTrailingSpaces);
	g.MeasureString(text.c_str(), static_cast<INT>(text.size()), &font, Gdiplus::PointF(0.0f, 0.0f), &fmt, &box);
	return box.Width;
}

Gdiplus::RectF ClipRectY(const Gdiplus::RectF &r, float top, float bot)
{
	float y0 = (std::max)(r.Y, top);
	float y1 = (std::min)(r.Y + r.Height, bot);
	return y1 > y0 ? Gdiplus::RectF(r.X, y0, r.Width, y1 - y0) : Gdiplus::RectF(0.0f, 0.0f, 0.0f, 0.0f);
}

void DrawButton(Gdiplus::Graphics &g, const HitButton &b, int idx, float s, const Palette &pal, const Gdiplus::Font &font)
{
	bool hover = (idx == g_hover_btn) && b.enabled;
	bool press = g_has_press && b.enabled && b.kind == g_press_kind && b.vol_uuid == g_press_uuid;
	Gdiplus::Color bg = pal.btn_bg;
	Gdiplus::Color fg = pal.text_primary;
	Gdiplus::Color border = pal.btn_border;
	bool draw_border = true;

	if (b.tone == BtnTone::Primary)
	{
		bg = press ? pal.accent_press : hover ? pal.accent_hover : pal.accent;
		fg = pal.accent_text;
		draw_border = false;
	}
	else if (b.tone == BtnTone::Purple)
	{
		bg = press ? pal.purple_press : hover ? pal.purple_hover : pal.purple;
		fg = Gdiplus::Color(255, 255, 255, 255);
		draw_border = false;
	}
	else if (b.tone == BtnTone::Amber)
	{
		bg = press ? pal.btn_press : hover ? pal.btn_hover : pal.amber_pill_bg;
		fg = pal.amber_pill_fg;
		border = pal.amber_pill_fg;
	}
	else if (b.tone == BtnTone::ActiveGreen)
	{
		bg = press ? pal.btn_press : hover ? pal.btn_hover : pal.green_pill_bg;
		fg = pal.green_pill_fg;
		border = pal.green_pill_fg;
	}
	else
	{
		bg = press ? pal.btn_press : hover ? pal.btn_hover : pal.btn_bg;
	}

	if (!b.enabled)
	{
		fg = pal.text_muted;
		bg = pal.btn_bg;
	}

	float r = 6.0f * s;
	FillRoundRect(g, b.rect, r, bg);
	if (draw_border)
		StrokeRoundRect(g, b.rect, r, border, 1.0f);

	Gdiplus::SolidBrush text_br(fg);
	Gdiplus::StringFormat fmt;
	fmt.SetAlignment(Gdiplus::StringAlignmentCenter);
	fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
	fmt.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
	fmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
	g.DrawString(b.text.c_str(), static_cast<INT>(b.text.size()), &font, b.rect, &fmt, &text_br);
}

void DrawPill(Gdiplus::Graphics &g, float x, float y, float h, const std::wstring &text, const Gdiplus::Color &bg, const Gdiplus::Color &fg,
	const Gdiplus::Font &font, float pad_x, float &out_right)
{
	float w = MeasureTextWidth(g, text, font) + pad_x * 2.0f;
	Gdiplus::RectF r(x, y, w, h);
	FillRoundRect(g, r, h * 0.5f, bg);
	Gdiplus::SolidBrush br(fg);
	Gdiplus::StringFormat fmt;
	fmt.SetAlignment(Gdiplus::StringAlignmentCenter);
	fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
	fmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
	g.DrawString(text.c_str(), static_cast<INT>(text.size()), &font, r, &fmt, &br);
	out_right = x + w;
}

bool IsPhysicalDisk(const VolumeStatus &v)
{
	return v.disk.compare(0, 4, "\\\\.\\") == 0;
}

void PaintWindow(HWND hwnd)
{
	PAINTSTRUCT ps;
	HDC hdc = BeginPaint(hwnd, &ps);
	RECT rc;
	GetClientRect(hwnd, &rc);
	int cw = rc.right - rc.left;
	int ch = rc.bottom - rc.top;
	if (cw <= 0 || ch <= 0)
	{
		EndPaint(hwnd, &ps);
		return;
	}

	HDC mem_dc = CreateCompatibleDC(hdc);
	HBITMAP bmp = CreateCompatibleBitmap(hdc, cw, ch);
	HGDIOBJ old_bmp = SelectObject(mem_dc, bmp);

	{
		Gdiplus::Graphics g(mem_dc);
		g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
		g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
		g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);

		float s = static_cast<float>(g_dpi) / 96.0f;
		Palette pal = MakePalette(g_dark);
		g.Clear(pal.bg);

		Gdiplus::FontFamily segoe(L"Segoe UI");
		Gdiplus::FontFamily segoe_semi(L"Segoe UI Semibold");
		const Gdiplus::FontFamily &semi_fam = segoe_semi.IsAvailable() ? segoe_semi : segoe;
		Gdiplus::FontStyle semi_style = segoe_semi.IsAvailable() ? Gdiplus::FontStyleRegular : Gdiplus::FontStyleBold;
		Gdiplus::Font f_title(&segoe, 18.0f * s, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
		Gdiplus::Font f_card_title(&semi_fam, 15.0f * s, semi_style, Gdiplus::UnitPixel);
		Gdiplus::Font f_tile(&segoe, 18.0f * s, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
		Gdiplus::Font f_tile_sm(&segoe, 12.0f * s, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
		Gdiplus::Font f_body(&segoe, 12.5f * s, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
		Gdiplus::Font f_btn(&semi_fam, 12.0f * s, semi_style, Gdiplus::UnitPixel);
		Gdiplus::Font f_pill(&semi_fam, 11.0f * s, semi_style, Gdiplus::UnitPixel);
		Gdiplus::Font f_small(&segoe, 11.5f * s, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);

		float header_h = 68.0f * s;
		float footer_h = 34.0f * s;
		float content_top = header_h;
		float content_bot = static_cast<float>(ch) - footer_h;
		float view_h = (std::max)(1.0f, content_bot - content_top);

		g_buttons.clear();
		g_card_rects.clear();

		// ---- Scrollable volume area (drawn first, clipped between header and footer)
		Gdiplus::Region clip_region(Gdiplus::RectF(0.0f, content_top, static_cast<float>(cw), view_h));
		g.SetClip(&clip_region);

		float pad_x = 20.0f * s;
		float pad_y = 16.0f * s;
		float gap = 12.0f * s;
		float card_w = static_cast<float>(cw) - pad_x * 2.0f - (g_max_scroll > 0 ? 10.0f * s : 0.0f);
		float y = content_top + pad_y - static_cast<float>(g_scroll_y);
		float total_h = pad_y;

		if (g_state.update_available)
		{
			float bh_banner = 56.0f * s;
			Gdiplus::RectF banner(pad_x, y, card_w, bh_banner);
			if (banner.Y + banner.Height >= content_top && banner.Y <= content_bot)
			{
				FillRoundRect(g, banner, 10.0f * s, pal.card_bg);
				StrokeRoundRect(g, banner, 10.0f * s, pal.accent, 1.25f);

				float after_pill = 0.0f;
				DrawPill(g, banner.X + 14.0f * s, banner.Y + (bh_banner - 20.0f * s) * 0.5f, 20.0f * s, L"UPDATE",
					pal.blue_pill_bg, pal.blue_pill_fg, f_pill, 8.0f * s, after_pill);

				float brx = banner.GetRight() - 14.0f * s;
				float bby = banner.Y + (bh_banner - 28.0f * s) * 0.5f;
				float bbh = 28.0f * s;
				auto add_banner_btn = [&](BtnKind kind, const std::wstring &label, BtnTone tone, bool enabled) {
					float bw = MeasureTextWidth(g, label, f_btn) + 20.0f * s;
					brx -= bw;
					HitButton hb;
					hb.rect = Gdiplus::RectF(brx, bby, bw, bbh);
					hb.hit_rect = ClipRectY(hb.rect, content_top, content_bot);
					hb.kind = kind;
					hb.tone = tone;
					hb.text = label;
					hb.enabled = enabled;
					g_buttons.push_back(hb);
					DrawButton(g, hb, static_cast<int>(g_buttons.size()) - 1, s, pal, f_btn);
					brx -= 8.0f * s;
				};

				std::wstring dl_label = g_state.update_downloading ? L"Downloading\x2026" : L"Download & Install";
				add_banner_btn(BtnKind::InstallUpdate, dl_label, BtnTone::Primary, !g_state.update_downloading);
				add_banner_btn(BtnKind::OpenReleasePage, L"Release Notes", BtnTone::Neutral, true);

				std::wstring msg = L"winapfs v" + ToWide(g_state.update_version) + L" is available";
				Gdiplus::SolidBrush msg_br(pal.text_primary);
				Gdiplus::StringFormat lfmt;
				lfmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
				lfmt.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
				lfmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
				g.DrawString(msg.c_str(), -1, &f_card_title, Gdiplus::RectF(after_pill + 10.0f * s, banner.Y, (std::max)(60.0f * s, brx - after_pill - 12.0f * s), bh_banner), &lfmt, &msg_br);
			}
			y += bh_banner + gap;
			total_h += bh_banner + gap;
		}

		if (g_state.vols.empty())
		{
			g_max_scroll = 0;
			g_scroll_y = 0;
			float box_w = (std::min)(480.0f * s, static_cast<float>(cw) - 40.0f * s);
			float box_h = 164.0f * s;
			float empty_top = g_state.update_available ? y + 10.0f * s : content_top + (view_h - box_h) * 0.45f;
			Gdiplus::RectF box((static_cast<float>(cw) - box_w) * 0.5f, empty_top, box_w, box_h);
			FillRoundRect(g, box, 12.0f * s, pal.card_bg);
			StrokeRoundRect(g, box, 12.0f * s, pal.card_border, 1.0f);

			std::wstring title = g_state.service_up ? L"No APFS disks connected"
				: g_state.service_installed ? L"winapfs service is stopped" : L"winapfs service is not installed";
			std::wstring desc = g_state.service_up
				? L"Plug in a Mac-formatted APFS external SSD or disk. Click Rescan if it does not appear automatically."
				: L"The background service mounts APFS disks as drive letters automatically when connected.";

			Gdiplus::SolidBrush t_br(pal.text_primary);
			Gdiplus::SolidBrush d_br(pal.text_secondary);
			Gdiplus::StringFormat cfmt;
			cfmt.SetAlignment(Gdiplus::StringAlignmentCenter);
			g.DrawString(title.c_str(), -1, &f_card_title, Gdiplus::RectF(box.X + 20.0f * s, box.Y + 24.0f * s, box.Width - 40.0f * s, 26.0f * s), &cfmt, &t_br);
			g.DrawString(desc.c_str(), -1, &f_body, Gdiplus::RectF(box.X + 24.0f * s, box.Y + 54.0f * s, box.Width - 48.0f * s, 44.0f * s), &cfmt, &d_br);

			float bw = 148.0f * s;
			float bh = 32.0f * s;
			HitButton cta;
			cta.rect = Gdiplus::RectF(box.X + (box.Width - bw) * 0.5f, box.Y + 110.0f * s, bw, bh);
			cta.hit_rect = ClipRectY(cta.rect, content_top, content_bot);
			cta.tone = BtnTone::Primary;
			if (g_state.service_up)
			{
				cta.kind = BtnKind::Rescan;
				cta.text = L"Rescan Disks";
			}
			else if (g_state.service_installed)
			{
				cta.kind = BtnKind::StartService;
				cta.text = L"Start Service";
			}
			else
			{
				cta.kind = BtnKind::InstallService;
				cta.text = L"Install Service";
			}
			g_buttons.push_back(cta);
			DrawButton(g, cta, static_cast<int>(g_buttons.size()) - 1, s, pal, f_btn);
		}
		else
		{

			for (size_t i = 0; i < g_state.vols.size(); i++)
			{
				const VolumeCardData &v = g_state.vols[i];
				bool mounted = v.state == "mounted";
				bool locked = v.state == "locked";
				bool rw = v.mode == "rw";
				bool has_note = !v.note.empty();
				float card_h = 148.0f * s + (v.has_space ? 26.0f * s : 0.0f) + (has_note ? 20.0f * s : 0.0f);

				Gdiplus::RectF card(pad_x, y, card_w, card_h);
				g_card_rects.push_back(ClipRectY(card, content_top, content_bot));

				if (card.Y + card.Height >= content_top && card.Y <= content_bot)
				{
					bool card_hovered = (static_cast<int>(i) == g_hover_card);
					FillRoundRect(g, card, 10.0f * s, card_hovered ? pal.card_hover : pal.card_bg);
					StrokeRoundRect(g, card, 10.0f * s, pal.card_border, 1.0f);

					// Drive letter / status tile
					float tile_sz = 50.0f * s;
					Gdiplus::RectF tile(card.X + 16.0f * s, card.Y + 16.0f * s, tile_sz, tile_sz);
					Gdiplus::Color tile_bg = pal.btn_bg;
					Gdiplus::Color tile_fg = pal.text_secondary;
					std::wstring tile_txt = L"APFS";
					bool tile_big = false;
					if (mounted && v.letter != "-")
					{
						tile_bg = rw ? pal.amber_pill_bg : pal.blue_pill_bg;
						tile_fg = rw ? pal.amber_pill_fg : pal.blue_pill_fg;
						tile_txt = ToWide(v.letter);
						tile_big = true;
					}
					else if (locked)
					{
						tile_bg = pal.purple_pill_bg;
						tile_fg = pal.purple_pill_fg;
						tile_txt = L"LOCK";
					}
					else if (v.state == "failed")
					{
						tile_bg = pal.red_pill_bg;
						tile_fg = pal.red_pill_fg;
						tile_txt = L"ERR";
					}
					FillRoundRect(g, tile, 10.0f * s, tile_bg);
					Gdiplus::SolidBrush tile_br(tile_fg);
					Gdiplus::StringFormat tfmt;
					tfmt.SetAlignment(Gdiplus::StringAlignmentCenter);
					tfmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
					g.DrawString(tile_txt.c_str(), -1, tile_big ? &f_tile : &f_tile_sm, tile, &tfmt, &tile_br);

					// Title + status pill
					float text_x = tile.GetRight() + 14.0f * s;
					float right_edge = card.GetRight() - 16.0f * s;
					std::wstring vname = ToWide(v.name.empty() ? "APFS Volume" : v.name);

					bool io_err = v.note.compare(0, 10, "I/O error:") == 0;
					std::wstring pill_txt = L"Unmounted";
					Gdiplus::Color pill_bg = pal.btn_bg;
					Gdiplus::Color pill_fg = pal.text_secondary;
					if (mounted && io_err)
					{
						pill_txt = L"I/O Error";
						pill_bg = pal.red_pill_bg;
						pill_fg = pal.red_pill_fg;
					}
					else if (mounted)
					{
						pill_txt = rw ? L"Read-Write" : L"Read-Only";
						pill_bg = rw ? pal.amber_pill_bg : pal.green_pill_bg;
						pill_fg = rw ? pal.amber_pill_fg : pal.green_pill_fg;
					}
					else if (locked)
					{
						pill_txt = L"Encrypted";
						pill_bg = pal.purple_pill_bg;
						pill_fg = pal.purple_pill_fg;
					}
					else if (v.state == "disabled")
					{
						pill_txt = L"Auto-mount Off";
					}
					else if (v.state == "failed")
					{
						pill_txt = L"Mount Failed";
						pill_bg = pal.red_pill_bg;
						pill_fg = pal.red_pill_fg;
					}

					std::wstring speed_txt;
					if (mounted && (v.read_bps > 0 || v.write_bps > 0))
					{
						if (v.read_bps > 0 && v.write_bps > 0)
							speed_txt = L"\x2193 " + FormatSpeed(v.read_bps) + L"  \x2191 " + FormatSpeed(v.write_bps);
						else if (v.read_bps > 0)
							speed_txt = L"\x2193 " + FormatSpeed(v.read_bps);
						else
							speed_txt = L"\x2191 " + FormatSpeed(v.write_bps);
					}
					float pill_w = MeasureTextWidth(g, pill_txt, f_pill) + 18.0f * s;
					float speed_w = speed_txt.empty() ? 0.0f : (MeasureTextWidth(g, speed_txt, f_pill) + 18.0f * s + 6.0f * s);
					float name_max_w = (std::max)(80.0f * s, right_edge - text_x - pill_w - speed_w - 10.0f * s);
					float name_w = (std::min)(name_max_w, MeasureTextWidth(g, vname, f_card_title) + 4.0f * s);

					Gdiplus::SolidBrush name_br(pal.text_primary);
					Gdiplus::StringFormat lfmt;
					lfmt.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
					lfmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
					g.DrawString(vname.c_str(), -1, &f_card_title, Gdiplus::RectF(text_x, card.Y + 15.0f * s, name_w, 22.0f * s), &lfmt, &name_br);

					float after_pill = 0.0f;
					DrawPill(g, text_x + name_w + 8.0f * s, card.Y + 16.0f * s, 20.0f * s, pill_txt, pill_bg, pill_fg, f_pill, 9.0f * s, after_pill);
					if (!speed_txt.empty())
					{
						float unused = 0.0f;
						DrawPill(g, after_pill + 6.0f * s, card.Y + 16.0f * s, 20.0f * s, speed_txt,
							v.write_bps > 0 ? pal.amber_pill_bg : pal.blue_pill_bg,
							v.write_bps > 0 ? pal.amber_pill_fg : pal.blue_pill_fg, f_pill, 8.0f * s, unused);
					}

					// Disk source + type
					std::wstring meta = (IsPhysicalDisk(v) ? L"Physical Disk  \x2022  " : L"Disk Image  \x2022  ") + ToWide(v.disk);
					Gdiplus::SolidBrush meta_br(pal.text_secondary);
					g.DrawString(meta.c_str(), -1, &f_small, Gdiplus::RectF(text_x, card.Y + 40.0f * s, right_edge - text_x, 18.0f * s), &lfmt, &meta_br);

					float cur_y = card.Y + 62.0f * s;
					if (has_note)
					{
						Gdiplus::SolidBrush note_br((v.state == "failed" || io_err) ? pal.red_pill_fg : pal.text_muted);
						g.DrawString(ToWide(v.note).c_str(), -1, &f_small, Gdiplus::RectF(text_x, cur_y, right_edge - text_x, 18.0f * s), &lfmt, &note_br);
						cur_y += 20.0f * s;
					}

					// Space usage bar (when mounted and space info available)
					if (v.has_space && v.total_bytes > 0)
					{
						uint64_t used = v.total_bytes > v.free_bytes ? (v.total_bytes - v.free_bytes) : 0;
						float frac = static_cast<float>(static_cast<double>(used) / static_cast<double>(v.total_bytes));
						frac = (std::max)(0.0f, (std::min)(1.0f, frac));
						int pct = static_cast<int>(frac * 100.0f + 0.5f);

						std::wstring space_txt = FormatBytes(v.free_bytes) + L" free of " + FormatBytes(v.total_bytes) + L" (" + std::to_wstring(pct) + L"% used)";
						float txt_w = MeasureTextWidth(g, space_txt, f_small) + 6.0f * s;
						float bar_w = (std::max)(80.0f * s, right_edge - text_x - txt_w - 10.0f * s);
						Gdiplus::RectF track(text_x, cur_y + 6.0f * s, bar_w, 6.0f * s);
						FillRoundRect(g, track, 3.0f * s, pal.track_bg);
						if (frac > 0.005f)
						{
							float fill_w = (std::max)(6.0f * s, bar_w * frac);
							Gdiplus::Color bar_c = frac > 0.90f ? pal.red_pill_fg : rw ? pal.amber_pill_fg : pal.accent;
							FillRoundRect(g, Gdiplus::RectF(track.X, track.Y, fill_w, track.Height), 3.0f * s, bar_c);
						}
						Gdiplus::SolidBrush sp_br(pal.text_secondary);
						Gdiplus::StringFormat rfmt;
						rfmt.SetAlignment(Gdiplus::StringAlignmentFar);
						rfmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
						g.DrawString(space_txt.c_str(), -1, &f_small, Gdiplus::RectF(track.GetRight() + 8.0f * s, cur_y, txt_w, 18.0f * s), &rfmt, &sp_br);
						cur_y += 26.0f * s;
					}

					// Action buttons row 1 (primary actions)
					float bx = text_x;
					float by = card.GetBottom() - 70.0f * s;
					float bh = 28.0f * s;
					float bgap = 7.0f * s;
					auto add_btn = [&](BtnKind kind, const std::wstring &label, BtnTone tone, bool enabled, const Gdiplus::Font &font, float pad, float row_h) {
						float bw = MeasureTextWidth(g, label, font) + pad * s;
						HitButton hb;
						hb.rect = Gdiplus::RectF(bx, by, bw, row_h);
						hb.hit_rect = ClipRectY(hb.rect, content_top, content_bot);
						hb.kind = kind;
						hb.vol_index = static_cast<int>(i);
						hb.vol_uuid = v.uuid;
						hb.tone = tone;
						hb.text = label;
						hb.enabled = enabled;
						g_buttons.push_back(hb);
						DrawButton(g, hb, static_cast<int>(g_buttons.size()) - 1, s, pal, font);
						bx += bw + bgap;
					};

					if (mounted)
					{
						add_btn(BtnKind::VolOpen, L"Open", BtnTone::Primary, true, f_btn, 20.0f, bh);
						bool rw_possible = !IsPhysicalDisk(v) || g_state.allow_writes;
						add_btn(BtnKind::VolToggleRw, rw ? L"Switch to Read-Only" : L"Enable Read-Write",
							rw ? BtnTone::Amber : BtnTone::Neutral, rw_possible, f_btn, 20.0f, bh);
						add_btn(BtnKind::VolUnmount, L"Unmount", BtnTone::Neutral, true, f_btn, 20.0f, bh);
					}
					else if (locked)
					{
						add_btn(BtnKind::VolUnlock, L"Unlock\x2026", BtnTone::Purple, true, f_btn, 20.0f, bh);
					}
					else
					{
						add_btn(BtnKind::VolMount, L"Mount", BtnTone::Primary, true, f_btn, 20.0f, bh);
					}

					if (IsPhysicalDisk(v))
						add_btn(BtnKind::VolEject, L"Safely Eject", BtnTone::Neutral, true, f_btn, 20.0f, bh);

					// Action buttons row 2 (volume preferences & health check)
					bx = text_x;
					by = card.GetBottom() - 36.0f * s;
					float bh2 = 24.0f * s;
					if (!locked)
					{
						std::wstring ltr = (v.letter.empty() || v.letter == "-") ? L"Letter\x2026" : (L"Letter: " + ToWide(v.letter));
						add_btn(BtnKind::VolChangeLetter, ltr, BtnTone::Neutral, true, f_small, 16.0f, bh2);
						add_btn(BtnKind::VolToggleDotfiles, v.hide_dotfiles ? L"Dotfiles: Hidden" : L"Dotfiles: Shown", BtnTone::Neutral, true, f_small, 16.0f, bh2);
						add_btn(BtnKind::VolToggleAuto, v.state != "disabled" ? L"Auto: On" : L"Auto: Off", BtnTone::Neutral, true, f_small, 16.0f, bh2);
					}
					add_btn(BtnKind::VolVerify, v.verifying ? L"Verifying\x2026" : L"Verify Disk", BtnTone::Neutral, !v.verifying, f_small, 16.0f, bh2);
				}

				y += card_h + gap;
				total_h += card_h + gap;
			}
			total_h += pad_y - gap;
			g_max_scroll = static_cast<int>((std::max)(0.0f, total_h - view_h));
			if (g_scroll_y > g_max_scroll)
				g_scroll_y = g_max_scroll;

			// Sleek scrollbar indicator
			if (g_max_scroll > 0)
			{
				float track_h = view_h - 12.0f * s;
				float thumb_h = (std::max)(28.0f * s, track_h * (view_h / total_h));
				float thumb_y = content_top + 6.0f * s + (track_h - thumb_h) * (static_cast<float>(g_scroll_y) / static_cast<float>(g_max_scroll));
				FillRoundRect(g, Gdiplus::RectF(static_cast<float>(cw) - 9.0f * s, thumb_y, 4.0f * s, thumb_h), 2.0f * s, pal.scroll_thumb);
			}
		}

		g.ResetClip();

		// ---- Header Bar
		{
			Gdiplus::SolidBrush hdr_br(pal.header_bg);
			g.FillRectangle(&hdr_br, 0.0f, 0.0f, static_cast<float>(cw), header_h);
			Gdiplus::Pen div_pen(pal.divider, 1.0f);
			g.DrawLine(&div_pen, 0.0f, header_h - 1.0f, static_cast<float>(cw), header_h - 1.0f);

			// App icon
			int ico_sz = static_cast<int>(28.0f * s + 0.5f);
			HICON hico = static_cast<HICON>(LoadImageW(g_inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, ico_sz, ico_sz, LR_SHARED));
			if (hico)
			{
				HDC gdc = g.GetHDC();
				DrawIconEx(gdc, static_cast<int>(20.0f * s), static_cast<int>((header_h - ico_sz) * 0.5f), hico, ico_sz, ico_sz, 0, nullptr, DI_NORMAL);
				g.ReleaseHDC(gdc);
			}

			float hx = 58.0f * s;
			Gdiplus::SolidBrush title_br(pal.text_primary);
			Gdiplus::StringFormat lfmt;
			lfmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
			g.DrawString(L"winapfs", -1, &f_title, Gdiplus::RectF(hx, 12.0f * s, 90.0f * s, 28.0f * s), &lfmt, &title_br);

			float next_x = hx + MeasureTextWidth(g, L"winapfs", f_title) + 10.0f * s;
			DrawPill(g, next_x, 17.0f * s, 20.0f * s, L"v" WINAPFS_VERSION_W, pal.btn_bg, pal.text_secondary, f_pill, 8.0f * s, next_x);

			std::wstring svc_txt = g_state.service_up ? L"\x25CF Service Running" : L"\x25CF Service Stopped";
			DrawPill(g, next_x + 8.0f * s, 17.0f * s, 20.0f * s, svc_txt,
				g_state.service_up ? pal.green_pill_bg : pal.red_pill_bg,
				g_state.service_up ? pal.green_pill_fg : pal.red_pill_fg, f_pill, 9.0f * s, next_x);

			// Top-right header buttons (right-to-left)
			float rx = static_cast<float>(cw) - 20.0f * s;
			float by = (header_h - 30.0f * s) * 0.5f;
			float bh = 30.0f * s;
			auto add_hdr_btn = [&](BtnKind kind, const std::wstring &label, BtnTone tone, bool enabled = true) {
				float bw = MeasureTextWidth(g, label, f_btn) + 20.0f * s;
				rx -= bw;
				HitButton hb;
				hb.rect = Gdiplus::RectF(rx, by, bw, bh);
				hb.hit_rect = hb.rect;
				hb.kind = kind;
				hb.tone = tone;
				hb.text = label;
				hb.enabled = enabled;
				g_buttons.push_back(hb);
				DrawButton(g, hb, static_cast<int>(g_buttons.size()) - 1, s, pal, f_btn);
				rx -= 8.0f * s;
			};

			add_hdr_btn(BtnKind::About, L"About", BtnTone::Neutral);
			add_hdr_btn(BtnKind::CheckUpdates, g_state.update_checking ? L"Checking\x2026" : L"Updates",
				g_state.update_available ? BtnTone::Primary : BtnTone::Neutral, !g_state.update_checking);
			if (g_state.service_up)
			{
				add_hdr_btn(BtnKind::Rescan, L"Rescan", BtnTone::Neutral);
				add_hdr_btn(BtnKind::ToggleAllowWrites,
					g_state.allow_writes ? L"Disk Writes: ON" : L"Disk Writes: OFF",
					g_state.allow_writes ? BtnTone::Amber : BtnTone::Neutral);
			}
			else if (g_state.service_installed)
			{
				add_hdr_btn(BtnKind::StartService, L"Start Service", BtnTone::Primary);
			}
			else
			{
				add_hdr_btn(BtnKind::InstallService, L"Install Service", BtnTone::Primary);
			}
		}

		// ---- Footer Bar (includes mandatory WinFsp attribution)
		{
			Gdiplus::SolidBrush ftr_br(pal.footer_bg);
			g.FillRectangle(&ftr_br, 0.0f, content_bot, static_cast<float>(cw), footer_h);
			Gdiplus::Pen div_pen(pal.divider, 1.0f);
			g.DrawLine(&div_pen, 0.0f, content_bot, static_cast<float>(cw), content_bot);

			Gdiplus::SolidBrush m_br(pal.text_muted);
			Gdiplus::StringFormat lfmt;
			lfmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
			lfmt.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
			lfmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
			std::wstring notice = L"Uses WinFsp - Windows File System Proxy, Copyright (C) Bill Zissimopoulos";
			g.DrawString(notice.c_str(), -1, &f_small, Gdiplus::RectF(20.0f * s, content_bot, (std::max)(120.0f * s, static_cast<float>(cw) - 300.0f * s), footer_h), &lfmt, &m_br);

			size_t mounted_cnt = 0;
			for (const auto &v : g_state.vols)
				mounted_cnt += (v.state == "mounted");
			std::wstring summary = g_state.vols.empty() ? L""
				: std::to_wstring(mounted_cnt) + L" of " + std::to_wstring(g_state.vols.size()) + L" mounted";
			if (!g_state.update_available && !g_state.update_status.empty())
			{
				if (!summary.empty())
					summary += L"  \x2022  ";
				summary += ToWide(g_state.update_status);
			}
			Gdiplus::StringFormat rfmt;
			rfmt.SetAlignment(Gdiplus::StringAlignmentFar);
			rfmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
			g.DrawString(summary.c_str(), -1, &f_small, Gdiplus::RectF(static_cast<float>(cw) - 280.0f * s, content_bot, 260.0f * s, footer_h), &rfmt, &m_br);
		}
	}

	BitBlt(hdc, 0, 0, cw, ch, mem_dc, 0, 0, SRCCOPY);
	SelectObject(mem_dc, old_bmp);
	DeleteObject(bmp);
	DeleteDC(mem_dc);
	EndPaint(hwnd, &ps);
}

int HitTestButton(int x, int y)
{
	float fx = static_cast<float>(x);
	float fy = static_cast<float>(y);
	for (int i = static_cast<int>(g_buttons.size()) - 1; i >= 0; i--)
	{
		const HitButton &b = g_buttons[static_cast<size_t>(i)];
		if (b.enabled && b.hit_rect.Width > 0.0f && b.hit_rect.Height > 0.0f && b.hit_rect.Contains(fx, fy))
			return i;
	}
	return -1;
}

int HitTestCard(int x, int y)
{
	float fx = static_cast<float>(x);
	float fy = static_cast<float>(y);
	for (size_t i = 0; i < g_card_rects.size(); i++)
	{
		const Gdiplus::RectF &r = g_card_rects[i];
		if (r.Width > 0.0f && r.Height > 0.0f && r.Contains(fx, fy))
			return static_cast<int>(i);
	}
	return -1;
}

void TriggerButton(const HitButton &b)
{
	switch (b.kind)
	{
	case BtnKind::ToggleAllowWrites:
		if (g_cbs.on_toggle_allow_writes)
			g_cbs.on_toggle_allow_writes(g_state.allow_writes);
		break;
	case BtnKind::Rescan:
		if (g_cbs.on_rescan)
			g_cbs.on_rescan();
		break;
	case BtnKind::StartService:
		if (g_cbs.on_start_service)
			g_cbs.on_start_service();
		break;
	case BtnKind::InstallService:
		if (g_cbs.on_install_service)
			g_cbs.on_install_service();
		break;
	case BtnKind::About:
		if (g_cbs.on_about)
			g_cbs.on_about();
		break;
	case BtnKind::CheckUpdates:
		if (g_cbs.on_check_updates)
			g_cbs.on_check_updates();
		break;
	case BtnKind::InstallUpdate:
		if (g_cbs.on_install_update)
			g_cbs.on_install_update();
		break;
	case BtnKind::OpenReleasePage:
		if (g_cbs.on_open_release_page)
			g_cbs.on_open_release_page();
		break;
	default:
		if (b.vol_index < 0 || static_cast<size_t>(b.vol_index) >= g_state.vols.size())
			break;
		{
			const VolumeStatus v = g_state.vols[static_cast<size_t>(b.vol_index)];
			switch (b.kind)
			{
			case BtnKind::VolOpen: if (g_cbs.on_open) g_cbs.on_open(v); break;
			case BtnKind::VolMount: if (g_cbs.on_mount) g_cbs.on_mount(v); break;
			case BtnKind::VolUnmount: if (g_cbs.on_unmount) g_cbs.on_unmount(v); break;
			case BtnKind::VolUnlock: if (g_cbs.on_unlock) g_cbs.on_unlock(v); break;
			case BtnKind::VolEject: if (g_cbs.on_eject) g_cbs.on_eject(v); break;
			case BtnKind::VolToggleRw: if (g_cbs.on_toggle_rw) g_cbs.on_toggle_rw(v); break;
			case BtnKind::VolToggleAuto: if (g_cbs.on_toggle_auto) g_cbs.on_toggle_auto(v); break;
			case BtnKind::VolToggleDotfiles: if (g_cbs.on_toggle_dotfiles) g_cbs.on_toggle_dotfiles(v); break;
			case BtnKind::VolChangeLetter: if (g_cbs.on_change_letter) g_cbs.on_change_letter(v); break;
			case BtnKind::VolVerify: if (g_cbs.on_verify) g_cbs.on_verify(v); break;
			default: break;
			}
		}
		break;
	}
}

void ScrollBy(int delta)
{
	int next = (std::max)(0, (std::min)(g_max_scroll, g_scroll_y + delta));
	if (next != g_scroll_y)
	{
		g_scroll_y = next;
		g_hover_btn = -1;
		g_hover_card = -1;
		InvalidateRect(g_hwnd, nullptr, FALSE);
	}
}

LRESULT CALLBACK DashProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg)
	{
	case WM_CREATE:
		g_dpi = GetDpiForWindow(hwnd);
		if (!g_dpi)
			g_dpi = 96;
		g_dark = IsSystemDarkMode();
		ApplyDwmAttributes(hwnd);
		return 0;

	case WM_ERASEBKGND:
		return 1;

	case WM_PAINT:
		PaintWindow(hwnd);
		return 0;

	case WM_SIZE:
		InvalidateRect(hwnd, nullptr, FALSE);
		return 0;

	case WM_GETMINMAXINFO:
	{
		auto *mmi = reinterpret_cast<MINMAXINFO *>(lp);
		float s = static_cast<float>(g_dpi) / 96.0f;
		mmi->ptMinTrackSize.x = static_cast<LONG>(740.0f * s);
		mmi->ptMinTrackSize.y = static_cast<LONG>(500.0f * s);
		return 0;
	}

	case WM_DPICHANGED:
	{
		g_dpi = HIWORD(wp);
		auto *r = reinterpret_cast<RECT *>(lp);
		SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
		InvalidateRect(hwnd, nullptr, FALSE);
		return 0;
	}

	case WM_SETTINGCHANGE:
	{
		bool dark = IsSystemDarkMode();
		if (dark != g_dark)
		{
			g_dark = dark;
			ApplyDwmAttributes(hwnd);
			InvalidateRect(hwnd, nullptr, FALSE);
		}
		return 0;
	}

	case WM_MOUSEMOVE:
	{
		if (!g_tracking_mouse)
		{
			TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
			TrackMouseEvent(&tme);
			g_tracking_mouse = true;
		}
		int x = GET_X_LPARAM(lp);
		int y = GET_Y_LPARAM(lp);
		int hb = HitTestButton(x, y);
		int hc = HitTestCard(x, y);
		if (hb != g_hover_btn || hc != g_hover_card)
		{
			g_hover_btn = hb;
			g_hover_card = hc;
			InvalidateRect(hwnd, nullptr, FALSE);
		}
		return 0;
	}

	case WM_MOUSELEAVE:
		g_tracking_mouse = false;
		if (g_hover_btn != -1 || g_hover_card != -1 || g_has_press)
		{
			g_hover_btn = -1;
			g_hover_card = -1;
			g_has_press = false;
			g_press_uuid.clear();
			InvalidateRect(hwnd, nullptr, FALSE);
		}
		return 0;

	case WM_SETCURSOR:
		if (LOWORD(lp) == HTCLIENT && g_hover_btn != -1)
		{
			SetCursor(LoadCursorW(nullptr, IDC_HAND));
			return TRUE;
		}
		break;

	case WM_LBUTTONDOWN:
	{
		int x = GET_X_LPARAM(lp);
		int y = GET_Y_LPARAM(lp);
		int hb = HitTestButton(x, y);
		if (hb != -1)
		{
			const HitButton &b = g_buttons[static_cast<size_t>(hb)];
			g_has_press = true;
			g_press_kind = b.kind;
			g_press_uuid = b.vol_uuid;
			SetCapture(hwnd);
			InvalidateRect(hwnd, nullptr, FALSE);
		}
		return 0;
	}

	case WM_LBUTTONUP:
	{
		int x = GET_X_LPARAM(lp);
		int y = GET_Y_LPARAM(lp);
		int hb = HitTestButton(x, y);
		bool was_pressed = g_has_press;
		BtnKind press_kind = g_press_kind;
		std::string press_uuid = std::move(g_press_uuid);
		g_has_press = false;
		g_press_uuid.clear();
		if (GetCapture() == hwnd)
			ReleaseCapture();
		InvalidateRect(hwnd, nullptr, FALSE);
		if (was_pressed && hb != -1 && static_cast<size_t>(hb) < g_buttons.size())
		{
			const HitButton &b = g_buttons[static_cast<size_t>(hb)];
			if (b.kind == press_kind && b.vol_uuid == press_uuid)
				TriggerButton(b);
		}
		return 0;
	}

	case WM_MOUSEWHEEL:
	{
		int delta = GET_WHEEL_DELTA_WPARAM(wp);
		int step = static_cast<int>(48.0f * (static_cast<float>(g_dpi) / 96.0f));
		ScrollBy(-(delta * step) / WHEEL_DELTA);
		return 0;
	}

	case WM_KEYDOWN:
	{
		int step = static_cast<int>(48.0f * (static_cast<float>(g_dpi) / 96.0f));
		if (wp == VK_DOWN)
			ScrollBy(step);
		else if (wp == VK_UP)
			ScrollBy(-step);
		else if (wp == VK_NEXT)
			ScrollBy(step * 5);
		else if (wp == VK_PRIOR)
			ScrollBy(-step * 5);
		return 0;
	}

	case WM_CLOSE:
		ShowWindow(hwnd, SW_HIDE);
		return 0;

	case WM_DESTROY:
		g_hwnd = nullptr;
		return 0;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

void DashboardInit(HINSTANCE inst, HWND owner_tray_wnd, DashboardCallbacks cbs)
{
	g_inst = inst;
	g_tray_wnd = owner_tray_wnd;
	g_cbs = std::move(cbs);

	WNDCLASSEXW wc = { sizeof(wc) };
	wc.lpfnWndProc = DashProc;
	wc.hInstance = inst;
	wc.lpszClassName = kClassName;
	wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
	wc.hIconSm = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
	RegisterClassExW(&wc);
}

void DashboardShow()
{
	if (!g_hwnd)
	{
		UINT dpi = 96;
		HDC sdc = GetDC(nullptr);
		if (sdc)
		{
			dpi = static_cast<UINT>(GetDeviceCaps(sdc, LOGPIXELSX));
			ReleaseDC(nullptr, sdc);
		}
		float s = static_cast<float>(dpi) / 96.0f;
		int w = static_cast<int>(800.0f * s);
		int h = static_cast<int>(560.0f * s);
		RECT wa = {};
		SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
		int x = wa.left + ((wa.right - wa.left) - w) / 2;
		int y = wa.top + ((wa.bottom - wa.top) - h) / 2;

		g_hwnd = CreateWindowExW(0, kClassName, L"winapfs \x2014 APFS for Windows",
			WS_OVERLAPPEDWINDOW, x, y, w, h, nullptr, nullptr, g_inst, nullptr);
	}
	if (!g_hwnd)
		return;
	ShowWindow(g_hwnd, IsIconic(g_hwnd) ? SW_RESTORE : SW_SHOW);
	SetForegroundWindow(g_hwnd);
	InvalidateRect(g_hwnd, nullptr, FALSE);
}

void DashboardUpdate(const DashboardState &state)
{
	g_state = state;
	if (g_hwnd && IsWindowVisible(g_hwnd))
		InvalidateRect(g_hwnd, nullptr, FALSE);
}

void DashboardDestroy()
{
	if (g_hwnd)
	{
		DestroyWindow(g_hwnd);
		g_hwnd = nullptr;
	}
}

HWND DashboardHwnd()
{
	return g_hwnd;
}

} // namespace winapfs
