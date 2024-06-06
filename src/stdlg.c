/*
 * Rufus: The Reliable USB Formatting Utility
 * Standard Dialog Routines (Browse for folder, About, etc)
 * Copyright © 2011-2024 Pete Batard <pete@akeo.ie>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/* Memory leaks detection - define _CRTDBG_MAP_ALLOC as preprocessor macro */
#ifdef _CRTDBG_MAP_ALLOC
#include <stdlib.h>
#include <crtdbg.h>
#endif

#include <windows.h>
#include <windowsx.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <shlobj.h>
#include <commdlg.h>
#include <richedit.h>
#include <assert.h>

#include "rufus.h"
#include "missing.h"
#include "resource.h"
#include "msapi_utf8.h"
#include "localization.h"
#include "ui.h"

#include "registry.h"
#include "settings.h"
#include "license.h"

/* Globals */
extern BOOL is_x86_64, appstore_version;
extern char unattend_username[MAX_USERNAME_LENGTH];
static HICON hMessageIcon = (HICON)INVALID_HANDLE_VALUE;
static char* szMessageText = NULL;
static char* szMessageTitle = NULL;
static char **szDialogItem;
static int nDialogItems;
static const SETTEXTEX friggin_microsoft_unicode_amateurs = { ST_DEFAULT, CP_UTF8 };
static BOOL notification_is_question;
static const notification_info* notification_more_info;
static const char* notification_dont_display_setting;
static WNDPROC update_original_proc = NULL;
static HWINEVENTHOOK ap_weh = NULL;
static char title_str[2][128], button_str[128];
HWND hFidoDlg = NULL;

/*
 * https://blogs.msdn.microsoft.com/oldnewthing/20040802-00/?p=38283/
 */
void SetDialogFocus(HWND hDlg, HWND hCtrl)
{
	SendMessage(hDlg, WM_NEXTDLGCTL, (WPARAM)hCtrl, TRUE);
}

/*
 * Return the UTF8 path of a file selected through a load or save dialog
 * All string parameters are UTF-8
 * IMPORTANT NOTE: Remember that you need to call CoInitializeEx() for
 * *EACH* thread you invoke FileDialog from, as GetDisplayName() will
 * return error 0x8001010E otherwise.
 */
char* FileDialog(BOOL save, char* path, const ext_t* ext, UINT* selected_ext)
{
	size_t i;
	char* filepath = NULL;
	HRESULT hr = FALSE;
	IFileDialog *pfd = NULL;
	IShellItem *psiResult;
	COMDLG_FILTERSPEC* filter_spec = NULL;
	wchar_t *wpath = NULL, *wfilename = NULL, *wext = NULL;
	IShellItem *si_path = NULL;	// Automatically freed

	if ((ext == NULL) || (ext->count == 0) || (ext->extension == NULL) || (ext->description == NULL))
		return NULL;

	filter_spec = (COMDLG_FILTERSPEC*)calloc(ext->count + 1, sizeof(COMDLG_FILTERSPEC));
	if (filter_spec == NULL)
		return NULL;

	dialog_showing++;

	// Setup the file extension filter table
	for (i = 0; i < ext->count; i++) {
		filter_spec[i].pszSpec = utf8_to_wchar(ext->extension[i]);
		filter_spec[i].pszName = utf8_to_wchar(ext->description[i]);
	}
	filter_spec[i].pszSpec = L"*.*";
	filter_spec[i].pszName = utf8_to_wchar(lmprintf(MSG_107));

	hr = CoCreateInstance(save ? &CLSID_FileSaveDialog : &CLSID_FileOpenDialog, NULL, CLSCTX_INPROC,
		&IID_IFileDialog, (LPVOID)&pfd);
	if (SUCCEEDED(hr) && (pfd == NULL))	// Never trust Microsoft APIs to do the right thing
		hr = RUFUS_ERROR(ERROR_API_UNAVAILABLE);

	if (FAILED(hr)) {
		SetLastError(hr);
		uprintf("CoCreateInstance for FileOpenDialog failed: %s", WindowsErrorString());
		goto out;
	}

	// Set the file extension filters
	IFileDialog_SetFileTypes(pfd, (UINT)ext->count + 1, filter_spec);

	if (path == NULL) {
		// Try to use the "Downloads" folder as the initial default directory
		const GUID download_dir_guid =
			{ 0x374de290, 0x123f, 0x4565, { 0x91, 0x64, 0x39, 0xc4, 0x92, 0x5e, 0x46, 0x7b } };
		hr = SHGetKnownFolderPath(&download_dir_guid, 0, 0, &wpath);
		if (SUCCEEDED(hr)) {
			hr = SHCreateItemFromParsingName(wpath, NULL, &IID_IShellItem, (LPVOID)&si_path);
			if (SUCCEEDED(hr)) {
				IFileDialog_SetDefaultFolder(pfd, si_path);
			}
			CoTaskMemFree(wpath);
		}
	} else {
		wpath = utf8_to_wchar(path);
		hr = SHCreateItemFromParsingName(wpath, NULL, &IID_IShellItem, (LPVOID)&si_path);
		if (SUCCEEDED(hr)) {
			IFileDialog_SetFolder(pfd, si_path);
		}
		safe_free(wpath);
	}

	// Set the default filename
	wfilename = utf8_to_wchar((ext->filename == NULL) ? "" : ext->filename);
	if (wfilename != NULL)
		IFileDialog_SetFileName(pfd, wfilename);
	// Set a default extension so that when the user switches filters it gets
	// automatically updated. Note that the IFileDialog::SetDefaultExtension()
	// doc says the extension shouldn't be prefixed with unwanted characters
	// but it appears to work regardless so we don't bother cleaning it.
	wext = utf8_to_wchar((ext->extension == NULL) ? "" : ext->extension[0]);
	if (wext != NULL)
		IFileDialog_SetDefaultExtension(pfd, wext);
	// Set the current selected extension
	IFileDialog_SetFileTypeIndex(pfd, selected_ext == NULL ? 0 : *selected_ext);

	// Display the dialog and (optionally) get the selected extension index
	hr = IFileDialog_Show(pfd, hMainDialog);
	if (selected_ext != NULL)
		IFileDialog_GetFileTypeIndex(pfd, selected_ext);

	// Cleanup
	safe_free(wext);
	safe_free(wfilename);
	for (i = 0; i < ext->count; i++) {
		safe_free(filter_spec[i].pszSpec);
		safe_free(filter_spec[i].pszName);
	}
	safe_free(filter_spec[i].pszName);
	safe_free(filter_spec);

	if (SUCCEEDED(hr)) {
		// Obtain the result of the user's interaction with the dialog.
		hr = IFileDialog_GetResult(pfd, &psiResult);
		if (SUCCEEDED(hr)) {
			hr = IShellItem_GetDisplayName(psiResult, SIGDN_FILESYSPATH, &wpath);
			if (SUCCEEDED(hr)) {
				filepath = wchar_to_utf8(wpath);
				CoTaskMemFree(wpath);
			} else {
				SetLastError(hr);
				uprintf("Unable to access file path: %s", WindowsErrorString());
			}
			IShellItem_Release(psiResult);
		}
	} else if (HRESULT_CODE(hr) != ERROR_CANCELLED) {
		// If it's not a user cancel, assume the dialog didn't show and fallback
		SetLastError(hr);
		uprintf("Could not show FileOpenDialog: %s", WindowsErrorString());
	}

out:
	safe_free(filter_spec);
	if (pfd != NULL)
		IFileDialog_Release(pfd);
	dialog_showing--;
	return filepath;
}

/*
 * Create the application status bar
 */
void CreateStatusBar(void)
{
	RECT rect;
	int edge[2];
	HFONT hFont;

	// Create the status bar
	hStatus = CreateWindowEx(0, STATUSCLASSNAME, NULL, WS_CHILD | WS_VISIBLE | SBARS_TOOLTIPS,
		CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, hMainDialog,
		(HMENU)IDC_STATUS, hMainInstance, NULL);

	// Create 2 status areas
	GetClientRect(hMainDialog, &rect);
	edge[0] = rect.right - (int)(SB_TIMER_SECTION_SIZE * fScale);
	edge[1] = rect.right;
	SendMessage(hStatus, SB_SETPARTS, (WPARAM)ARRAYSIZE(edge), (LPARAM)&edge);

	// Set the font
	hFont = CreateFontA(-MulDiv(9, GetDeviceCaps(GetDC(hMainDialog), LOGPIXELSY), 72),
		0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
		0, 0, PROOF_QUALITY, 0, "Segoe UI");
	SendMessage(hStatus, WM_SETFONT, (WPARAM)hFont, TRUE);
}

/*
 * Center a dialog with regards to the main application Window or the desktop
 * See https://docs.microsoft.com/en-gb/windows/desktop/dlgbox/using-dialog-boxes#initializing-a-dialog-box
 */
void CenterDialog(HWND hDlg, HWND hParent)
{
	RECT rc, rcDlg, rcParent;

	if (hParent == NULL)
		hParent = GetParent(hDlg);
	if (hParent == NULL)
		hParent = GetDesktopWindow();

	GetWindowRect(hParent, &rcParent);
	GetWindowRect(hDlg, &rcDlg);
	CopyRect(&rc, &rcParent);

	// Offset the parent and dialog box rectangles so that right and bottom
	// values represent the width and height, and then offset the parent again
	// to discard space taken up by the dialog box.
	OffsetRect(&rcDlg, -rcDlg.left, -rcDlg.top);
	OffsetRect(&rc, -rc.left, -rc.top);
	OffsetRect(&rc, -rcDlg.right, -rcDlg.bottom);

	SetWindowPos(hDlg, HWND_TOP, rcParent.left + (rc.right / 2), rcParent.top + (rc.bottom / 2) - 25, 0, 0, SWP_NOSIZE);
}

