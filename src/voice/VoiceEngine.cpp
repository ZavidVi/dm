#include "VoiceEngine.hpp"

#ifdef ENABLE_VOICE

#include <memory>
#include "VoiceConnection.hpp"
#include "windows/Main.hpp"
#include "windows/WindowMessages.hpp"

static std::unique_ptr<VoiceConnection> g_pConnection;
static unsigned g_generation = 0;
static bool g_bMuted = false;
static bool g_bDeafened = false;
static std::string g_lastError;

// Runs on whichever thread noticed that the connection is over.
static void ConnectionStopped(unsigned generation, bool bFailed)
{
	PostMessage(g_Hwnd, WM_VOICEENGINESTOPPED, (WPARAM)generation, (LPARAM)(bFailed ? 1 : 0));
}

bool VoiceEngine::IsAvailable()
{
	return true;
}

void VoiceEngine::Connect(const VoiceConnectParams& params, const std::set<Snowflake>& usersInChannel)
{
	Disconnect();
	g_lastError.clear();

	g_generation++;
	g_pConnection.reset(new VoiceConnection(params, usersInChannel, g_generation, &ConnectionStopped));
	g_pConnection->SetMuted(g_bMuted);
	g_pConnection->SetDeafened(g_bDeafened);

	if (!g_pConnection->Start())
	{
		// The failure is kept so that the interface can tell the user about it.
		g_lastError = g_pConnection->GetErrorText();
		g_pConnection.reset();
		PostMessage(g_Hwnd, WM_VOICEENGINESTOPPED, (WPARAM)g_generation, (LPARAM)1);
	}
}

void VoiceEngine::Disconnect()
{
	g_pConnection.reset();
}

void VoiceEngine::SetMuted(bool muted)
{
	g_bMuted = muted;
	if (g_pConnection)
		g_pConnection->SetMuted(muted);
}

void VoiceEngine::SetDeafened(bool deafened)
{
	g_bDeafened = deafened;
	if (g_pConnection)
		g_pConnection->SetDeafened(deafened);
}

VoiceEngine::eState VoiceEngine::GetState()
{
	if (!g_pConnection)
		return g_lastError.empty() ? STATE_IDLE : STATE_ERROR;

	switch (g_pConnection->GetStatus())
	{
		case VoiceConnection::STATUS_CONNECTING:
		case VoiceConnection::STATUS_HANDSHAKING:
			return STATE_CONNECTING;
		case VoiceConnection::STATUS_CONNECTED:
			return STATE_CONNECTED;
		case VoiceConnection::STATUS_RECONNECTING:
			return STATE_RECONNECTING;
		case VoiceConnection::STATUS_FAILED:
			return STATE_ERROR;
		default:
			return STATE_IDLE;
	}
}

bool VoiceEngine::HasMicrophone()
{
	return g_pConnection && g_pConnection->HasMicrophone();
}

std::string VoiceEngine::GetErrorText()
{
	if (g_pConnection)
		return g_pConnection->GetErrorText();

	return g_lastError;
}

void VoiceEngine::GetSpeakingUsers(std::set<Snowflake>& users)
{
	if (g_pConnection)
		g_pConnection->GetSpeakingUsers(users);
}

bool VoiceEngine::OnStopped(unsigned generation, bool bFailed)
{
	if (generation != g_generation)
		return false;

	if (g_pConnection)
		g_lastError = bFailed ? g_pConnection->GetErrorText() : std::string();
	else if (!bFailed)
		g_lastError.clear();

	g_pConnection.reset();
	return true;
}

#else // !ENABLE_VOICE

bool VoiceEngine::IsAvailable() { return false; }
void VoiceEngine::Connect(const VoiceConnectParams&, const std::set<Snowflake>&) {}
void VoiceEngine::Disconnect() {}
void VoiceEngine::SetMuted(bool) {}
void VoiceEngine::SetDeafened(bool) {}
VoiceEngine::eState VoiceEngine::GetState() { return STATE_IDLE; }
bool VoiceEngine::HasMicrophone() { return false; }
std::string VoiceEngine::GetErrorText() { return std::string(); }
void VoiceEngine::GetSpeakingUsers(std::set<Snowflake>&) {}
bool VoiceEngine::OnStopped(unsigned, bool) { return false; }

#endif
