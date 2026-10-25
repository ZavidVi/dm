#ifndef NOMINMAX
#define NOMINMAX
#endif

// inet_addr is all that is needed to turn the dotted address of the media server into an address,
// and the replacement is not available on Windows XP.
#define _WINSOCK_DEPRECATED_NO_WARNINGS

#include "VoiceConnection.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ws2tcpip.h>
#include <opus.h>

#include "utils/Util.hpp"

#define VOICE_GATEWAY_VERSION   8
#define OPUS_PAYLOAD_TYPE       120
#define OPUS_MAX_PACKET_SIZE    1500
#define OPUS_MAX_FRAME_SAMPLES  5760     // 120 ms at 48 kHz
#define SILENCE_FRAMES_AT_END   5

// Voice gateway opcodes.
namespace VoiceOp
{
	enum
	{
		IDENTIFY            = 0,
		SELECT_PROTOCOL     = 1,
		READY               = 2,
		HEARTBEAT           = 3,
		SESSION_DESCRIPTION = 4,
		SPEAKING            = 5,
		HEARTBEAT_ACK       = 6,
		RESUME              = 7,
		HELLO               = 8,
		RESUMED             = 9,
		CLIENTS_CONNECT     = 11,
		CLIENT_DISCONNECT   = 13,
	};
}

// Close codes of the voice gateway that we care about.
namespace VoiceClose
{
	enum
	{
		AUTHENTICATION_FAILED = 4004,
		SESSION_INVALID       = 4006,
		DISCONNECTED          = 4014,   // kicked, moved elsewhere, channel deleted ...
		SERVER_CRASHED        = 4015,
		UNKNOWN_ENCRYPTION    = 4016,
		DAVE_REQUIRED         = 4017,
		CALL_TERMINATED       = 4022,
	};
}

// Tuning of the voice detection.  Audio is only sent while the microphone picks up something
// that is louder than this (average amplitude, out of 32768), and for a little while after.
#define VOICE_THRESHOLD         250
#define VOICE_HANGOVER_FRAMES   15       // 300 ms

#define HEARTBEAT_ACK_GRACE_MS  5000
#define KEEPALIVE_INTERVAL_MS   5000
#define MAX_RECONNECT_ATTEMPTS  6
#define STREAM_TIMEOUT_MS       15000

static inline uint16_t ReadBE16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
static inline uint32_t ReadBE32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
static inline void WriteBE16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
static inline void WriteBE32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }

// ---- A participant whose audio we're playing ----

struct VoiceConnection::RemoteStream
{
	uint32_t m_ssrc;
	Snowflake m_user;
	OpusDecoder* m_decoder;

	std::map<int32_t, std::vector<uint8_t> > m_packets; // by extended sequence number
	bool m_bHaveBase;
	int32_t m_highestSeq;
	int32_t m_nextSeq;
	bool m_bPrimed;
	int m_concealed;
	DWORD m_gapSince;
	DWORD m_lastPacketTick;
	DWORD m_lastVoiceTick;
	std::vector<int16_t> m_fifo;

	RemoteStream(uint32_t ssrc, Snowflake user) :
		m_ssrc(ssrc),
		m_user(user),
		m_decoder(nullptr),
		m_bHaveBase(false),
		m_highestSeq(0),
		m_nextSeq(0),
		m_bPrimed(false),
		m_concealed(0),
		m_gapSince(0),
		m_lastPacketTick(0),
		m_lastVoiceTick(0)
	{
		int error = 0;
		m_decoder = opus_decoder_create(VoiceAudioIO::SAMPLE_RATE, 1, &error);
		m_fifo.reserve(VoiceAudioIO::FRAME_SAMPLES * 4);
	}

	~RemoteStream()
	{
		if (m_decoder)
			opus_decoder_destroy(m_decoder);
	}

	void Push(uint16_t seq, const std::vector<uint8_t>& opus, DWORD now)
	{
		m_lastPacketTick = now;
		if (opus.size() > 3)
			m_lastVoiceTick = now;

		int32_t ext;
		if (!m_bHaveBase)
		{
			m_bHaveBase = true;
			m_highestSeq = seq;
			m_nextSeq = seq;
			ext = seq;
		}
		else
		{
			int16_t delta = int16_t(uint16_t(seq - uint16_t(m_highestSeq)));
			ext = m_highestSeq + delta;
			if (delta > 0)
				m_highestSeq = ext;
		}

		if (ext < m_nextSeq)
			return; // too late, we've played past it already

		m_packets[ext] = opus;

		// Don't let a stalled playback build up an ever growing delay.
		while (m_packets.size() > 12)
		{
			m_nextSeq = m_packets.begin()->first;
			m_packets.erase(m_packets.begin());
			m_nextSeq = m_packets.begin()->first;
		}
	}

	void Decode(const uint8_t* data, size_t size)
	{
		int16_t pcm[OPUS_MAX_FRAME_SAMPLES];
		int count = opus_decode(m_decoder, data, int(size), pcm, OPUS_MAX_FRAME_SAMPLES, 0);
		if (count > 0)
			m_fifo.insert(m_fifo.end(), pcm, pcm + count);
	}