// http://stackoverflow.com/questions/431470/window-border-width-and-height-in-win32-how-do-i-get-it
SIZE GetBorderSize(HWND hDlg)
{
	RECT rect = {0, 0, 0, 0};
	SIZE size = {0, 0};
	WINDOWINFO wi;
	wi.cbSize = sizeof(WINDOWINFO);

	GetWindowInfo(hDlg, &wi);

	AdjustWindowRectEx(&rect, wi.dwStyle, FALSE, wi.dwExStyle);
	size.cx = rect.right - rect.left;
	size.cy = rect.bottom - rect.top;
	return size;
}

void ResizeMoveCtrl(HWND hDlg, HWND hCtrl, int dx, int dy, int dw, int dh, float scale)
{
	RECT rect;
	POINT point;
	SIZE border;

	GetWindowRect(hCtrl, &rect);
	point.x = (right_to_left_mode && (hDlg != hCtrl))?rect.right:rect.left;
	point.y = rect.top;
	if (hDlg != hCtrl)
		ScreenToClient(hDlg, &point);
	GetClientRect(hCtrl, &rect);

	// If the control has any borders (dialog, edit box), take them into account
	border = GetBorderSize(hCtrl);
	MoveWindow(hCtrl, point.x + (int)(scale*(float)dx), point.y + (int)(scale*(float)dy),
		(rect.right - rect.left) + (int)(scale*(float)dw + border.cx),
		(rect.bottom - rect.top) + (int)(scale*(float)dh + border.cy), TRUE);
	// Don't be tempted to call InvalidateRect() here - it causes intempestive whole screen refreshes
}

void ResizeButtonHeight(HWND hDlg, int id)
{
	HWND hCtrl, hPrevCtrl;
	RECT rc;
	int dy = 0;

	hCtrl = GetDlgItem(hDlg, id);
	GetWindowRect(hCtrl, &rc);
	MapWindowPoints(NULL, hDlg, (POINT*)&rc, 2);
	if (rc.bottom - rc.top < bh)
		dy = (bh - (rc.bottom - rc.top)) / 2;
	hPrevCtrl = GetNextWindow(hCtrl, GW_HWNDPREV);
	SetWindowPos(hCtrl, hPrevCtrl, rc.left, rc.top - dy, rc.right - rc.left, bh, 0);
}

/*
 * License callback
 */
INT_PTR CALLBACK LicenseCallback(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
	LONG_PTR style;
	HWND hLicense;
	switch (message) {
	case WM_INITDIALOG:
		hLicense = GetDlgItem(hDlg, IDC_LICENSE_TEXT);
		apply_localization(IDD_LICENSE, hDlg);
		CenterDialog(hDlg, NULL);
		ResizeButtonHeight(hDlg, IDCANCEL);
		// Suppress any inherited RTL flags
		style = GetWindowLongPtr(hLicense, GWL_EXSTYLE);
		style &= ~(WS_EX_RTLREADING | WS_EX_RIGHT | WS_EX_LEFTSCROLLBAR);
		SetWindowLongPtr(hLicense, GWL_EXSTYLE, style);
		style = GetWindowLongPtr(hLicense, GWL_STYLE);
		style &= ~(ES_RIGHT);
		SetWindowLongPtr(hLicense, GWL_STYLE, style);
		SetDlgItemTextA(hDlg, IDC_LICENSE_TEXT, gplv3);
		break;
	case WM_COMMAND:
		switch (LOWORD(wParam)) {
		case IDOK:
		case IDCANCEL:
			reset_localization(IDD_LICENSE);
			EndDialog(hDlg, LOWORD(wParam));
			return (INT_PTR)TRUE;
		}
	}
	return (INT_PTR)FALSE;
}

/*
 * About dialog callback
 */
INT_PTR CALLBACK AboutCallback(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
	int i, dy;
	const int edit_id[2] = { IDC_ABOUT_BLURB, IDC_ABOUT_COPYRIGHTS };
	char about_blurb[2048];
	const char* edit_text[2] = { about_blurb, additional_copyrights };
	HWND hEdit[2], hCtrl;
	TEXTRANGEW tr;
	ENLINK* enl;
	RECT rc;
	REQRESIZE* rsz;
	wchar_t wUrl[256];
	static BOOL resized_already = TRUE;

	switch (message) {
	case WM_INITDIALOG:
		resized_already = FALSE;
		// Execute dialog localization
		apply_localization(IDD_ABOUTBOX, hDlg);
		SetTitleBarIcon(hDlg);
		CenterDialog(hDlg, NULL);
		// Resize the 'License' button
		hCtrl = GetDlgItem(hDlg, IDC_ABOUT_LICENSE);
		GetWindowRect(hCtrl, &rc);
		MapWindowPoints(NULL, hDlg, (POINT*)&rc, 2);
		dy = 0;
		if (rc.bottom - rc.top < bh)
			dy = (bh - (rc.bottom - rc.top)) / 2;
		SetWindowPos(hCtrl, NULL, rc.left, rc.top - dy,
			max(rc.right - rc.left, GetTextSize(hCtrl, NULL).cx + cbw), bh, SWP_NOZORDER);
		ResizeButtonHeight(hDlg, IDOK);
		static_sprintf(about_blurb, about_blurb_format, lmprintf(MSG_174|MSG_RTF),
			lmprintf(MSG_175|MSG_RTF, rufus_version[0], rufus_version[1], rufus_version[2]),
			"Fork made by Smu1zel. Original software is Copyright © 2011-2024 Pete Batard",
			lmprintf(MSG_176|MSG_RTF), lmprintf(MSG_177|MSG_RTF), lmprintf(MSG_178|MSG_RTF));
		for (i = 0; i < ARRAYSIZE(hEdit); i++) {
			hEdit[i] = GetDlgItem(hDlg, edit_id[i]);
			SendMessage(hEdit[i], EM_AUTOURLDETECT, 1, 0);
			/* Can't use SetDlgItemText, because it only works with RichEdit20A... and VS insists
			 * on reverting to RichEdit20W as soon as you edit the dialog. You can try all the W
			 * methods you want, it JUST WON'T WORK unless you use EM_SETTEXTEX. Also see:
			 * http://blog.kowalczyk.info/article/eny/Setting-unicode-rtf-text-in-rich-edit-control.html */
			SendMessageA(hEdit[i], EM_SETTEXTEX, (WPARAM)&friggin_microsoft_unicode_amateurs, (LPARAM)edit_text[i]);
			SendMessage(hEdit[i], EM_SETSEL, -1, -1);
			SendMessage(hEdit[i], EM_SETEVENTMASK, 0, ENM_LINK | ((i == 0) ? ENM_REQUESTRESIZE : 0));
			SendMessage(hEdit[i], EM_SETBKGNDCOLOR, 0, (LPARAM)GetSysColor(COLOR_BTNFACE));
		}
		// Need to send an explicit SetSel to avoid being positioned at the end of richedit control when tabstop is used
		SendMessage(hEdit[1], EM_SETSEL, 0, 0);
		SendMessage(hEdit[0], EM_REQUESTRESIZE, 0, 0);
		break;
	case WM_NOTIFY:
		switch (((LPNMHDR)lParam)->code) {
		case EN_REQUESTRESIZE:
			if (!resized_already) {
				resized_already = TRUE;
				GetWindowRect(GetDlgItem(hDlg, edit_id[0]), &rc);
				dy = rc.bottom - rc.top;
				rsz = (REQRESIZE *)lParam;
				dy -= rsz->rc.bottom - rsz->rc.top;
				ResizeMoveCtrl(hDlg, GetDlgItem(hDlg, edit_id[0]), 0, 0, 0, -dy, 1.0f);
				ResizeMoveCtrl(hDlg, GetDlgItem(hDlg, edit_id[1]), 0, -dy, 0, dy, 1.0f);
			}
			break;
		case EN_LINK:
			enl = (ENLINK*) lParam;
			if (enl->msg == WM_LBUTTONUP) {
				tr.lpstrText = wUrl;
				tr.chrg.cpMin = enl->chrg.cpMin;
				tr.chrg.cpMax = enl->chrg.cpMax;
				SendMessageW(enl->nmhdr.hwndFrom, EM_GETTEXTRANGE, 0, (LPARAM)&tr);
				wUrl[ARRAYSIZE(wUrl)-1] = 0;
				ShellExecuteW(hDlg, L"open", wUrl, NULL, NULL, SW_SHOWNORMAL);
			}
			break;
		}
		break;
	case WM_COMMAND:
		switch (LOWORD(wParam)) {
		case IDOK:
		case IDCANCEL:
			reset_localization(IDD_ABOUTBOX);
			EndDialog(hDlg, LOWORD(wParam));
			return (INT_PTR)TRUE;
		case IDC_ABOUT_LICENSE:
			MyDialogBox(hMainInstance, IDD_LICENSE, hDlg, LicenseCallback);
			break;
		}
		break;
	}
	return (INT_PTR)FALSE;
}

INT_PTR CreateAboutBox(void)
{
	INT_PTR r;
	dialog_showing++;
	r = MyDialogBox(hMainInstance, IDD_ABOUTBOX, hMainDialog, AboutCallback);
	dialog_showing--;
	return r;
}

/*
 * We use our own MessageBox for notifications to have greater control (center, no close button, etc)
 */
