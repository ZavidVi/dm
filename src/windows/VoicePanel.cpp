#include "VoicePanel.hpp"
#include "Main.hpp"
#include "voice/VoiceEngine.hpp"

#define T_VOICE_PANEL_CLASS TEXT("DMVoicePanelClass")

#define IDT_VOICE_REFRESH  (1)
#define VOICE_REFRESH_MS   (150)

enum
{
	IDC_VOICE_STATUS = 7001,
	IDC_VOICE_LIST,
	IDC_VOICE_MUTE,
	IDC_VOICE_DEAFEN,
	IDC_VOICE_HANGUP,
};

static HWND g_hwndPanel = NULL;
static HWND g_hwndStatus = NULL;
static HWND g_hwndList = NULL;
static HWND g_hwndMute = NULL;
static HWND g_hwndDeafen = NULL;
static HWND g_hwndHangUp = NULL;
static bool g_bClassRegistered = false;

// What is currently shown, so that controls are only touched when something changed.
static std::string g_shownTitle;
static std::string g_shownStatus;
static std::string g_shownMute;
static std::string g_shownDeafen;
static std::vector<std::string> g_shownLines;

static void SetTextIfChanged(HWND hwnd, std::string& shown, const std::string& text)
{
	if (shown == text)
		return;

	shown = text;

	LPTSTR tstr = ConvertCppStringToTString(text);
	SetWindowText(hwnd, tstr);
	free(tstr);
}

static std::string GetString(int id)
{
	return MakeStringFromTString(TmGetTString(id));
}

static std::string BuildTitle()
{
	DiscordInstance* pInstance = GetDiscordInstance();
	Channel* pChan = pInstance->GetChannel(pInstance->GetVoiceChannelID());
	if (!pChan)
		return GetString(IDS_VOICE_TITLE);

	if (pChan->IsDM())
	{
		std::string name = pChan->m_name;
		if (pChan->m_channelType == Channel::DM)
		{
			Profile* pf = GetProfileCache()->LookupProfile(pChan->GetDMRecipient(), "", "", "", false);
			name = pf->GetName(0);
		}

		return GetString(IDS_VOICE_CALL_WITH) + ": " + name;
	}

	std::string title;
	Guild* pGuild = pInstance->GetGuild(pInstance->GetVoiceGuildID());
	if (pGuild)
		title = pGuild->m_name + " - ";

	return title + pChan->m_name;
}

static std::string BuildStatus()
{
	switch (VoiceEngine::GetState())
	{
		case VoiceEngine::STATE_CONNECTED:
		{
			std::string text = GetString(IDS_VOICE_CONNECTED);
			if (!VoiceEngine::HasMicrophone())
				text += "\n" + GetString(IDS_VOICE_NO_MICROPHONE);

			return text;
		}
		case VoiceEngine::STATE_RECONNECTING:
			return GetString(IDS_VOICE_RECONNECTING);

		case VoiceEngine::STATE_ERROR:
			return VoiceEngine::GetErrorText();

		default:
			return GetString(IDS_VOICE_CONNECTING);
	}
}

static void BuildParticipantLines(std::vector<std::string>& lines)
{
	DiscordInstance* pInstance = GetDiscordInstance();

	std::set<Snowflake> speaking;
	VoiceEngine::GetSpeakingUsers(speaking);

	Snowflake channel = pInstance->GetVoiceChannelID();
	Snowflake guild = pInstance->GetVoiceGuildID();

	for (Snowflake user : pInstance->GetVoiceStates().GetUsersInChannel(channel))
	{
		Profile* pf = GetProfileCache()->LookupProfile(user, "", "", "", false);
		std::string line = (speaking.find(user) != speaking.end()) ? "(*) " : "      ";
		line += pf->GetName(guild);

		const VoiceState* pState = pInstance->GetVoiceStates().Get(user);
		if (pState)
		{
			if (pState->IsDeafened())
				line += GetString(IDS_VOICE_DEAFENED_SUFFIX);
			else if (pState->IsMuted())
				line += GetString(IDS_VOICE_MUTED_SUFFIX);
		}

		lines.push_back(line);
	}
}

static void Layout(HWND hWnd)
{
	RECT rc;
	GetClientRect(hWnd, &rc);

	int margin = ScaleByDPI(8);
	int width = rc.right - rc.left;
	int height = rc.bottom - rc.top;
	int statusHeight = ScaleByDPI(34);
	int buttonHeight = ScaleByDPI(24);
	int buttonWidth = (width - margin * 4) / 3;

	MoveWindow(g_hwndStatus, margin, margin, width - margin * 2, statusHeight, TRUE);

	int listTop = margin * 2 + statusHeight;
	int buttonsTop = height - margin - buttonHeight;
	MoveWindow(g_hwndList, margin, listTop, width - margin * 2, std::max(buttonsTop - margin - listTop, 10), TRUE);

	MoveWindow(g_hwndMute,   margin,                          buttonsTop, buttonWidth, buttonHeight, TRUE);
	MoveWindow(g_hwndDeafen, margin * 2 + buttonWidth,        buttonsTop, buttonWidth, buttonHeight, TRUE);
	MoveWindow(g_hwndHangUp, margin * 3 + buttonWidth * 2,    buttonsTop, buttonWidth, buttonHeight, TRUE);
}