	void Conceal()
	{
		int16_t pcm[VoiceAudioIO::FRAME_SAMPLES];
		int count = opus_decode(m_decoder, NULL, 0, pcm, VoiceAudioIO::FRAME_SAMPLES, 0);
		if (count > 0)
			m_fifo.insert(m_fifo.end(), pcm, pcm + count);
	}

	void FillFifo(DWORD now)
	{
		if (!m_bPrimed)
		{
			// Collect a couple of packets before starting, so that a little jitter is tolerated.
			if (m_packets.size() < 2)
				return;

			m_bPrimed = true;
			m_nextSeq = m_packets.begin()->first;
		}

		if (m_packets.empty())
		{
			// Ran dry.  Smooth over a short hole, but don't invent audio forever.
			if (now - m_lastPacketTick < 120 && m_concealed < 3)
			{
				Conceal();
				m_concealed++;
				m_nextSeq++;
			}
			else if (now - m_lastPacketTick >= 120)
			{
				m_bPrimed = false;
			}
			return;
		}

		auto first = m_packets.begin();
		if (first->first - m_nextSeq > 50)
			m_nextSeq = first->first; // the other side restarted, or we lost a lot

		if (first->first == m_nextSeq)
		{
			Decode(first->second.data(), first->second.size());
			m_packets.erase(first);
			m_nextSeq++;
			m_concealed = 0;
			m_gapSince = 0;
		}
		else
		{
			// A packet is missing.  Give it a moment to show up, then conceal it.
			if (m_gapSince == 0)
				m_gapSince = now ? now : 1;

			if (m_packets.size() >= 3 || now - m_gapSince > 40)
			{
				Conceal();
				m_nextSeq++;
				m_gapSince = 0;
			}
		}
	}

	// Gets the next 20 ms of this participant's audio.  Returns false if there's nothing to play.
	bool Pull(int16_t* out, DWORD now)
	{
		const size_t frame = VoiceAudioIO::FRAME_SAMPLES;
		if (m_fifo.size() < frame)
			FillFifo(now);

		if (m_fifo.size() < frame)
			return false;

		memcpy(out, m_fifo.data(), frame * sizeof(int16_t));
		m_fifo.erase(m_fifo.begin(), m_fifo.begin() + frame);

		// Keep the delay bounded in case more arrives than we play.
		if (m_fifo.size() > frame * 6)
			m_fifo.erase(m_fifo.begin(), m_fifo.end() - frame * 2);

		return true;
	}
};

// ---- Construction ----

VoiceConnection::VoiceConnection(
	const VoiceConnectParams& params,
	const std::set<Snowflake>& usersInChannel,
	unsigned generation,
	StoppedCallback pfnStopped) :
	m_params(params),
	m_generation(generation),
	m_pfnStopped(pfnStopped),
	m_status(STATUS_CONNECTING),
	m_bStopRequested(false),
	m_bMuted(false),
	m_bDeafened(false),
	m_bFinished(false),
	m_lastSeq(-1),
	m_heartbeatInterval(0),
	m_lastHeartbeatTick(0),
	m_lastAckTick(0),
	m_bResuming(false),
	m_bHaveSession(false),
	m_bWsLost(false),
	m_wsCloseCode(0),
	m_reconnectAttempts(0),
	m_nextReconnectTick(0),
	m_bReconnectPending(false),
	m_ssrc(0),
	m_serverPort(0),
	m_cryptoMode(VoiceCrypto::MODE_NONE),
	m_bHaveKey(false),
	m_users(usersInChannel),
	m_socket(INVALID_SOCKET),
	m_lastKeepAliveTick(0),
	m_keepAliveCounter(0),
	m_encoder(nullptr),
	m_rtpSeq(0),
	m_rtpTimestamp(0),
	m_nonceCounter(0),
	m_hangoverFrames(0),
	m_bSpeakingSent(false),
	m_bTransmitting(false),
	m_bAudioStarted(false),
	m_hWake(NULL)
{
	memset(m_secretKey, 0, sizeof m_secretKey);
	m_users.insert(m_params.m_userId);
	m_hWake = CreateEvent(NULL, FALSE, FALSE, NULL);
}

VoiceConnection::~VoiceConnection()
{
	Stop();

	// The order matters: the signalling is what starts the audio and the receive thread, so it
	// has to be shut down first, and the control thread is what swaps the websocket around.
	m_controlThread.Join();
	m_ws.reset();
	m_audio.Stop();
	CloseSocket();
	m_receiveThread.Join();

	DestroyCodec();
	ClearStreams();

	if (m_hWake)
		CloseHandle(m_hWake);

	WSACleanup();
}

bool VoiceConnection::Start()
{
	WSADATA wsaData;
	WSAStartup(MAKEWORD(2, 2), &wsaData);

	if (!CreateCodec()) {
		SetError("Could not initialize the audio codec.");
		SetStatus(STATUS_FAILED);
		return false;
	}

	m_lastAckTick = GetTickCount();

	if (!ConnectWebsocket(false)) {
		SetError("Could not connect to the voice server.");
		SetStatus(STATUS_FAILED);
		return false;
	}

	return m_controlThread.Start(this, &VoiceConnection::ControlThreadProc);
}