INT_PTR CALLBACK NotificationCallback(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
	LRESULT loc;
	int i, dh, cbh = 0;
	// Prevent resizing
	static LRESULT disabled[9] = { HTLEFT, HTRIGHT, HTTOP, HTBOTTOM, HTSIZE,
		HTTOPLEFT, HTTOPRIGHT, HTBOTTOMLEFT, HTBOTTOMRIGHT };
	static HBRUSH background_brush, separator_brush, buttonface_brush;
	// To use the system message font
	NONCLIENTMETRICS ncm;
	HFONT hDlgFont;
	HWND hCtrl;
	RECT rc;
	HDC hDC;

	switch (message) {
	case WM_INITDIALOG:
		// Get the system message box font. See http://stackoverflow.com/a/6057761
		ncm.cbSize = sizeof(ncm);
		// If we're compiling with the Vista SDK or later, the NONCLIENTMETRICS struct
		// will be the wrong size for previous versions, so we need to adjust it.
#if defined(_MSC_VER) && (_MSC_VER >= 1500) && (_WIN32_WINNT >= _WIN32_WINNT_VISTA)
		ncm.cbSize -= sizeof(ncm.iPaddedBorderWidth);
#endif
		SystemParametersInfo(SPI_GETNONCLIENTMETRICS, ncm.cbSize, &ncm, 0);
		hDlgFont = CreateFontIndirect(&(ncm.lfMessageFont));
		// Set the dialog to use the system message box font
		SendMessage(hDlg, WM_SETFONT, (WPARAM)hDlgFont, MAKELPARAM(TRUE, 0));
		SendMessage(GetDlgItem(hDlg, IDC_NOTIFICATION_TEXT), WM_SETFONT, (WPARAM)hDlgFont, MAKELPARAM(TRUE, 0));
		SendMessage(GetDlgItem(hDlg, IDC_MORE_INFO), WM_SETFONT, (WPARAM)hDlgFont, MAKELPARAM(TRUE, 0));
		SendMessage(GetDlgItem(hDlg, IDYES), WM_SETFONT, (WPARAM)hDlgFont, MAKELPARAM(TRUE, 0));
		SendMessage(GetDlgItem(hDlg, IDNO), WM_SETFONT, (WPARAM)hDlgFont, MAKELPARAM(TRUE, 0));
		if (bh != 0) {
			ResizeButtonHeight(hDlg, IDC_MORE_INFO);
			ResizeButtonHeight(hDlg, IDYES);
			ResizeButtonHeight(hDlg, IDNO);
		}

		apply_localization(IDD_NOTIFICATION, hDlg);
		background_brush = CreateSolidBrush(GetSysColor(COLOR_WINDOW));
		separator_brush = CreateSolidBrush(GetSysColor(COLOR_3DLIGHT));
		buttonface_brush = CreateSolidBrush(GetSysColor(COLOR_BTNFACE));
		SetTitleBarIcon(hDlg);
		CenterDialog(hDlg, NULL);
		// Change the default icon
		if (Static_SetIcon(GetDlgItem(hDlg, IDC_NOTIFICATION_ICON), hMessageIcon) == 0) {
			uprintf("Could not set dialog icon\n");
		}
		// Set the dialog title
		if (szMessageTitle != NULL) {
			SetWindowTextU(hDlg, szMessageTitle);
		}
		// Enable/disable the buttons and set text
		if (!notification_is_question) {
			SetWindowTextU(GetDlgItem(hDlg, IDNO), lmprintf(MSG_006));
		} else {
			ShowWindow(GetDlgItem(hDlg, IDYES), SW_SHOW);
		}
		hCtrl = GetDlgItem(hDlg, IDC_DONT_DISPLAY_AGAIN);
		if (notification_dont_display_setting != NULL) {
			SetWindowTextU(hCtrl, lmprintf(MSG_127));
		} else {
			// Remove the "Don't display again" checkbox
			ShowWindow(hCtrl, SW_HIDE);
			GetWindowRect(hCtrl, &rc);
			MapWindowPoints(NULL, hDlg, (POINT*)&rc, 2);
			cbh = rc.bottom - rc.top;
		}
		if ((notification_more_info != NULL) && (notification_more_info->callback != NULL)) {
			hCtrl = GetDlgItem(hDlg, IDC_MORE_INFO);
			// Resize the 'More information' button
			GetWindowRect(hCtrl, &rc);
			MapWindowPoints(NULL, hDlg, (POINT*)&rc, 2);
			SetWindowPos(hCtrl, NULL, rc.left, rc.top,
				max(rc.right - rc.left, GetTextSize(hCtrl, NULL).cx + cbw), rc.bottom - rc.top, SWP_NOZORDER);
			ShowWindow(hCtrl, SW_SHOW);
		}
		// Set the control text and resize the dialog if needed
		if (szMessageText != NULL) {
			hCtrl = GetDlgItem(hDlg, IDC_NOTIFICATION_TEXT);
			SetWindowTextU(hCtrl, szMessageText);
			hDC = GetDC(hCtrl);
			SelectFont(hDC, hDlgFont);	// Yes, you *MUST* reapply the font to the DC, even after SetWindowText!
			GetWindowRect(hCtrl, &rc);
			dh = rc.bottom - rc.top;
			DrawTextU(hDC, szMessageText, -1, &rc, DT_CALCRECT | DT_WORDBREAK);
			dh = max(rc.bottom - rc.top - dh + (int)(8.0f * fScale), 0);
			safe_release_dc(hCtrl, hDC);
			ResizeMoveCtrl(hDlg, hCtrl, 0, 0, 0, dh, 1.0f);
			ResizeMoveCtrl(hDlg, hDlg, 0, 0, 0, dh - cbh, 1.0f);
			ResizeMoveCtrl(hDlg, GetDlgItem(hDlg, -1), 0, 0, 0, dh, 1.0f);	// IDC_STATIC = -1
			ResizeMoveCtrl(hDlg, GetDlgItem(hDlg, IDC_SELECTION_LINE), 0, dh, 0, 0, 1.0f);
			ResizeMoveCtrl(hDlg, GetDlgItem(hDlg, IDC_DONT_DISPLAY_AGAIN), 0, dh, 0, 0, 1.0f);
			ResizeMoveCtrl(hDlg, GetDlgItem(hDlg, IDC_MORE_INFO), 0, dh - cbh, 0, 0, 1.0f);
			ResizeMoveCtrl(hDlg, GetDlgItem(hDlg, IDYES), 0, dh -cbh, 0, 0, 1.0f);
			ResizeMoveCtrl(hDlg, GetDlgItem(hDlg, IDNO), 0, dh -cbh, 0, 0, 1.0f);
		}
		return (INT_PTR)TRUE;
	case WM_CTLCOLORSTATIC:
		// Change the background colour for static text and icon
		SetBkMode((HDC)wParam, TRANSPARENT);
		if ((HWND)lParam == GetDlgItem(hDlg, IDC_NOTIFICATION_LINE)) {
			return (INT_PTR)separator_brush;
		}
		if ((HWND)lParam == GetDlgItem(hDlg, IDC_DONT_DISPLAY_AGAIN)) {
			return (INT_PTR)buttonface_brush;
		}
		return (INT_PTR)background_brush;
	case WM_NCHITTEST:
		// Check coordinates to prevent resize actions
		loc = DefWindowProc(hDlg, message, wParam, lParam);
		for(i = 0; i < 9; i++) {
			if (loc == disabled[i]) {
				return (INT_PTR)TRUE;
			}
		}
		return (INT_PTR)FALSE;
	case WM_COMMAND:
		switch (LOWORD(wParam)) {
		case IDOK:
		case IDCANCEL:
		case IDYES:
		case IDNO:
			if (IsDlgButtonChecked(hDlg, IDC_DONT_DISPLAY_AGAIN) == BST_CHECKED) {
				WriteSettingBool(SETTING_DISABLE_SECURE_BOOT_NOTICE, TRUE);
			}
			EndDialog(hDlg, LOWORD(wParam));
			return (INT_PTR)TRUE;
		case IDC_MORE_INFO:
			if (notification_more_info != NULL) {
				assert(notification_more_info->callback != NULL);
				if (notification_more_info->id == MORE_INFO_URL) {
					ShellExecuteA(hDlg, "open", notification_more_info->url, NULL, NULL, SW_SHOWNORMAL);
				} else {
					MyDialogBox(hMainInstance, notification_more_info->id, hDlg, notification_more_info->callback);
				}
			}
			break;
		}
		break;
	}
	return (INT_PTR)FALSE;
}

/*
 * Display a custom notification
 */
BOOL Notification(int type, const char* dont_display_setting, const notification_info* more_info,  char* title, char* format, ...)
{
	BOOL ret;
	va_list args;

	dialog_showing++;
	szMessageText = (char*)malloc(LOC_MESSAGE_SIZE);
	if (szMessageText == NULL)
		return FALSE;
	szMessageTitle = safe_strdup(title);
	if (szMessageTitle == NULL)
		return FALSE;
	va_start(args, format);
	safe_vsnprintf(szMessageText, LOC_MESSAGE_SIZE - 1, format, args);
	va_end(args);
	szMessageText[LOC_MESSAGE_SIZE - 1] = 0;
	notification_more_info = more_info;
	notification_is_question = FALSE;
	notification_dont_display_setting = dont_display_setting;

	switch(type) {
	case MSG_WARNING_QUESTION:
		notification_is_question = TRUE;
		// Fall through
	case MSG_WARNING:
		hMessageIcon = LoadIcon(NULL, IDI_WARNING);
		break;
	case MSG_ERROR:
		hMessageIcon = LoadIcon(NULL, IDI_ERROR);
		break;
	case MSG_QUESTION:
		hMessageIcon = LoadIcon(NULL, IDI_QUESTION);
		notification_is_question = TRUE;
		break;
	case MSG_INFO:
	default:
		hMessageIcon = LoadIcon(NULL, IDI_INFORMATION);
		break;
	}
	ret = (MyDialogBox(hMainInstance, IDD_NOTIFICATION, hMainDialog, NotificationCallback) == IDYES);
	safe_free(szMessageText);
	safe_free(szMessageTitle);
	dialog_showing--;
	return ret;
}

// We only ever display one selection dialog, so set some params as globals
static int selection_dialog_style, selection_dialog_mask, selection_dialog_username_index;

/*
 * Custom dialog for radio button selection dialog
 */