static HWND CreateChild(HWND hParent, LPCTSTR cls, LPCTSTR text, DWORD style, DWORD exStyle, int id)
{
	HWND hwnd = CreateWindowEx(
		exStyle, cls, text, WS_CHILD | WS_VISIBLE | style,
		0, 0, 10, 10,
		hParent, (HMENU)(INT_PTR)id, g_hInstance, NULL
	);

	SendMessage(hwnd, WM_SETFONT, (WPARAM)g_SendButtonFont, TRUE);
	return hwnd;
}

static LRESULT CALLBACK VoicePanelProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch (uMsg)
	{
		case WM_CREATE:
		{
			g_hwndStatus = CreateChild(hWnd, WC_STATIC, TEXT(""), SS_LEFT, 0, IDC_VOICE_STATUS);
			g_hwndList   = CreateChild(hWnd, WC_LISTBOX, TEXT(""), WS_VSCROLL | LBS_NOSEL | LBS_NOINTEGRALHEIGHT, WS_EX_CLIENTEDGE, IDC_VOICE_LIST);
			g_hwndMute   = CreateChild(hWnd, WC_BUTTON, TmGetTString(IDS_VOICE_MUTE),       WS_TABSTOP | BS_PUSHBUTTON, 0, IDC_VOICE_MUTE);
			g_hwndDeafen = CreateChild(hWnd, WC_BUTTON, TmGetTString(IDS_VOICE_DEAFEN),     WS_TABSTOP | BS_PUSHBUTTON, 0, IDC_VOICE_DEAFEN);
			g_hwndHangUp = CreateChild(hWnd, WC_BUTTON, TmGetTString(IDS_VOICE_DISCONNECT), WS_TABSTOP | BS_PUSHBUTTON, 0, IDC_VOICE_HANGUP);
			SetTimer(hWnd, IDT_VOICE_REFRESH, VOICE_REFRESH_MS, NULL);
			return 0;
		}
		case WM_SIZE:
		{
			Layout(hWnd);
			return 0;
		}
		case WM_GETMINMAXINFO:
		{
			MINMAXINFO* pmmi = (MINMAXINFO*) lParam;
			pmmi->ptMinTrackSize.x = ScaleByDPI(230);
			pmmi->ptMinTrackSize.y = ScaleByDPI(200);
			return 0;
		}
		case WM_TIMER:
		{
			if (wParam == IDT_VOICE_REFRESH)
				VoicePanel::Refresh();
			return 0;
		}
		case WM_COMMAND:
		{
			switch (LOWORD(wParam))
			{
				case IDC_VOICE_MUTE:   VoiceUIToggleMute();   break;
				case IDC_VOICE_DEAFEN: VoiceUIToggleDeafen(); break;
				case IDC_VOICE_HANGUP: VoiceUIHangUp();       break;
			}
			return 0;
		}
		case WM_CLOSE:
		{
			// Closing the panel means leaving, it'd be too easy to forget being in a call otherwise.
			VoiceUIHangUp();
			return 0;
		}
		case WM_DESTROY:
		{
			KillTimer(hWnd, IDT_VOICE_REFRESH);
			g_hwndPanel = g_hwndStatus = g_hwndList = g_hwndMute = g_hwndDeafen = g_hwndHangUp = NULL;
			return 0;
		}
	}

	return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

void VoicePanel::Initialize()
{
	if (g_bClassRegistered)
		return;

	WNDCLASS wc;
	ZeroMemory(&wc, sizeof wc);
	wc.lpszClassName = T_VOICE_PANEL_CLASS;
	wc.hbrBackground = ri::GetSysColorBrush(COLOR_3DFACE);
	wc.hCursor       = LoadCursor(0, IDC_ARROW);
	wc.hIcon         = g_Icon;
	wc.lpfnWndProc   = VoicePanelProc;
	wc.hInstance     = g_hInstance;

	RegisterClass(&wc);
	g_bClassRegistered = true;
}

bool VoicePanel::IsShown()
{
	return g_hwndPanel != NULL;
}