void VoiceConnection::Stop()
{
	m_bStopRequested = true;
	if (m_hWake)
		SetEvent(m_hWake);
}

std::string VoiceConnection::GetErrorText()
{
	VoiceAutoLock lock(m_lock);
	return m_errorText;
}

void VoiceConnection::SetError(const std::string& text)
{
	VoiceAutoLock lock(m_lock);
	m_errorText = text;
}

void VoiceConnection::SetStatus(eStatus status)
{
	m_status = status;
}

// Ends the connection on our own initiative and lets the owner know.
void VoiceConnection::Finish(bool bFailed, const std::string& error)
{
	if (m_bFinished)
		return;

	m_bFinished = true;
	m_bStopRequested = true;
	m_bTransmitting = false;

	if (bFailed)
		SetError(error);

	SetStatus(bFailed ? STATUS_FAILED : STATUS_STOPPED);

	if (m_pfnStopped)
		m_pfnStopped(m_generation, bFailed);
}

// ---- Codec ----

bool VoiceConnection::CreateCodec()
{
	int error = 0;
	m_encoder = opus_encoder_create(VoiceAudioIO::SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP, &error);
	if (!m_encoder || error != OPUS_OK)
		return false;

	opus_encoder_ctl(m_encoder, OPUS_SET_BITRATE(64000));
	opus_encoder_ctl(m_encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
	opus_encoder_ctl(m_encoder, OPUS_SET_INBAND_FEC(1));
	opus_encoder_ctl(m_encoder, OPUS_SET_PACKET_LOSS_PERC(10));
	return true;
}

void VoiceConnection::DestroyCodec()
{
	if (m_encoder) {
		opus_encoder_destroy(m_encoder);
		m_encoder = nullptr;
	}
}

// ---- Websocket ----

bool VoiceConnection::ConnectWebsocket(bool bResume)
{
	// The endpoint comes without a scheme, and may have a port that's not the one to use.
	std::string host = m_params.m_endpoint;
	size_t colon = host.find(':');
	if (colon != std::string::npos)
		host = host.substr(0, colon);

	m_bResuming = bResume;
	m_bWsLost = false;

	// The new websocket is put in place before it connects, as its callbacks start sending right away.
	VoiceWebsocket* pNew = new VoiceWebsocket;
	std::unique_ptr<VoiceWebsocket> pOld;
	{
		VoiceAutoLock lock(m_wsLock);
		pOld = std::move(m_ws);
		m_ws.reset(pNew);
	}

	// Taking the old one down waits for its thread, which may be waiting for the lock.
	pOld.reset();

	bool ok = pNew->Connect(host, "/?v=" + std::to_string(VOICE_GATEWAY_VERSION), this);
	if (!ok)
	{
		VoiceAutoLock lock(m_wsLock);
		m_ws.reset();
	}

	return ok;
}

void VoiceConnection::SendJson(const nlohmann::json& j)
{
	std::string text = j.dump();

	VoiceAutoLock lock(m_wsLock);
	if (m_ws)
		m_ws->SendText(text);
}

void VoiceConnection::OnWsOpen()
{
	SetStatus(m_bResuming ? STATUS_RECONNECTING : STATUS_HANDSHAKING);
	m_lastAckTick = GetTickCount();

	if (m_bResuming)
		SendResume();
	else
		SendIdentify();
}

void VoiceConnection::OnWsClosed(int closeCode, const std::string& reason)
{
	{
		VoiceAutoLock lock(m_lock);
		m_bWsLost = true;
		m_wsCloseCode = closeCode;
		m_wsCloseReason = reason;
	}

	SetEvent(m_hWake);
}

void VoiceConnection::OnWsMessage(const std::string& payload, bool bBinary)
{
	try
	{
		if (bBinary)
			HandleBinaryMessage(payload);
		else
			HandleJsonMessage(payload);
	}
	catch (nlohmann::json::exception& ex)
	{
		DbgPrintF("VoiceConnection: bad message from the voice server: %s", ex.what());
	}
}

void VoiceConnection::SendIdentify()
{
	nlohmann::json d;
	d["server_id"]  = std::to_string(m_params.GetServerId());
	d["user_id"]    = std::to_string(m_params.m_userId);
	d["channel_id"] = std::to_string(m_params.m_channelId);
	d["session_id"] = m_params.m_sessionId;
	d["token"]      = m_params.m_token;
	d["video"]      = false;
	d["max_dave_protocol_version"] = VoiceDave::GetMaxProtocolVersion();

	nlohmann::json j;
	j["op"] = int(VoiceOp::IDENTIFY);
	j["d"] = d;
	SendJson(j);
}

void VoiceConnection::SendResume()
{
	nlohmann::json d;
	d["server_id"]  = std::to_string(m_params.GetServerId());
	d["session_id"] = m_params.m_sessionId;
	d["token"]      = m_params.m_token;
	d["seq_ack"]    = long(m_lastSeq);

	nlohmann::json j;
	j["op"] = int(VoiceOp::RESUME);
	j["d"] = d;
	SendJson(j);
}

void VoiceConnection::SendSelectProtocol(const std::string& address, int port)
{
	nlohmann::json data;
	data["address"] = address;
	data["port"]    = port;
	data["mode"]    = VoiceCrypto::ModeToString(m_cryptoMode);

	nlohmann::json opus;
	opus["name"]         = "opus";
	opus["type"]         = "audio";
	opus["priority"]     = 1000;
	opus["payload_type"] = OPUS_PAYLOAD_TYPE;

	nlohmann::json codecs = nlohmann::json::array();
	codecs.push_back(opus);

	nlohmann::json d;
	d["protocol"] = "udp";
	d["data"]     = data;
	d["codecs"]   = codecs;

	nlohmann::json j;
	j["op"] = int(VoiceOp::SELECT_PROTOCOL);
	j["d"] = d;
	SendJson(j);
}

void VoiceConnection::SendHeartbeat()
{
	nlohmann::json d;
	d["t"]       = (long long) GetTimeMs();
	d["seq_ack"] = long(m_lastSeq);

	nlohmann::json j;
	j["op"] = int(VoiceOp::HEARTBEAT);
	j["d"] = d;
	SendJson(j);
}

void VoiceConnection::SendSpeaking(bool bSpeaking)
{
	nlohmann::json d;
	d["speaking"] = bSpeaking ? 1 : 0;
	d["delay"]    = 0;
	d["ssrc"]     = m_ssrc;

	nlohmann::json j;
	j["op"] = int(VoiceOp::SPEAKING);
	j["d"] = d;
	SendJson(j);
}

void VoiceConnection::SendDaveOutput(const VoiceDave::Output& out)
{
	for (auto& message : out.m_binary)
	{
		VoiceAutoLock lock(m_wsLock);
		if (m_ws)
			m_ws->SendBinary(message.data(), message.size());
	}

	for (int transition : out.m_readyForTransition)
	{
		nlohmann::json d;
		d["transition_id"] = transition;

		nlohmann::json j;
		j["op"] = int(VoiceDave::OP_READY_FOR_TRANSITION);
		j["d"] = d;
		SendJson(j);
	}

	for (int transition : out.m_invalidCommitWelcome)
	{
		nlohmann::json d;
		d["transition_id"] = transition;

		nlohmann::json j;
		j["op"] = int(VoiceDave::OP_MLS_INVALID_COMMIT_WELCOME);
		j["d"] = d;
		SendJson(j);
	}
}

void VoiceConnection::HandleJsonMessage(const std::string& payload)
{
	nlohmann::json j = nlohmann::json::parse(payload);

	if (j.contains("seq") && j["seq"].is_number_integer())
		m_lastSeq = j["seq"].get<long>();

	int op = j["op"];
	nlohmann::json empty;
	nlohmann::json& d = j.contains("d") ? j["d"] : empty;

	switch (op)
	{
		case VoiceOp::HELLO:               HandleHello(d); break;
		case VoiceOp::READY:               HandleReady(d); break;
		case VoiceOp::SESSION_DESCRIPTION: HandleSessionDescription(d); break;
		case VoiceOp::SPEAKING:            HandleSpeaking(d); break;
		case VoiceOp::CLIENTS_CONNECT:     HandleClientsConnect(d); break;
		case VoiceOp::CLIENT_DISCONNECT:   HandleClientDisconnect(d); break;

		case VoiceOp::HEARTBEAT_ACK:
			m_lastAckTick = GetTickCount();
			break;

		case VoiceOp::RESUMED:
			m_reconnectAttempts = 0;
			m_lastAckTick = GetTickCount();
			SetStatus(STATUS_CONNECTED);
			break;

		case VoiceDave::OP_PREPARE_TRANSITION:
		{
			VoiceDave::Output out;
			m_dave.OnPrepareTransition(GetFieldSafeInt(d, "protocol_version"), GetFieldSafeInt(d, "transition_id"), out);
			SendDaveOutput(out);
			break;
		}
		case VoiceDave::OP_EXECUTE_TRANSITION:
		{
			VoiceDave::Output out;
			m_dave.OnExecuteTransition(GetFieldSafeInt(d, "transition_id"), out);
			SendDaveOutput(out);
			break;
		}
		case VoiceDave::OP_PREPARE_EPOCH:
		{
			VoiceDave::Output out;
			m_dave.OnPrepareEpoch(GetFieldSafeInt(d, "protocol_version"), GetFieldSafeInt(d, "epoch"), out);
			SendDaveOutput(out);
			break;
		}

		default:
			DbgPrintF("VoiceConnection: unhandled voice opcode %d", op);
			break;
	}
}

void VoiceConnection::HandleBinaryMessage(const std::string& payload)
{
	// Server to client: uint16 sequence number, uint8 opcode, then the contents.
	if (payload.size() < 3)
		return;

	const uint8_t* data = (const uint8_t*)payload.data();
	m_lastSeq = ReadBE16(data);

	int op = data[2];
	const uint8_t* body = data + 3;
	size_t bodySize = payload.size() - 3;

	VoiceDave::Output out;

	switch (op)
	{
		case VoiceDave::OP_MLS_EXTERNAL_SENDER:
			m_dave.OnExternalSender(body, bodySize);
			break;

		case VoiceDave::OP_MLS_PROPOSALS:
			m_dave.OnProposals(body, bodySize, out);
			break;

		case VoiceDave::OP_MLS_ANNOUNCE_COMMIT:
			if (bodySize >= 2)
				m_dave.OnAnnounceCommit(ReadBE16(body), body + 2, bodySize - 2, out);
			break;

		case VoiceDave::OP_MLS_WELCOME:
			if (bodySize >= 2)
				m_dave.OnWelcome(ReadBE16(body), body + 2, bodySize - 2, out);
			break;

		default:
			DbgPrintF("VoiceConnection: unhandled binary voice opcode %d", op);
			break;
	}

	SendDaveOutput(out);
}

void VoiceConnection::HandleHello(nlohmann::json& d)
{
	double interval = 13750;
	if (d.contains("heartbeat_interval") && d["heartbeat_interval"].is_number())
		interval = d["heartbeat_interval"].get<double>();

	m_heartbeatInterval = int(interval);
	m_lastHeartbeatTick = GetTickCount();
	m_lastAckTick = m_lastHeartbeatTick;
	SetEvent(m_hWake);
}

void VoiceConnection::HandleReady(nlohmann::json& d)
{
	std::vector<std::string> modes;
	if (d.contains("modes") && d["modes"].is_array()) {
		for (auto& m : d["modes"]) {
			if (m.is_string())
				modes.push_back(m.get<std::string>());
		}
	}

	VoiceCrypto::eMode mode = VoiceCrypto::ChooseMode(modes);
	if (mode == VoiceCrypto::MODE_NONE) {
		Finish(true, "The voice server doesn't offer an encryption mode this program supports.");
		return;
	}

	{
		VoiceAutoLock lock(m_lock);
		m_ssrc = uint32_t(GetFieldSafeInt(d, "ssrc"));
		m_serverIp = GetFieldSafe(d, "ip");
		m_serverPort = GetFieldSafeInt(d, "port");
		m_cryptoMode = mode;
		m_bHaveKey = false;
	}

	// The SSRC may differ from the last session's.
	m_dave.Init(m_params.m_userId, m_params.m_channelId, m_ssrc);

	if (!OpenSocket(m_serverIp, m_serverPort)) {
		Finish(true, "Could not open a connection to the media server.");
		return;
	}

	std::string address;
	int port = 0;
	if (!DiscoverExternalAddress(address, port)) {
		Finish(true, "The media server didn't answer.  Are UDP packets blocked?");
		return;
	}

	SendSelectProtocol(address, port);
}

void VoiceConnection::HandleSessionDescription(nlohmann::json& d)
{
	if (!d.contains("secret_key") || !d["secret_key"].is_array() || d["secret_key"].size() != VoiceCrypto::KEY_SIZE) {
		Finish(true, "The voice server sent an invalid key.");
		return;
	}

	std::string modeString = GetFieldSafe(d, "mode");
	VoiceCrypto::eMode mode = VoiceCrypto::ModeFromString(modeString);
	if (mode == VoiceCrypto::MODE_NONE) {
		Finish(true, "The voice server chose an unsupported encryption mode.");
		return;
	}

	{
		VoiceAutoLock lock(m_lock);
		for (size_t i = 0; i < VoiceCrypto::KEY_SIZE; i++)
			m_secretKey[i] = uint8_t(d["secret_key"][i].get<int>());
		m_cryptoMode = mode;
		m_bHaveKey = true;
	}

	int daveVersion = GetFieldSafeInt(d, "dave_protocol_version");

	{
		VoiceDave::Output out;
		m_dave.SetUsers(m_users);
		m_dave.OnSessionDescription(daveVersion, out);
		SendDaveOutput(out);
	}

	m_bHaveSession = true;
	m_reconnectAttempts = 0;
	m_rtpSeq = uint16_t(GetTimeMs());
	m_rtpTimestamp = uint32_t(GetTimeMs() * 48);
	m_nonceCounter = 0;

	if (!m_bAudioStarted)
	{
		if (!m_audio.Start(this)) {
			Finish(true, "Could not open a microphone or speakers.");
			return;
		}

		m_bAudioStarted = true;
	}

	m_receiveThread.Start(this, &VoiceConnection::ReceiveThreadProc);
	SetStatus(STATUS_CONNECTED);
}

void VoiceConnection::HandleSpeaking(nlohmann::json& d)
{
	Snowflake user = GetSnowflake(d, "user_id");
	uint32_t ssrc = uint32_t(GetFieldSafeInt(d, "ssrc"));

	if (!user || !ssrc)
		return;

	VoiceAutoLock lock(m_lock);
	m_ssrcToUser[ssrc] = user;
	m_users.insert(user);
}

void VoiceConnection::HandleClientsConnect(nlohmann::json& d)
{
	if (!d.contains("user_ids") || !d["user_ids"].is_array())
		return;

	for (auto& id : d["user_ids"])
	{
		Snowflake user = GetSnowflakeFromJsonObject(id);
		if (!user)
			continue;

		{
			VoiceAutoLock lock(m_lock);
			m_users.insert(user);
		}

		m_dave.AddUser(user);
	}
}

void VoiceConnection::HandleClientDisconnect(nlohmann::json& d)
{
	Snowflake user = GetSnowflake(d, "user_id");
	if (!user)
		return;

	std::vector<uint32_t> gone;
	{
		VoiceAutoLock lock(m_lock);
		m_users.erase(user);

		for (auto iter = m_ssrcToUser.begin(); iter != m_ssrcToUser.end();) {
			if (iter->second == user) {
				gone.push_back(iter->first);
				iter = m_ssrcToUser.erase(iter);
			}
			else {
				++iter;
			}
		}
	}

	for (uint32_t ssrc : gone)
		RemoveStream(ssrc);

	m_dave.RemoveUser(user);
}

// ---- UDP ----

bool VoiceConnection::OpenSocket(const std::string& ip, int port)
{
	CloseSocket();
	m_receiveThread.Join();

	SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock == INVALID_SOCKET)
		return false;

	sockaddr_in addr;
	memset(&addr, 0, sizeof addr);
	addr.sin_family = AF_INET;
	addr.sin_port = htons(u_short(port));
	addr.sin_addr.s_addr = inet_addr(ip.c_str());

	if (addr.sin_addr.s_addr == INADDR_NONE || connect(sock, (sockaddr*)&addr, sizeof addr) == SOCKET_ERROR)
	{
		closesocket(sock);
		return false;
	}

	m_socket = sock;
	m_lastKeepAliveTick = GetTickCount();
	return true;
}