static INT_PTR CALLBACK CustomSelectionCallback(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
	// This "Mooo" is designed to give us enough space for a regular username length
	static const char* base_username = "MOOOOOOOOOOO";	// 🐮
	// https://learn.microsoft.com/en-us/previous-versions/cc722458(v=technet.10)#user-name-policies
	static const char* username_invalid_chars = "/\\[]:;|=,+*?<>\"";
	// Prevent resizing
	static LRESULT disabled[9] = { HTLEFT, HTRIGHT, HTTOP, HTBOTTOM, HTSIZE,
		HTTOPLEFT, HTTOPRIGHT, HTBOTTOMLEFT, HTBOTTOMRIGHT };
	static HBRUSH background_brush, separator_brush;
	char username[128] = { 0 };
	int i, m, dw, dh, r = -1, mw;
	DWORD size = sizeof(username);
	LRESULT loc;
	// To use the system message font
	NONCLIENTMETRICS ncm;
	RECT rc, rc2;
	HFONT hDlgFont;
	HWND hCtrl;
	HDC hDC;

	switch (message) {
	case WM_INITDIALOG:
		// Don't overflow our max radio button
		if (nDialogItems > (IDC_SELECTION_CHOICEMAX - IDC_SELECTION_CHOICE1 + 1)) {
			uprintf("Warning: Too many options requested for Selection (%d vs %d)",
				nDialogItems, IDC_SELECTION_CHOICEMAX - IDC_SELECTION_CHOICE1);
			nDialogItems = IDC_SELECTION_CHOICEMAX - IDC_SELECTION_CHOICE1;
		}
		// Switch to checkboxes or some other style if requested
		for (i = 0; i < nDialogItems; i++)
			Button_SetStyle(GetDlgItem(hDlg, IDC_SELECTION_CHOICE1 + i), selection_dialog_style, TRUE);
		// Get the system message box font. See http://stackoverflow.com/a/6057761
		ncm.cbSize = sizeof(ncm);
		SystemParametersInfo(SPI_GETNONCLIENTMETRICS, ncm.cbSize, &ncm, 0);
		hDlgFont = CreateFontIndirect(&ncm.lfMessageFont);
		// Set the dialog to use the system message box font
		SendMessage(hDlg, WM_SETFONT, (WPARAM)hDlgFont, MAKELPARAM(TRUE, 0));
		SendMessage(GetDlgItem(hDlg, IDC_SELECTION_TEXT), WM_SETFONT, (WPARAM)hDlgFont, MAKELPARAM(TRUE, 0));
		for (i = 0; i < nDialogItems; i++)
			SendMessage(GetDlgItem(hDlg, IDC_SELECTION_CHOICE1 + i), WM_SETFONT, (WPARAM)hDlgFont, MAKELPARAM(TRUE, 0));
		SendMessage(GetDlgItem(hDlg, IDYES), WM_SETFONT, (WPARAM)hDlgFont, MAKELPARAM(TRUE, 0));
		SendMessage(GetDlgItem(hDlg, IDNO), WM_SETFONT, (WPARAM)hDlgFont, MAKELPARAM(TRUE, 0));

		apply_localization(IDD_SELECTION, hDlg);
		background_brush = CreateSolidBrush(GetSysColor(COLOR_WINDOW));
		separator_brush = CreateSolidBrush(GetSysColor(COLOR_3DLIGHT));
		SetTitleBarIcon(hDlg);
		CenterDialog(hDlg, NULL);

		// Get the dialog's default size
		GetWindowRect(GetDlgItem(hDlg, IDC_SELECTION_TEXT), &rc);
		MapWindowPoints(NULL, hDlg, (POINT*)&rc, 2);
		mw = rc.right - rc.left - ddw;	// ddw seems to work okay as a fudge
		dw = mw;

		// Change the default icon and set the text
		Static_SetIcon(GetDlgItem(hDlg, IDC_SELECTION_ICON), LoadIcon(NULL, IDI_QUESTION));
		SetWindowTextU(hDlg, szMessageTitle);
		SetWindowTextU(GetDlgItem(hDlg, IDCANCEL), lmprintf(MSG_007));
		SetWindowTextU(GetDlgItem(hDlg, IDC_SELECTION_TEXT), szMessageText);
		for (i = 0; i < nDialogItems; i++) {
			char *str = szDialogItem[i];
			SetWindowTextU(GetDlgItem(hDlg, IDC_SELECTION_CHOICE1 + i), str);
			ShowWindow(GetDlgItem(hDlg, IDC_SELECTION_CHOICE1 + i), SW_SHOW);
			// Compute the maximum line's width (with some extra for the username field if needed)
			if (i == selection_dialog_username_index) {
				str = calloc(strlen(szDialogItem[i]) + strlen(base_username) + 8, 1);
				sprintf(str, "%s __%s__", szDialogItem[i], base_username);
			}
			mw = max(mw, GetTextSize(GetDlgItem(hDlg, IDC_SELECTION_CHOICE1 + i), str).cx);
			if (i == selection_dialog_username_index)
				free(str);
		}
		// If our maximum line's width is greater than the default, set a nonzero delta width
		dw = (mw <= dw) ? 0: mw - dw;
		// Move/Resize the controls as needed to fit our text
		hCtrl = GetDlgItem(hDlg, IDC_SELECTION_TEXT);
		ResizeMoveCtrl(hDlg, hCtrl, 0, 0, dw, 0, 1.0f);
		hDC = GetDC(hCtrl);
		SelectFont(hDC, hDlgFont);	// Yes, you *MUST* reapply the font to the DC, even after SetWindowText!
		GetWindowRect(hCtrl, &rc);
		dh = rc.bottom - rc.top;
		DrawTextU(hDC, szMessageText, -1, &rc, DT_CALCRECT | DT_WORDBREAK);
		dh = rc.bottom - rc.top - dh;
		safe_release_dc(hCtrl, hDC);
		ResizeMoveCtrl(hDlg, hCtrl, 0, 0, 0, dh, 1.0f);
		for (i = 0; i < nDialogItems; i++)
			ResizeMoveCtrl(hDlg, GetDlgItem(hDlg, IDC_SELECTION_CHOICE1 + i), 0, dh, dw, 0, 1.0f);

		// If required, set up the the username edit box
		if (selection_dialog_username_index != -1) {
			unattend_username[0] = 0;
			hCtrl = GetDlgItem(hDlg, IDC_SELECTION_CHOICE1 + selection_dialog_username_index);
			GetClientRect(hCtrl, &rc);
			ResizeMoveCtrl(hDlg, hCtrl, 0, 0,
				(rc.left - rc.right) + GetTextSize(hCtrl, szDialogItem[selection_dialog_username_index]).cx + ddw, 0, 1.0f);
			GetWindowRect(hCtrl, &rc);
			SetWindowPos(GetDlgItem(hDlg, IDC_SELECTION_USERNAME), hCtrl, rc.left, rc.top, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
			hCtrl = GetDlgItem(hDlg, IDC_SELECTION_USERNAME);
			GetWindowRect(hCtrl, &rc2);
			ResizeMoveCtrl(hDlg, hCtrl, right_to_left_mode ? rc2.right - rc.left : rc.right - rc2.left, rc.top - rc2.top,
				GetTextSize(hCtrl, (char*)base_username).cx, 0, 1.0f);
			if (!GetUserNameU(username, &size) || username[0] == 0)
				static_strcpy(username, "User");
			SetWindowTextU(hCtrl, username);
			ShowWindow(hCtrl, SW_SHOW);
		}

		if (nDialogItems > 2) {
			GetWindowRect(GetDlgItem(hDlg, IDC_SELECTION_CHOICE2), &rc);
			GetWindowRect(GetDlgItem(hDlg, IDC_SELECTION_CHOICE1 + nDialogItems - 1), &rc2);
			dh += rc2.top - rc.top;
		}
		if (dw != 0)
			dw += ddw;
		ResizeMoveCtrl(hDlg, hDlg, 0, 0, dw, dh, 1.0f);
		ResizeMoveCtrl(hDlg, GetDlgItem(hDlg, -1), 0, 0, dw, dh, 1.0f);	// IDC_STATIC = -1
		ResizeMoveCtrl(hDlg, GetDlgItem(hDlg, IDC_SELECTION_LINE), 0, dh, dw, 0, 1.0f);
		ResizeMoveCtrl(hDlg, GetDlgItem(hDlg, IDOK), dw, dh, 0, 0, 1.0f);
		ResizeMoveCtrl(hDlg, GetDlgItem(hDlg, IDCANCEL), dw, dh, 0, 0, 1.0f);
		ResizeButtonHeight(hDlg, IDOK);
		ResizeButtonHeight(hDlg, IDCANCEL);

		// Set the default selection
		for (i = 0, m = 1; i < nDialogItems; i++, m <<= 1)
			Button_SetCheck(GetDlgItem(hDlg, IDC_SELECTION_CHOICE1 + i), (m & selection_dialog_mask) ? BST_CHECKED : BST_UNCHECKED);
		return (INT_PTR)TRUE;
	case WM_CTLCOLORSTATIC:
		// Change the background colour for static text and icon
		SetBkMode((HDC)wParam, TRANSPARENT);
		if ((HWND)lParam == GetDlgItem(hDlg, IDC_NOTIFICATION_LINE)) {
			return (INT_PTR)separator_brush;
		}
		return (INT_PTR)background_brush;
	case WM_NCHITTEST:
		// Check coordinates to prevent resize actions
		loc = DefWindowProc(hDlg, message, wParam, lParam);
		for (i = 0; i < 9; i++) {
			if (loc == disabled[i]) {
				return (INT_PTR)TRUE;
			}
		}
		return (INT_PTR)FALSE;
	case WM_COMMAND:
		switch (LOWORD(wParam)) {
		case IDOK:
			for (r = 0, i = 0, m = 1; i < nDialogItems; i++, m <<= 1)
				if (Button_GetCheck(GetDlgItem(hDlg, IDC_SELECTION_CHOICE1 + i)) == BST_CHECKED)
					r += m;
			if (selection_dialog_username_index != -1) {
				GetWindowTextU(GetDlgItem(hDlg, IDC_SELECTION_USERNAME), unattend_username, MAX_USERNAME_LENGTH);
				// Perform string sanitization (NB: GetWindowTextU always terminates the string)
				for (i = 0; unattend_username[i] != 0; i++) {
					if (strchr(username_invalid_chars, unattend_username[i]) != NULL)
						unattend_username[i] = '_';
				}
			}
			// Fall through
		case IDNO:
		case IDCANCEL:
			EndDialog(hDlg, r);
			return (INT_PTR)TRUE;
		}
		break;
	}
	return (INT_PTR)FALSE;
}

/*
 * Display an item selection dialog
 */
int CustomSelectionDialog(int style, char* title, char* message, char** choices, int size, int mask, int username_index)
{
	int ret;

	dialog_showing++;
	szMessageTitle = title;
	szMessageText = message;
	szDialogItem = choices;
	nDialogItems = size;
	selection_dialog_style = style;
	selection_dialog_mask = mask;
	selection_dialog_username_index = username_index;
	assert(selection_dialog_style == BS_AUTORADIOBUTTON || selection_dialog_style == BS_AUTOCHECKBOX);
	ret = (int)MyDialogBox(hMainInstance, IDD_SELECTION, hMainDialog, CustomSelectionCallback);
	dialog_showing--;

	return ret;
}

/*
 * Custom dialog for list dialog
 */
INT_PTR CALLBACK ListCallback(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
	LRESULT loc;
	int i, dh, r  = -1;
	// Prevent resizing
	static LRESULT disabled[9] = { HTLEFT, HTRIGHT, HTTOP, HTBOTTOM, HTSIZE,
		HTTOPLEFT, HTTOPRIGHT, HTBOTTOMLEFT, HTBOTTOMRIGHT };
	static HBRUSH background_brush, separator_brush;
	// To use the system message font
	NONCLIENTMETRICS ncm;
	RECT rc, rc2;
	HFONT hDlgFont;
	HWND hCtrl;
	HDC hDC;

	switch (message) {
	case WM_INITDIALOG:
		// Don't overflow our max radio button
		if (nDialogItems > (IDC_LIST_ITEMMAX - IDC_LIST_ITEM1 + 1)) {
			uprintf("Warning: Too many items requested for List (%d vs %d)",
				nDialogItems, IDC_LIST_ITEMMAX - IDC_LIST_ITEM1);
			nDialogItems = IDC_LIST_ITEMMAX - IDC_LIST_ITEM1;
		}
		// Get the system message box font. See http://stackoverflow.com/a/6057761
		ncm.cbSize = sizeof(ncm);
		SystemParametersInfo(SPI_GETNONCLIENTMETRICS, ncm.cbSize, &ncm, 0);
		hDlgFont = CreateFontIndirect(&(ncm.lfMessageFont));
		// Set the dialog to use the system message box font
		SendMessage(hDlg, WM_SETFONT, (WPARAM)hDlgFont, MAKELPARAM(TRUE, 0));
		SendMessage(GetDlgItem(hDlg, IDC_LIST_TEXT), WM_SETFONT, (WPARAM)hDlgFont, MAKELPARAM(TRUE, 0));
		for (i = 0; i < nDialogItems; i++)
			SendMessage(GetDlgItem(hDlg, IDC_LIST_ITEM1 + i), WM_SETFONT, (WPARAM)hDlgFont, MAKELPARAM(TRUE, 0));
		SendMessage(GetDlgItem(hDlg, IDYES), WM_SETFONT, (WPARAM)hDlgFont, MAKELPARAM(TRUE, 0));
		SendMessage(GetDlgItem(hDlg, IDNO), WM_SETFONT, (WPARAM)hDlgFont, MAKELPARAM(TRUE, 0));

		apply_localization(IDD_LIST, hDlg);
		background_brush = CreateSolidBrush(GetSysColor(COLOR_WINDOW));
		separator_brush = CreateSolidBrush(GetSysColor(COLOR_3DLIGHT));
		SetTitleBarIcon(hDlg);
		CenterDialog(hDlg, NULL);
		// Change the default icon and set the text
		Static_SetIcon(GetDlgItem(hDlg, IDC_LIST_ICON), LoadIcon(NULL, IDI_EXCLAMATION));
		SetWindowTextU(hDlg, szMessageTitle);
		SetWindowTextU(GetDlgItem(hDlg, IDCANCEL), lmprintf(MSG_007));
		SetWindowTextU(GetDlgItem(hDlg, IDC_LIST_TEXT), szMessageText);
		for (i = 0; i < nDialogItems; i++) {
			SetWindowTextU(GetDlgItem(hDlg, IDC_LIST_ITEM1 + i), szDialogItem[i]);
			ShowWindow(GetDlgItem(hDlg, IDC_LIST_ITEM1 + i), SW_SHOW);
		}
		// Move/Resize the controls as needed to fit our text
		hCtrl = GetDlgItem(hDlg, IDC_LIST_TEXT);
		hDC = GetDC(hCtrl);
		SelectFont(hDC, hDlgFont);	// Yes, you *MUST* reapply the font to the DC, even after SetWindowText!
		GetWindowRect(hCtrl, &rc);
		dh = rc.bottom - rc.top;
		DrawTextU(hDC, szMessageText, -1, &rc, DT_CALCRECT | DT_WORDBREAK);
		dh = rc.bottom - rc.top - dh;
		safe_release_dc(hCtrl, hDC);
		ResizeMoveCtrl(hDlg, hCtrl, 0, 0, 0, dh, 1.0f);
		for (i = 0; i < nDialogItems; i++)
			ResizeMoveCtrl(hDlg, GetDlgItem(hDlg, IDC_LIST_ITEM1 + i), 0, dh, 0, 0, 1.0f);
		if (nDialogItems > 1) {
			GetWindowRect(GetDlgItem(hDlg, IDC_LIST_ITEM1), &rc);
			GetWindowRect(GetDlgItem(hDlg, IDC_LIST_ITEM1 + nDialogItems - 1), &rc2);
			dh += rc2.top - rc.top;
		}
		ResizeMoveCtrl(hDlg, hDlg, 0, 0, 0, dh, 1.0f);
		ResizeMoveCtrl(hDlg, GetDlgItem(hDlg, -1), 0, 0, 0, dh, 1.0f);	// IDC_STATIC = -1
		ResizeMoveCtrl(hDlg, GetDlgItem(hDlg, IDC_LIST_LINE), 0, dh, 0, 0, 1.0f);
		ResizeMoveCtrl(hDlg, GetDlgItem(hDlg, IDOK), 0, dh, 0, 0, 1.0f);
		ResizeMoveCtrl(hDlg, GetDlgItem(hDlg, IDCANCEL), 0, dh, 0, 0, 1.0f);
		ResizeButtonHeight(hDlg, IDOK);
		ResizeButtonHeight(hDlg, IDCANCEL);
		return (INT_PTR)TRUE;
	case WM_CTLCOLORSTATIC:
		// Change the background colour for static text and icon
		SetBkMode((HDC)wParam, TRANSPARENT);
		if ((HWND)lParam == GetDlgItem(hDlg, IDC_NOTIFICATION_LINE)) {
			return (INT_PTR)separator_brush;
		}
		return (INT_PTR)background_brush;
	case WM_NCHITTEST:
		// Check coordinates to prevent resize actions
		loc = DefWindowProc(hDlg, message, wParam, lParam);
		for (i = 0; i < 9; i++) {
			if (loc == disabled[i]) {
				return (INT_PTR)TRUE;
			}
		}
		return (INT_PTR)FALSE;
	case WM_COMMAND:
		switch (LOWORD(wParam)) {
		case IDOK:
		case IDNO:
		case IDCANCEL:
			EndDialog(hDlg, r);
			return (INT_PTR)TRUE;
		}
		break;
	}
	return (INT_PTR)FALSE;
}

/*
 * Display a dialog with a list of items
 */
void ListDialog(char* title, char* message, char** items, int size)
{
	dialog_showing++;
	szMessageTitle = title;
	szMessageText = message;
	szDialogItem = items;
	nDialogItems = size;
	MyDialogBox(hMainInstance, IDD_LIST, hMainDialog, ListCallback);
	dialog_showing--;
}

static struct {
	HWND hTip;		// Tooltip handle
	HWND hCtrl;		// Handle of the control the tooltip belongs to
	WNDPROC original_proc;
	LPWSTR wstring;
} ttlist[MAX_TOOLTIPS] = { {0} };

INT_PTR CALLBACK TooltipCallback(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
	LPNMTTDISPINFOW lpnmtdi;
	int i = MAX_TOOLTIPS;

	// Make sure we have an original proc
	for (i=0; i<MAX_TOOLTIPS; i++) {
		if (ttlist[i].hTip == hDlg) break;
	}
	if (i == MAX_TOOLTIPS)
		return (INT_PTR)FALSE;

	switch (message) {
	case WM_NOTIFY:
		switch (((LPNMHDR)lParam)->code) {
		case TTN_GETDISPINFOW:
			lpnmtdi = (LPNMTTDISPINFOW)lParam;
			lpnmtdi->lpszText = ttlist[i].wstring;
			// Don't ask me WHY we need to clear RTLREADING for RTL multiline text to look good
			lpnmtdi->uFlags &= ~TTF_RTLREADING;
			SendMessage(hDlg, TTM_SETMAXTIPWIDTH, 0, (LPARAM)(int)(150.0f * fScale));
			return (INT_PTR)TRUE;
		}
		break;
	}
#ifdef _DEBUG
	// comctl32 causes issues if the tooltips are not being manipulated from the same thread as their parent controls
	if (GetCurrentThreadId() != MainThreadId)
		uprintf("Warning: Tooltip callback is being called from wrong thread");
#endif
	return CallWindowProc(ttlist[i].original_proc, hDlg, message, wParam, lParam);
}

/*
 * Create a tooltip for the control passed as first parameter
 * duration sets the duration in ms. Use -1 for default
 * message is an UTF-8 string
 */
BOOL CreateTooltip(HWND hControl, const char* message, int duration)
{
	TOOLINFOW toolInfo = {0};
	int i;

	if ( (hControl == NULL) || (message == NULL) ) {
		return FALSE;
	}

	// Destroy existing tooltip if any
	DestroyTooltip(hControl);

	// Find an empty slot
	for (i=0; i<MAX_TOOLTIPS; i++) {
		if (ttlist[i].hTip == NULL) break;
	}
	if (i >= MAX_TOOLTIPS) {
		uprintf("Maximum number of tooltips reached (%d)\n", MAX_TOOLTIPS);
		return FALSE;
	}

	// Create the tooltip window
	ttlist[i].hTip = CreateWindowEx(right_to_left_mode ? WS_EX_LAYOUTRTL : 0,
		TOOLTIPS_CLASS, NULL, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
		CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, hMainDialog, NULL,
		hMainInstance, NULL);

	if (ttlist[i].hTip == NULL) {
		return FALSE;
	}
	ttlist[i].hCtrl = hControl;

	// Subclass the tooltip to handle multiline
	ttlist[i].original_proc = (WNDPROC)SetWindowLongPtr(ttlist[i].hTip, GWLP_WNDPROC, (LONG_PTR)TooltipCallback);

	// Set the string to display (can be multiline)
	ttlist[i].wstring = utf8_to_wchar(message);

	// Set tooltip duration (ms)
	PostMessage(ttlist[i].hTip, TTM_SETDELAYTIME, (WPARAM)TTDT_AUTOPOP, (LPARAM)duration);

	// Associate the tooltip to the control
	toolInfo.cbSize = sizeof(toolInfo);
	toolInfo.hwnd = ttlist[i].hTip;	// Set to the tooltip itself to ease up subclassing
	toolInfo.uFlags = TTF_IDISHWND | TTF_SUBCLASS | ((right_to_left_mode)?TTF_RTLREADING:0);
	// set TTF_NOTBUTTON and TTF_CENTERTIP if it isn't a button
	if (!(SendMessage(hControl, WM_GETDLGCODE, 0, 0) & DLGC_BUTTON))
		toolInfo.uFlags |= 0x80000000L | TTF_CENTERTIP;

	toolInfo.uId = (UINT_PTR)hControl;
	toolInfo.lpszText = LPSTR_TEXTCALLBACKW;
	SendMessageW(ttlist[i].hTip, TTM_ADDTOOLW, 0, (LPARAM)&toolInfo);

	return TRUE;
}

/* Destroy a tooltip. hCtrl = handle of the control the tooltip is associated with */
void DestroyTooltip(HWND hControl)
{
	int i;

	if (hControl == NULL) return;
	for (i=0; i<MAX_TOOLTIPS; i++) {
		if (ttlist[i].hCtrl == hControl) break;
	}
	if (i >= MAX_TOOLTIPS) return;
	DestroyWindow(ttlist[i].hTip);
	safe_free(ttlist[i].wstring);
	ttlist[i].original_proc = NULL;
	ttlist[i].hTip = NULL;
	ttlist[i].hCtrl = NULL;
}

void DestroyAllTooltips(void)
{
	int i, j;

	for (i=0, j=0; i<MAX_TOOLTIPS; i++) {
		if (ttlist[i].hTip == NULL) continue;
		j++;
		DestroyWindow(ttlist[i].hTip);
		safe_free(ttlist[i].wstring);
		ttlist[i].original_proc = NULL;
		ttlist[i].hTip = NULL;
		ttlist[i].hCtrl = NULL;
	}
}

/* Determine if a Windows is being displayed or not */
BOOL IsShown(HWND hDlg)
{
	WINDOWPLACEMENT placement = {0};
	placement.length = sizeof(WINDOWPLACEMENT);
	if (!GetWindowPlacement(hDlg, &placement))
		return FALSE;
	switch (placement.showCmd) {
	case SW_SHOWNORMAL:
	case SW_SHOWMAXIMIZED:
	case SW_SHOW:
	case SW_SHOWDEFAULT:
		return TRUE;
	default:
		return FALSE;
	}
}

/* Compute the width of a dropdown list entry */
LONG GetEntryWidth(HWND hDropDown, const char *entry)
{
	HDC hDC;
	HFONT hFont, hDefFont = NULL;
	SIZE size;

	hDC = GetDC(hDropDown);
	hFont = (HFONT)SendMessage(hDropDown, WM_GETFONT, 0, 0);
	if (hFont != NULL)
		hDefFont = (HFONT)SelectObject(hDC, hFont);

	if (!GetTextExtentPointU(hDC, entry, &size))
		size.cx = 0;

	if (hFont != NULL)
		SelectObject(hDC, hDefFont);

	safe_release_dc(hDropDown, hDC);
	return size.cx;
}

/*
 * Windows taskbar icon handling (progress bar overlay, etc)
 */
static ITaskbarList3* ptbl = NULL;

// Create a taskbar icon progressbar
BOOL CreateTaskbarList(void)
{
	HRESULT hr;

	hr = CoCreateInstance(&CLSID_TaskbarList, NULL, CLSCTX_ALL, &IID_ITaskbarList3, (LPVOID)&ptbl);
	if (FAILED(hr)) {
		uprintf("CoCreateInstance for TaskbarList failed: error %X\n", hr);
		ptbl = NULL;
		return FALSE;
	}
	return TRUE;
}

BOOL SetTaskbarProgressState(TASKBAR_PROGRESS_FLAGS tbpFlags)
{
	if (ptbl == NULL)
		return FALSE;
	return !FAILED(ITaskbarList3_SetProgressState(ptbl, hMainDialog, tbpFlags));
}

BOOL SetTaskbarProgressValue(ULONGLONG ullCompleted, ULONGLONG ullTotal)
{
	if (ptbl == NULL)
		return FALSE;
	return !FAILED(ITaskbarList3_SetProgressValue(ptbl, hMainDialog, ullCompleted, ullTotal));
}

/*
 * Use a thread to enable the download button as this may be a lengthy
 * operation due to the external download check.
 */
static DWORD WINAPI CheckForFidoThread(LPVOID param)
{
	static BOOL is_active = FALSE;
	LONG_PTR style;
	char* loc = NULL;
	HWND hCtrl;

	// Because a user may switch language before this thread has completed,
	// we need to detect concurrency.
	// Checking on a static boolean is more than good enough for our purpose.
	if (is_active)
		return -1;
	is_active = TRUE;
	fido_url = WHITEBAR_URL;
	if (safe_strncmp(fido_url, "https://github.com/Smu1zel/Whitebar", 35) != 0) {
		uprintf("WARNING: Download script URL %s is invalid ✗", fido_url);
		safe_free(fido_url);
		goto out;
	}
	if (IsDownloadable(fido_url)) {
		hCtrl = GetDlgItem(hMainDialog, IDC_SELECT);
		style = GetWindowLongPtr(hCtrl, GWL_STYLE);
		style |= BS_SPLITBUTTON;
		SetWindowLongPtr(hCtrl, GWL_STYLE, style);
		RedrawWindow(hCtrl, NULL, NULL, RDW_ALLCHILDREN | RDW_UPDATENOW);
		InvalidateRect(hCtrl, NULL, TRUE);
	}

out:
	safe_free(loc);
	is_active = FALSE;
	return 0;
}

void SetFidoCheck(void)
{
	// Detect if we can use Fido, which depends on:
	// - Powershell being installed
	// - Rufus running in AppStore mode or update check being enabled
	// - URL for the script being reachable
	if ((ReadRegistryKey32(REGKEY_HKLM, "Software\\Microsoft\\PowerShell\\1\\Install") <= 0) &&
		(ReadRegistryKey32(REGKEY_HKLM, "Software\\Microsoft\\PowerShell\\3\\Install") <= 0)) {
		ubprintf("Notice: The ISO download feature has been deactivated because "
			"a compatible PowerShell version was not detected on this system.");
		return;
	}

	CreateThread(NULL, 0, CheckForFidoThread, NULL, 0, NULL);
}
void CreateStaticFont(HDC hDC, HFONT* hFont, BOOL underlined)
{
	TEXTMETRIC tm;
	LOGFONT lf;

	if (*hFont != NULL)
		return;
	GetTextMetrics(hDC, &tm);
	lf.lfHeight = tm.tmHeight;
	lf.lfWidth = 0;
	lf.lfEscapement = 0;
	lf.lfOrientation = 0;
	lf.lfWeight = tm.tmWeight;
	lf.lfItalic = tm.tmItalic;
	lf.lfUnderline = underlined;
	lf.lfStrikeOut = tm.tmStruckOut;
	lf.lfCharSet = tm.tmCharSet;
	lf.lfOutPrecision = OUT_DEFAULT_PRECIS;
	lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
	lf.lfQuality = DEFAULT_QUALITY;
	lf.lfPitchAndFamily = tm.tmPitchAndFamily;
	GetTextFace(hDC, LF_FACESIZE, lf.lfFaceName);
	*hFont = CreateFontIndirect(&lf);
}

/*
 * Work around the limitations of edit control, to display a hand cursor for hyperlinks
 * NB: The LTEXT control must have SS_NOTIFY attribute for this to work
 */
INT_PTR CALLBACK update_subclass_callback(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_SETCURSOR:
		if ((HWND)wParam == GetDlgItem(hDlg, IDC_WEBSITE)) {
			SetCursor(LoadCursor(NULL, IDC_HAND));
			return (INT_PTR)TRUE;
		}
		break;
	}
	return CallWindowProc(update_original_proc, hDlg, message, wParam, lParam);
}