void VoicePanel::Show()
{
	Initialize();

	if (g_hwndPanel)
	{
		ShowWindow(g_hwndPanel, SW_SHOWNORMAL);
		SetForegroundWindow(g_hwndPanel);
		Refresh();
		return;
	}

	DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_CLIPCHILDREN;
	DWORD exStyle = WS_EX_TOOLWINDOW;

	RECT rc = { 0, 0, ScaleByDPI(280), ScaleByDPI(300) };
	AdjustWindowRectEx(&rc, style, FALSE, exStyle);

	g_shownTitle.clear();
	g_shownStatus.clear();
	g_shownMute.clear();
	g_shownDeafen.clear();
	g_shownLines.clear();

	g_hwndPanel = CreateWindowEx(
		exStyle,
		T_VOICE_PANEL_CLASS,
		TmGetTString(IDS_VOICE_TITLE),
		style,
		CW_USEDEFAULT, CW_USEDEFAULT,
		rc.right - rc.left, rc.bottom - rc.top,
		g_Hwnd, NULL, g_hInstance, NULL
	);

	if (!g_hwndPanel)
		return;

	// Park it in the corner of the main window, out of the way of the messages.
	RECT rcMain;
	GetWindowRect(g_Hwnd, &rcMain);
	SetWindowPos(
		g_hwndPanel, NULL,
		rcMain.left + ScaleByDPI(40), rcMain.top + ScaleByDPI(100),
		0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE
	);

	Layout(g_hwndPanel);
	ShowWindow(g_hwndPanel, SW_SHOWNOACTIVATE);
	Refresh();
}

void VoicePanel::Hide()
{
	if (g_hwndPanel)
		DestroyWindow(g_hwndPanel);
}

void VoicePanel::Refresh()
{
	if (!g_hwndPanel)
		return;

	DiscordInstance* pInstance = GetDiscordInstance();
	if (!pInstance->IsInVoice())
	{
		Hide();
		return;
	}

	SetTextIfChanged(g_hwndPanel, g_shownTitle, BuildTitle());
	SetTextIfChanged(g_hwndStatus, g_shownStatus, BuildStatus());

	bool bDeaf = pInstance->IsVoiceSelfDeafened();
	bool bMute = pInstance->IsVoiceSelfMuted() && !bDeaf;
	SetTextIfChanged(g_hwndMute,   g_shownMute,   GetString(bMute ? IDS_VOICE_UNMUTE   : IDS_VOICE_MUTE));
	SetTextIfChanged(g_hwndDeafen, g_shownDeafen, GetString(bDeaf ? IDS_VOICE_UNDEAFEN : IDS_VOICE_DEAFEN));

	std::vector<std::string> lines;
	BuildParticipantLines(lines);

	if (lines != g_shownLines)
	{
		g_shownLines = lines;

		int top = ListBox_GetTopIndex(g_hwndList);
		SendMessage(g_hwndList, WM_SETREDRAW, FALSE, 0);
		ListBox_ResetContent(g_hwndList);

		for (auto& line : lines)
		{
			LPTSTR tstr = ConvertCppStringToTString(line);
			ListBox_AddString(g_hwndList, tstr);
			free(tstr);
		}

		ListBox_SetTopIndex(g_hwndList, top);
		SendMessage(g_hwndList, WM_SETREDRAW, TRUE, 0);
		InvalidateRect(g_hwndList, NULL, TRUE);
	}
}

// ---- Actions ----

static bool CheckVoiceAvailable()
{
	if (VoiceEngine::IsAvailable())
		return true;

	MessageBox(g_Hwnd, TmGetTString(IDS_VOICE_NOT_AVAILABLE), TmGetTString(IDS_PROGRAM_NAME), MB_ICONINFORMATION | MB_OK);
	return false;
}

void VoiceUIJoinChannel(Snowflake guild, Snowflake channel)
{
	if (!CheckVoiceAvailable())
		return;

	if (!GetDiscordInstance()->JoinVoiceChannel(guild, channel)) {
		MessageBox(g_Hwnd, TmGetTString(IDS_VOICE_NO_PERMISSION), TmGetTString(IDS_PROGRAM_NAME), MB_ICONINFORMATION | MB_OK);
		return;
	}

	VoicePanel::Show();
}

void VoiceUIStartCall(Snowflake channel)
{
	if (!CheckVoiceAvailable())
		return;

	if (!GetDiscordInstance()->StartCall(channel))
		return;

	VoicePanel::Show();
}

void VoiceUIHangUp()
{
	GetDiscordInstance()->LeaveVoiceChannel();
	VoicePanel::Hide();
}

static void ApplySelfState(bool mute, bool deaf)
{
	GetDiscordInstance()->SetVoiceSelfState(mute, deaf);
	VoiceEngine::SetMuted(mute || deaf);
	VoiceEngine::SetDeafened(deaf);
	VoicePanel::Refresh();
}

void VoiceUIToggleMute()
{
	DiscordInstance* pInstance = GetDiscordInstance();
	if (!pInstance->IsInVoice())
		return;

	if (pInstance->IsVoiceSelfDeafened())
		// Like on the official client, unmuting while deafened gets you out of being deafened too.
		ApplySelfState(false, false);
	else
		ApplySelfState(!pInstance->IsVoiceSelfMuted(), false);
}

void VoiceUIToggleDeafen()
{
	DiscordInstance* pInstance = GetDiscordInstance();
	if (!pInstance->IsInVoice())
		return;

	ApplySelfState(pInstance->IsVoiceSelfMuted(), !pInstance->IsVoiceSelfDeafened());
}
