#pragma once

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "state/VoiceState.hpp"
#include "VoiceSync.hpp"
#include "VoiceAudio.hpp"
#include "VoiceCrypto.hpp"
#include "VoiceDave.hpp"
#include "VoiceWebsocket.hpp"

struct OpusEncoder;
struct OpusDecoder;

// One connection to a Discord voice server: the websocket for signalling, the UDP socket the
// audio travels on, and the microphone and speakers.
//
// Threads involved:
//   - the I/O thread of the VoiceWebsocket, which handles the signalling messages,
//   - the control thread, which does the periodic things (heartbeats, keep alives) and reconnects,
//   - the UDP receive thread,
//   - the audio thread of the VoiceAudioIO, which encodes and sends what the microphone picked up,
//     and decodes and mixes what comes from the other participants.
class VoiceConnection : public VoiceWebsocket::Listener, public VoiceAudioIO::Listener
{
public:
	enum eStatus
	{
		STATUS_CONNECTING,   // establishing the websocket
		STATUS_HANDSHAKING,  // authenticating, finding out how to reach the media server
		STATUS_CONNECTED,
		STATUS_RECONNECTING,
		STATUS_STOPPED,      // ended by request, or by the server (kicked, call ended, ...)
		STATUS_FAILED,       // ended because of an error, see GetErrorText
	};

	// Called when the connection ended by itself (not because of Stop), from some thread.
	typedef void (*StoppedCallback)(unsigned generation, bool bFailed);

public:
	VoiceConnection(
		const VoiceConnectParams& params,
		const std::set<Snowflake>& usersInChannel,
		unsigned generation,
		StoppedCallback pfnStopped
	);
	~VoiceConnection();

	bool Start();

	// Asks the connection to wind down.  The destructor finishes the job.
	void Stop();

	void SetMuted(bool muted)       { m_bMuted = muted; }
	void SetDeafened(bool deafened) { m_bDeafened = deafened; }

	unsigned GetGeneration() const { return m_generation; }
	eStatus GetStatus() const { return eStatus(m_status); }
	std::string GetErrorText();

	// Users who are talking right now (ourselves included).
	void GetSpeakingUsers(std::set<Snowflake>& users);

	// Whether we're able to speak and hear, as opposed to being busy connecting.
	bool HasMicrophone() const { return m_audio.HasCapture(); }
	bool HasSpeakers() const { return m_audio.HasPlayback(); }

private:
	struct RemoteStream;

	// ---- VoiceWebsocket::Listener ----
	virtual void OnWsOpen();
	virtual void OnWsMessage(const std::string& payload, bool bBinary);
	virtual void OnWsClosed(int closeCode, const std::string& reason);

	// ---- VoiceAudioIO::Listener ----
	virtual void OnCapturedFrame(const int16_t* samples);
	virtual void OnPlaybackFrame(int16_t* samples);

	// Signalling
	bool ConnectWebsocket(bool bResume);
	void HandleJsonMessage(const std::string& payload);
	void HandleBinaryMessage(const std::string& payload);
	void HandleHello(nlohmann::json& d);
	void HandleReady(nlohmann::json& d);
	void HandleSessionDescription(nlohmann::json& d);
	void HandleSpeaking(nlohmann::json& d);
	void HandleClientsConnect(nlohmann::json& d);
	void HandleClientDisconnect(nlohmann::json& d);
	void SendIdentify();
	void SendResume();
	void SendSelectProtocol(const std::string& address, int port);
	void SendHeartbeat();
	void SendSpeaking(bool bSpeaking);
	void SendDaveOutput(const VoiceDave::Output& out);
	void SendJson(const nlohmann::json& j);

	// Transport
	bool OpenSocket(const std::string& ip, int port);
	bool DiscoverExternalAddress(std::string& address, int& port);
	void CloseSocket();
	void SendKeepAlive();
	void ReceiveThreadProc();
	void HandleUdpPacket(const uint8_t* data, size_t size);

	// Media
	void SendOpusFrame(const uint8_t* data, size_t size);
	void SendSilence();
	bool CreateCodec();
	void DestroyCodec();
	void PushRemotePacket(uint32_t ssrc, Snowflake user, uint16_t seq, const std::vector<uint8_t>& opus);
	void RemoveStream(uint32_t ssrc);
	void ClearStreams();

	// Control
	void ControlThreadProc();
	void HandleWebsocketLoss();
	void ResetSession();
	void SetStatus(eStatus status);
	void SetError(const std::string& text);
	void Finish(bool bFailed, const std::string& error);

private:
	VoiceConnectParams m_params;
	unsigned m_generation;
	StoppedCallback m_pfnStopped;

	volatile long m_status;
	volatile bool m_bStopRequested;
	volatile bool m_bMuted;
	volatile bool m_bDeafened;
	volatile bool m_bFinished;

	VoiceLock m_lock; // protects the session data and the error text
	std::string m_errorText;

	// Signalling state
	VoiceLock m_wsLock;                // guards the m_ws pointer, which is replaced when reconnecting
	std::unique_ptr<VoiceWebsocket> m_ws;
	volatile long m_lastSeq;           // last sequence number seen from the server, -1 if none
	int m_heartbeatInterval;           // ms
	DWORD m_lastHeartbeatTick;
	DWORD m_lastAckTick;
	bool m_bResuming;
	bool m_bHaveSession;               // Session Description arrived, so resuming is possible
	bool m_bWsLost;                    // set by OnWsClosed, handled by the control thread
	int m_wsCloseCode;
	std::string m_wsCloseReason;
	int m_reconnectAttempts;
	DWORD m_nextReconnectTick;
	bool m_bReconnectPending;

	// Session data
	uint32_t m_ssrc;
	std::string m_serverIp;
	int m_serverPort;
	std::vector<std::string> m_offeredModes;
	VoiceCrypto::eMode m_cryptoMode;
	uint8_t m_secretKey[VoiceCrypto::KEY_SIZE];
	bool m_bHaveKey;
	std::set<Snowflake> m_users;
	std::map<uint32_t, Snowflake> m_ssrcToUser;

	// Transport
	SOCKET m_socket;
	VoiceThread<VoiceConnection> m_receiveThread;
	DWORD m_lastKeepAliveTick;
	uint32_t m_keepAliveCounter;

	// Outgoing media (touched only by the audio thread, apart from the reset)
	OpusEncoder* m_encoder;
	uint16_t m_rtpSeq;
	uint32_t m_rtpTimestamp;
	uint32_t m_nonceCounter;
	int m_hangoverFrames;
	bool m_bSpeakingSent;
	volatile bool m_bTransmitting;

	// Incoming media
	VoiceLock m_streamLock;
	std::map<uint32_t, RemoteStream*> m_streams;

	VoiceDave m_dave;
	VoiceAudioIO m_audio;
	bool m_bAudioStarted;

	VoiceThread<VoiceConnection> m_controlThread;
	HANDLE m_hWake;
};