void SetTitleBarIcon(HWND hDlg)
{
	int i16, s16, s32;
	HICON hSmallIcon, hBigIcon;

	// High DPI scaling
	i16 = GetSystemMetrics(SM_CXSMICON);
	// Adjust icon size lookup
	s16 = i16;
	s32 = (int)(32.0f*fScale);
	if (s16 >= 54)
		s16 = 64;
	else if (s16 >= 40)
		s16 = 48;
	else if (s16 >= 28)
		s16 = 32;
	else if (s16 >= 20)
		s16 = 24;
	if (s32 >= 54)
		s32 = 64;
	else if (s32 >= 40)
		s32 = 48;
	else if (s32 >= 28)
		s32 = 32;
	else if (s32 >= 20)
		s32 = 24;

	// Create the title bar icon
	hSmallIcon = (HICON)LoadImage(hMainInstance, MAKEINTRESOURCE(IDI_ICON), IMAGE_ICON, s16, s16, 0);
	SendMessage (hDlg, WM_SETICON, ICON_SMALL, (LPARAM)hSmallIcon);
	hBigIcon = (HICON)LoadImage(hMainInstance, MAKEINTRESOURCE(IDI_ICON), IMAGE_ICON, s32, s32, 0);
	SendMessage (hDlg, WM_SETICON, ICON_BIG, (LPARAM)hBigIcon);
}