void VoiceConnection::CloseSocket()
{
	SOCKET sock = m_socket;
	m_socket = INVALID_SOCKET;

	if (sock != INVALID_SOCKET)
		closesocket(sock);
}

// Asks the media server what our address looks like from the outside.
bool VoiceConnection::DiscoverExternalAddress(std::string& address, int& port)
{
	uint8_t request[74];
	memset(request, 0, sizeof request);
	WriteBE16(request + 0, 1);    // type: request
	WriteBE16(request + 2, 70);   // length of the rest
	WriteBE32(request + 4, m_ssrc);

	for (int attempt = 0; attempt < 5 && !m_bStopRequested; attempt++)
	{
		if (send(m_socket, (const char*)request, sizeof request, 0) == SOCKET_ERROR)
			return false;

		fd_set readSet;
		FD_ZERO(&readSet);
		FD_SET(m_socket, &readSet);

		timeval timeout;
		timeout.tv_sec = 0;
		timeout.tv_usec = 600 * 1000;

		if (select(0, &readSet, NULL, NULL, &timeout) <= 0)
			continue;

		uint8_t response[128];
		int received = recv(m_socket, (char*)response, sizeof response, 0);
		if (received < 74 || ReadBE16(response) != 2)
			continue;

		// The address is a null terminated string, followed by the port.
		char buffer[65];
		memcpy(buffer, response + 8, 64);
		buffer[64] = 0;

		address = buffer;
		port = ReadBE16(response + 72);
		return true;
	}

	return false;
}