// Return the onscreen size of the text displayed by a control
SIZE GetTextSize(HWND hCtrl, char* txt)
{
	SIZE sz = {0, 0};
	HDC hDC;
	wchar_t *wstr = NULL;
	int len;
	HFONT hFont;

	// Compute the size of the text
	hDC = GetDC(hCtrl);
	if (hDC == NULL)
		goto out;
	hFont = (HFONT)SendMessageA(hCtrl, WM_GETFONT, 0, 0);
	if (hFont == NULL)
		goto out;
	SelectObject(hDC, hFont);
	if (txt == NULL) {
		len = GetWindowTextLengthW(hCtrl);
		if (len <= 0)
			goto out;
		wstr = calloc(len + 1, sizeof(wchar_t));
		if (wstr == NULL)
			goto out;
		if (GetWindowTextW(hCtrl, wstr, len + 1) > 0)
			GetTextExtentPoint32W(hDC, wstr, len, &sz);
	} else {
		wstr = utf8_to_wchar(txt);
		if (wstr != NULL)
			GetTextExtentPoint32W(hDC, wstr, (int)wcslen(wstr), &sz);
	}
out:
	safe_free(wstr);
	safe_release_dc(hCtrl, hDC);
	return sz;
}

/*
 * The following is used to work around dialog template limitations when switching from LTR to RTL
 * or switching the font. This avoids having to multiply similar templates in the RC.
 * TODO: Can we use http://stackoverflow.com/questions/6057239/which-font-is-the-default-for-mfc-dialog-controls?
  */

// Produce a dialog template from our RC, and update its RTL and Font settings dynamically
// See http://blogs.msdn.com/b/oldnewthing/archive/2004/06/21/163596.aspx as well as
// https://msdn.microsoft.com/en-us/library/windows/desktop/ms645389.aspx for a description
// of the Dialog structure
LPCDLGTEMPLATE GetDialogTemplate(int Dialog_ID)
{
	int i;
	const char thai_id[] = "th-TH";
	size_t len;
	DWORD size = 0;
	DWORD* dwBuf;
	WCHAR* wBuf;
	LPCDLGTEMPLATE rcTemplate = (LPCDLGTEMPLATE) GetResource(hMainInstance, MAKEINTRESOURCEA(Dialog_ID),
		_RT_DIALOG, get_name_from_id(Dialog_ID), &size, TRUE);

	if ((size == 0) || (rcTemplate == NULL)) {
		safe_free(rcTemplate);
		return NULL;
	}
	if (right_to_left_mode) {
		// Add the RTL styles into our RC copy, so that we don't have to multiply dialog definitions in the RC
		dwBuf = (DWORD*)rcTemplate;
		dwBuf[2] = WS_EX_APPWINDOW | WS_EX_LAYOUTRTL;
	}

	// All our dialogs are set to use 'Segoe UI Symbol' by default:
	// 1. So that we can replace the font name with 'Segoe UI'
	// 2. So that Thai displays properly on RTF controls as it won't work with regular
	// 'Segoe UI'... but Cyrillic won't work with 'Segoe UI Symbol'

	// If 'Segoe UI Symbol' is available, and we are using Thai, we're done here
	if (IsFontAvailable("Segoe UI Symbol") && (selected_locale != NULL)
		&& (safe_strcmp(selected_locale->txt[0], thai_id) == 0))
		return rcTemplate;

	// 'Segoe UI Symbol' cannot be used => Fall back to the best we have
	wBuf = (WCHAR*)rcTemplate;
	wBuf = &wBuf[14];	// Move to class name
	// Skip class name and title
	for (i = 0; i<2; i++) {
		if (*wBuf == 0xFFFF)
			wBuf = &wBuf[2];	// Ordinal
		else
			wBuf = &wBuf[wcslen(wBuf) + 1]; // String
	}
	// NB: to change the font size to 9, you can use
	// wBuf[0] = 0x0009;
	wBuf = &wBuf[3];
	// Make sure we are where we want to be and adjust the font
	if (wcscmp(L"Segoe UI Symbol", wBuf) == 0) {
		uintptr_t src, dst, start = (uintptr_t)rcTemplate;
		// We can't simply zero the characters we don't want, as the size of the font
		// string determines the next item lookup. So we must memmove the remaining of
		// our buffer. Oh, and those items are DWORD aligned.
		// 'Segoe UI Symbol' -> 'Segoe UI'
		wBuf[8] = 0;
		len = wcslen(wBuf);
		wBuf[len + 1] = 0;
		dst = (uintptr_t)&wBuf[len + 2];
		dst &= ~3;
		src = (uintptr_t)&wBuf[17];
		src &= ~3;
		memmove((void*)dst, (void*)src, size - (src - start));
	} else {
		uprintf("Could not locate font for %s!", get_name_from_id(Dialog_ID));
	}
	return rcTemplate;
}

HWND MyCreateDialog(HINSTANCE hInstance, int Dialog_ID, HWND hWndParent, DLGPROC lpDialogFunc)
{
	LPCDLGTEMPLATE rcTemplate = GetDialogTemplate(Dialog_ID);
	assert(rcTemplate != NULL);
	HWND hDlg = CreateDialogIndirect(hInstance, rcTemplate, hWndParent, lpDialogFunc);
	safe_free(rcTemplate);
	return hDlg;
}

INT_PTR MyDialogBox(HINSTANCE hInstance, int Dialog_ID, HWND hWndParent, DLGPROC lpDialogFunc)
{
	INT_PTR ret;
	LPCDLGTEMPLATE rcTemplate = GetDialogTemplate(Dialog_ID);

	// A DialogBox doesn't handle reduce/restore so it won't pass restore messages to the
	// main dialog if the main dialog was minimized. This can result in situations where the
	// user cannot restore the main window if a new dialog prompt was triggered while the
	// main dialog was reduced => Ensure the main dialog is visible before we display anything.
	ShowWindow(hMainDialog, SW_NORMAL);

	assert(rcTemplate != NULL);
	ret = DialogBoxIndirect(hMainInstance, rcTemplate, hWndParent, lpDialogFunc);
	safe_free(rcTemplate);
	return ret;
}

/*
 * The following function calls are used to automatically detect and close the native
 * Windows format prompt "You must format the disk in drive X:". To do that, we use
 * an event hook that gets triggered whenever a window is placed in the foreground.
 * In that hook, we look for a dialog that has style WS_POPUPWINDOW and has the relevant
 * title. However, because the title in itself is too generic (the expectation is that
 * it will be "Microsoft Windows") we also enumerate all the child controls from that
 * prompt, using another callback, until we find one that contains the text we expect
 * for the "Format disk" button.
 * Oh, and since all of these strings are localized, we must first pick them up from
 * the relevant mui's.
 */
static BOOL CALLBACK AlertPromptCallback(HWND hWnd, LPARAM lParam)
{
	char str[128];
	BOOL *found = (BOOL*)lParam;

	if (GetWindowTextU(hWnd, str, sizeof(str)) == 0)
		return TRUE;
	if (strcmp(str, button_str) == 0)
		*found = TRUE;
	return TRUE;
}

static void CALLBACK AlertPromptHook(HWINEVENTHOOK hWinEventHook, DWORD Event, HWND hWnd, LONG idObject, LONG idChild, DWORD dwEventThread, DWORD dwmsEventTime)
{
	char str[128];
	BOOL found;

	if (Event == EVENT_SYSTEM_FOREGROUND) {
		if (GetWindowLongPtr(hWnd, GWL_STYLE) & WS_POPUPWINDOW) {
			str[0] = 0;
			GetWindowTextU(hWnd, str, sizeof(str));
			if (strcmp(str, title_str[0]) == 0) {
				found = FALSE;
				EnumChildWindows(hWnd, AlertPromptCallback, (LPARAM)&found);
				if (found) {
					SendMessage(hWnd, WM_COMMAND, (WPARAM)IDCANCEL, (LPARAM)0);
					uprintf("Closed Windows format prompt");
				}
			} else if ((strcmp(str, title_str[1]) == 0) && (hWnd != hFidoDlg)) {
				// A wild Fido dialog appeared! => Keep track of its handle and center it
				hFidoDlg = hWnd;
				CenterDialog(hWnd, hMainDialog);
			}
		}
	}
}