void VoiceConnection::SendKeepAlive()
{
	if (m_socket == INVALID_SOCKET)
		return;

	uint8_t packet[8];
	memset(packet, 0, sizeof packet);
	WriteBE32(packet, m_keepAliveCounter++);
	send(m_socket, (const char*)packet, sizeof packet, 0);
}

void VoiceConnection::ReceiveThreadProc()
{
	uint8_t buffer[2048];

	while (!m_bStopRequested)
	{
		SOCKET sock = m_socket;
		if (sock == INVALID_SOCKET)
			break;

		fd_set readSet;
		FD_ZERO(&readSet);
		FD_SET(sock, &readSet);

		timeval timeout;
		timeout.tv_sec = 0;
		timeout.tv_usec = 100 * 1000;

		int ready = select(0, &readSet, NULL, NULL, &timeout);
		if (ready < 0)
			break;
		if (ready == 0)
			continue;

		int received = recv(sock, (char*)buffer, sizeof buffer, 0);
		if (received <= 0)
			continue;

		HandleUdpPacket(buffer, size_t(received));
	}
}

void VoiceConnection::HandleUdpPacket(const uint8_t* data, size_t size)
{
	if (size < VoiceCrypto::RTP_HEADER_SIZE + VoiceCrypto::PACKET_OVERHEAD)
		return;

	if ((data[0] & 0xC0) != 0x80)
		return;

	// Skip RTCP, and anything that isn't audio.
	if ((data[1] & 0x7F) != OPUS_PAYLOAD_TYPE)
		return;

	uint16_t seq = ReadBE16(data + 2);
	uint32_t ssrc = ReadBE32(data + 8);

	VoiceCrypto::eMode mode;
	uint8_t key[VoiceCrypto::KEY_SIZE];
	Snowflake user = 0;
	uint32_t ownSsrc;
	{
		VoiceAutoLock lock(m_lock);
		if (!m_bHaveKey)
			return;

		mode = m_cryptoMode;
		memcpy(key, m_secretKey, sizeof key);
		ownSsrc = m_ssrc;

		auto iter = m_ssrcToUser.find(ssrc);
		if (iter != m_ssrcToUser.end())
			user = iter->second;
	}

	if (ssrc == ownSsrc)
		return;

	std::vector<uint8_t> payload;
	if (!VoiceCrypto::DecryptPacket(mode, key, data, size, payload) || payload.empty())
		return;

	std::vector<uint8_t> opus;
	if (m_dave.GetProtocolVersion() > 0)
	{
		// Each sender has their own keys, so we have to know who this is.
		if (!user)
			return;

		if (!m_dave.Decrypt(user, payload.data(), payload.size(), opus) || opus.empty())
			return;
	}
	else
	{
		opus.swap(payload);
	}

	PushRemotePacket(ssrc, user, seq, opus);
}

// ---- Incoming media ----

void VoiceConnection::PushRemotePacket(uint32_t ssrc, Snowflake user, uint16_t seq, const std::vector<uint8_t>& opus)
{
	DWORD now = GetTickCount();

	VoiceAutoLock lock(m_streamLock);

	RemoteStream*& pStream = m_streams[ssrc];
	if (!pStream)
		pStream = new RemoteStream(ssrc, user);

	if (user)
		pStream->m_user = user;

	pStream->Push(seq, opus, now);
}

void VoiceConnection::RemoveStream(uint32_t ssrc)
{
	VoiceAutoLock lock(m_streamLock);

	auto iter = m_streams.find(ssrc);
	if (iter == m_streams.end())
		return;

	delete iter->second;
	m_streams.erase(iter);
}

void VoiceConnection::ClearStreams()
{
	VoiceAutoLock lock(m_streamLock);

	for (auto& pair : m_streams)
		delete pair.second;

	m_streams.clear();
}

void VoiceConnection::GetSpeakingUsers(std::set<Snowflake>& users)
{
	DWORD now = GetTickCount();

	{
		VoiceAutoLock lock(m_streamLock);
		for (auto& pair : m_streams) {
			RemoteStream* pStream = pair.second;
			if (pStream->m_user && pStream->m_lastVoiceTick && now - pStream->m_lastVoiceTick < 300)
				users.insert(pStream->m_user);
		}
	}

	if (m_bTransmitting)
		users.insert(m_params.m_userId);
}