void SetAlertPromptMessages(void)
{
	HMODULE hMui;
	char mui_path[MAX_PATH];

	// Fetch the localized strings in the relevant MUI
	// Must use sysnative_dir rather than system_dir as we may not find the MUI's otherwise
	// Also don't bother with LibLibraryEx() since we have a full path here.
	static_sprintf(mui_path, "%s\\%s\\shell32.dll.mui", sysnative_dir, ToLocaleName(GetUserDefaultUILanguage()));
	hMui = LoadLibraryU(mui_path);
	if (hMui != NULL) {
		// 4097 = "You need to format the disk in drive %c: before you can use it." (dialog text)
		// 4125 = "Microsoft Windows" (dialog title)
		// 4126 = "Format disk" (button)
		if (LoadStringU(hMui, 4125, title_str[0], sizeof(title_str[0])) <= 0) {
			static_strcpy(title_str[0], "Microsoft Windows");
			uprintf("Warning: Could not locate localized format prompt title string in '%s': %s", mui_path, WindowsErrorString());
		}
		if (LoadStringU(hMui, 4126, button_str, sizeof(button_str)) <= 0) {
			static_strcpy(button_str, "Format disk");
			uprintf("Warning: Could not locate localized format prompt button string in '%s': %s", mui_path, WindowsErrorString());
		}
		FreeLibrary(hMui);
	}
	static_strcpy(title_str[1], lmprintf(MSG_149));
}

BOOL SetAlertPromptHook(void)
{
	if (ap_weh != NULL)
		return TRUE;	// No need to set again if active
	ap_weh = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, NULL,
		AlertPromptHook, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
	return (ap_weh != NULL);
}

void ClrAlertPromptHook(void) {
	UnhookWinEvent(ap_weh);
	ap_weh = NULL;
}

void FlashTaskbar(HANDLE handle)
{
	FLASHWINFO pf;

	if (handle == NULL)
		return;
	pf.cbSize = sizeof(FLASHWINFO);
	pf.hwnd = handle;
	// Could also use FLASHW_ALL to flash the main dialog)
	pf.dwFlags = FLASHW_TIMER | FLASHW_TRAY;
	pf.uCount = 5;
	pf.dwTimeout = 75;
	FlashWindowEx(&pf);
}

// https://docs.microsoft.com/en-us/globalization/localizability/mirroring-in-win32
// Note: This function *destroys* the original icon
HICON CreateMirroredIcon(HICON hiconOrg)
{
	HDC hdcScreen, hdcBitmap, hdcMask = NULL;
	HBITMAP hbm, hbmMask, hbmOld, hbmOldMask;
	BITMAP bm;
	ICONINFO ii;
	HICON hicon = NULL;
	hdcBitmap = CreateCompatibleDC(NULL);
	if (hdcBitmap) {
		hdcMask = CreateCompatibleDC(NULL);
		if (hdcMask) {
			SetLayout(hdcBitmap, LAYOUT_RTL);
			SetLayout(hdcMask, LAYOUT_RTL);
		} else {
			DeleteDC(hdcBitmap);
			hdcBitmap = NULL;
		}
	}
	hdcScreen = GetDC(NULL);
	if (hdcScreen) {
		if (hdcBitmap && hdcMask) {
			if (hiconOrg) {
				if (GetIconInfo(hiconOrg, &ii) && GetObject(ii.hbmColor, sizeof(BITMAP), &bm)) {
					// Do the cleanup for the bitmaps.
					DeleteObject(ii.hbmMask);
					DeleteObject(ii.hbmColor);
					ii.hbmMask = ii.hbmColor = NULL;
					hbm = CreateCompatibleBitmap(hdcScreen, bm.bmWidth, bm.bmHeight);
					hbmMask = CreateBitmap(bm.bmWidth, bm.bmHeight, 1, 1, NULL);
					hbmOld = (HBITMAP)SelectObject(hdcBitmap, hbm);
					hbmOldMask = (HBITMAP)SelectObject(hdcMask, hbmMask);
					DrawIconEx(hdcBitmap, 0, 0, hiconOrg, bm.bmWidth, bm.bmHeight, 0, NULL, DI_IMAGE);
					DrawIconEx(hdcMask, 0, 0, hiconOrg, bm.bmWidth, bm.bmHeight, 0, NULL, DI_MASK);
					SelectObject(hdcBitmap, hbmOld);
					SelectObject(hdcMask, hbmOldMask);

					// Create the new mirrored icon and delete bitmaps
					ii.hbmMask = hbmMask;
					ii.hbmColor = hbm;
					hicon = CreateIconIndirect(&ii);
					DeleteObject(hbm);
					DeleteObject(hbmMask);
				}
			}
		}
		ReleaseDC(NULL, hdcScreen);
	}

	if (hdcBitmap)
		DeleteDC(hdcBitmap);
	if (hdcMask)
		DeleteDC(hdcMask);
	DestroyIcon(hiconOrg);
	return hicon;
}

#ifdef RUFUS_TEST
static __inline LPWORD lpwAlign(LPWORD addr)
{
	return (LPWORD)((((uintptr_t)addr) + 3) & (~3));
}

INT_PTR CALLBACK SelectionDynCallback(HWND hwndDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
	int r = -1;
	switch (message) {
	case WM_INITDIALOG:
		return (INT_PTR)TRUE;

	case WM_COMMAND:
		switch (LOWORD(wParam)) {
		case IDOK:
			r = 0;
		case IDCANCEL:
			EndDialog(hwndDlg, r);
			return (INT_PTR)TRUE;
		}
	}
	return FALSE;
}

int SelectionDyn(char* title, char* message, char** szChoice, int nChoices)
{
#define ID_RADIO  12345
	LPCWSTR lpwszTypeFace = L"MS Shell Dlg";
	LPDLGTEMPLATEA lpdt;
	LPDLGITEMTEMPLATEA lpdit;
	LPCWSTR lpwszCaption;
	LPWORD lpw;
	LPWSTR lpwsz;
	int i, ret, nchar;

	lpdt = (LPDLGTEMPLATE)calloc(512 + nChoices * 256, 1);

	// Set up a dialog window
	lpdt->style = WS_POPUP | WS_BORDER | WS_SYSMENU | WS_CAPTION | DS_MODALFRAME | DS_CENTER | DS_SHELLFONT;
	lpdt->cdit = 2 + nChoices;
	lpdt->x = 10;
	lpdt->y = 10;
	lpdt->cx = 300;
	lpdt->cy = 100;

	// https://msdn.microsoft.com/en-us/library/windows/desktop/ms645394.aspx:
	// In a standard template for a dialog box, the DLGTEMPLATE structure is always immediately followed by
	// three variable-length arrays that specify the menu, class, and title for the dialog box.
	// When the DS_SETFONT style is specified, these arrays are also followed by a 16-bit value specifying
	// point size and another variable-length array specifying a typeface name. Each array consists of one
	// or more 16-bit elements. The menu, class, title, and font arrays must be aligned on WORD boundaries.
	lpw = (LPWORD)(&lpdt[1]);
	*lpw++ = 0;		// No menu
	*lpw++ = 0;		// Default dialog class
	lpwsz = (LPWSTR)lpw;
	nchar = MultiByteToWideChar(CP_UTF8, 0, title, -1, lpwsz, 50);
	lpw += nchar;

	// Set point size and typeface name if required
	if (lpdt->style & (DS_SETFONT | DS_SHELLFONT)) {
		*lpw++ = 8;
		for (lpwsz = (LPWSTR)lpw, lpwszCaption = lpwszTypeFace; (*lpwsz++ = *lpwszCaption++) != 0; );
		lpw = (LPWORD)lpwsz;
	}

	// Add an OK button
	lpw = lpwAlign(lpw);
	lpdit = (LPDLGITEMTEMPLATE)lpw;
	lpdit->style = WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON;
	lpdit->x = 10;
	lpdit->y = 70;
	lpdit->cx = 50;
	lpdit->cy = 14;
	lpdit->id = IDOK;

	lpw = (LPWORD)(&lpdit[1]);
	*lpw++ = 0xFFFF;
	*lpw++ = 0x0080;	// Button class

	lpwsz = (LPWSTR)lpw;
	nchar = MultiByteToWideChar(CP_UTF8, 0, "OK", -1, lpwsz, 50);
	lpw += nchar;
	*lpw++ = 0;		// No creation data

					// Add a Cancel button
	lpw = lpwAlign(lpw);
	lpdit = (LPDLGITEMTEMPLATE)lpw;
	lpdit->x = 90;
	lpdit->y = 70;
	lpdit->cx = 50;
	lpdit->cy = 14;
	lpdit->id = IDCANCEL;
	lpdit->style = WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON;

	lpw = (LPWORD)(&lpdit[1]);
	*lpw++ = 0xFFFF;
	*lpw++ = 0x0080;

	lpwsz = (LPWSTR)lpw;
	nchar = MultiByteToWideChar(CP_UTF8, 0, lmprintf(MSG_007), -1, lpwsz, 50);
	lpw += nchar;
	*lpw++ = 0;

	// Add radio buttons
	for (i = 0; i < nChoices; i++) {
		lpw = lpwAlign(lpw);
		lpdit = (LPDLGITEMTEMPLATE)lpw;
		lpdit->x = 10;
		lpdit->y = 10 + 15 * i;
		lpdit->cx = 40;
		lpdit->cy = 20;
		lpdit->id = ID_RADIO;
		lpdit->style = WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | (i == 0 ? WS_GROUP : 0);

		lpw = (LPWORD)(&lpdit[1]);
		*lpw++ = 0xFFFF;
		*lpw++ = 0x0080;

		lpwsz = (LPWSTR)lpw;
		nchar = MultiByteToWideChar(CP_UTF8, 0, szChoice[i], -1, lpwsz, 150);
		lpw += nchar;
		*lpw++ = 0;
	}

	ret = (int)DialogBoxIndirect(hMainInstance, (LPDLGTEMPLATE)lpdt, hMainDialog, (DLGPROC)SelectionDynCallback);
	free(lpdt);
	return ret;
}
#endif