void VoiceConnection::OnPlaybackFrame(int16_t* samples)
{
	const int frame = VoiceAudioIO::FRAME_SAMPLES;
	int32_t mix[VoiceAudioIO::FRAME_SAMPLES];
	memset(mix, 0, sizeof mix);

	bool bDeafened = m_bDeafened;
	DWORD now = GetTickCount();

	{
		VoiceAutoLock lock(m_streamLock);
		for (auto& pair : m_streams)
		{
			int16_t pcm[VoiceAudioIO::FRAME_SAMPLES];
			if (!pair.second->Pull(pcm, now) || bDeafened)
				continue;

			for (int i = 0; i < frame; i++)
				mix[i] += pcm[i];
		}
	}

	for (int i = 0; i < frame; i++)
		samples[i] = int16_t(std::max<int32_t>(-32768, std::min<int32_t>(32767, mix[i])));
}

// ---- Outgoing media ----

void VoiceConnection::OnCapturedFrame(const int16_t* samples)
{
	bool bCanSend =
		m_status == STATUS_CONNECTED &&
		m_socket != INVALID_SOCKET &&
		!m_bMuted &&
		!m_bDeafened &&
		m_dave.CanEncrypt();

	if (!bCanSend)
	{
		if (m_bSpeakingSent)
			SendSilence();

		return;
	}

	// Voice activity detection.
	int64_t sum = 0;
	for (int i = 0; i < VoiceAudioIO::FRAME_SAMPLES; i++)
		sum += std::abs(int(samples[i]));

	int average = int(sum / VoiceAudioIO::FRAME_SAMPLES);
	if (average > VOICE_THRESHOLD)
		m_hangoverFrames = VOICE_HANGOVER_FRAMES;

	if (m_hangoverFrames <= 0)
	{
		if (m_bSpeakingSent)
			SendSilence();

		return;
	}

	m_hangoverFrames--;

	if (!m_bSpeakingSent)
	{
		SendSpeaking(true);
		m_bSpeakingSent = true;
		m_bTransmitting = true;
	}

	uint8_t packet[OPUS_MAX_PACKET_SIZE];
	int length = opus_encode(m_encoder, samples, VoiceAudioIO::FRAME_SAMPLES, packet, sizeof packet);
	if (length > 0)
		SendOpusFrame(packet, size_t(length));
}

// Ends a stretch of speech: the silence frames keep the decoder on the other end from
// guessing at what comes next.
void VoiceConnection::SendSilence()
{
	static const uint8_t silence[3] = { 0xF8, 0xFF, 0xFE };

	for (int i = 0; i < SILENCE_FRAMES_AT_END; i++)
		SendOpusFrame(silence, sizeof silence);

	SendSpeaking(false);
	m_bSpeakingSent = false;
	m_bTransmitting = false;
	m_hangoverFrames = 0;
}

void VoiceConnection::SendOpusFrame(const uint8_t* data, size_t size)
{
	SOCKET sock = m_socket;
	if (sock == INVALID_SOCKET)
		return;

	VoiceCrypto::eMode mode;
	uint8_t key[VoiceCrypto::KEY_SIZE];
	uint32_t ssrc;
	{
		VoiceAutoLock lock(m_lock);
		if (!m_bHaveKey)
			return;

		mode = m_cryptoMode;
		memcpy(key, m_secretKey, sizeof key);
		ssrc = m_ssrc;
	}

	// End-to-end encryption comes first, then the transport encryption wraps it.
	std::vector<uint8_t> frame;
	if (!m_dave.Encrypt(ssrc, data, size, frame))
		return;

	uint8_t header[VoiceCrypto::RTP_HEADER_SIZE];
	header[0] = 0x80;
	header[1] = OPUS_PAYLOAD_TYPE;
	WriteBE16(header + 2, m_rtpSeq);
	WriteBE32(header + 4, m_rtpTimestamp);
	WriteBE32(header + 8, ssrc);

	m_rtpSeq++;
	m_rtpTimestamp += VoiceAudioIO::FRAME_SAMPLES;

	std::vector<uint8_t> packet;
	if (!VoiceCrypto::EncryptPacket(mode, key, m_nonceCounter++, header, frame.data(), frame.size(), packet))
		return;

	send(sock, (const char*)packet.data(), int(packet.size()), 0);
}

// ---- Control thread ----

void VoiceConnection::ResetSession()
{
	// Throw away what belongs to the old session in order to identify from scratch.
	CloseSocket();
	m_receiveThread.Join();

	{
		VoiceAutoLock lock(m_lock);
		m_bHaveKey = false;
		m_ssrcToUser.clear();
	}

	ClearStreams();
	m_bHaveSession = false;
	m_lastSeq = -1;
}

void VoiceConnection::HandleWebsocketLoss()
{
	int code;
	std::string reason;
	{
		VoiceAutoLock lock(m_lock);
		if (!m_bWsLost)
			return;

		code = m_wsCloseCode;
		reason = m_wsCloseReason;
		m_bWsLost = false;
	}

	if (m_bFinished || m_bStopRequested)
		return;

	switch (code)
	{
		case VoiceClose::DISCONNECTED:
		case VoiceClose::CALL_TERMINATED:
			// Kicked, moved, the channel is gone ...  Nothing for us to do any longer.
			Finish(false, "");
			return;

		case VoiceClose::DAVE_REQUIRED:
			Finish(true, "The voice server requires end-to-end encryption, which this build doesn't manage to negotiate.");
			return;

		case VoiceClose::AUTHENTICATION_FAILED:
		case VoiceClose::SESSION_INVALID:
		case VoiceClose::UNKNOWN_ENCRYPTION:
			Finish(true, "The voice server refused the connection (code " + std::to_string(code) + ").");
			return;
	}

	if (m_reconnectAttempts >= MAX_RECONNECT_ATTEMPTS) {
		Finish(true, "Lost the connection to the voice server (" + reason + ").");
		return;
	}

	// Try again soon, with increasing patience.
	SetStatus(STATUS_RECONNECTING);
	m_bReconnectPending = true;
	m_nextReconnectTick = GetTickCount() + 500 + 1000 * m_reconnectAttempts;
	m_reconnectAttempts++;
}

void VoiceConnection::ControlThreadProc()
{
	while (!m_bStopRequested)
	{
		WaitForSingleObject(m_hWake, 50);
		if (m_bStopRequested)
			break;

		HandleWebsocketLoss();

		DWORD now = GetTickCount();

		if (m_bReconnectPending && int(now - m_nextReconnectTick) >= 0)
		{
			m_bReconnectPending = false;

			// Resuming keeps the media going.  The first attempts try that, if possible; failing
			// that, start over.
			bool bResume = m_bHaveSession && m_reconnectAttempts <= 2;
			if (!bResume)
				ResetSession();

			if (!ConnectWebsocket(bResume))
				Finish(true, "Could not reconnect to the voice server.");
		}

		if (m_heartbeatInterval > 0 && m_ws && m_ws->IsOpen())
		{
			if (int(now - m_lastHeartbeatTick) >= m_heartbeatInterval)
			{
				m_lastHeartbeatTick = now;
				SendHeartbeat();
			}

			// A server that stopped answering is as good as gone.
			if (int(now - m_lastAckTick) > m_heartbeatInterval * 2 + HEARTBEAT_ACK_GRACE_MS)
			{
				m_lastAckTick = now;
				m_ws->Close(1001);
			}
		}

		if (m_socket != INVALID_SOCKET && int(now - m_lastKeepAliveTick) >= KEEPALIVE_INTERVAL_MS)
		{
			m_lastKeepAliveTick = now;
			SendKeepAlive();
		}

		// Forget participants that have been quiet for a long time.
		std::vector<uint32_t> stale;
		{
			VoiceAutoLock lock(m_streamLock);
			for (auto& pair : m_streams) {
				if (now - pair.second->m_lastPacketTick > STREAM_TIMEOUT_MS)
					stale.push_back(pair.first);
			}
		}
		for (uint32_t ssrc : stale)
			RemoveStream(ssrc);
	}
}
