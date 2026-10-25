#pragma once

#include <string>
#include "network/WebsocketClient.hpp"

// A secure websocket connection to a Discord voice gateway.
//
// Unlike the WebsocketClient used for the main gateway, every VoiceWebsocket has an I/O thread of
// its own, so it can be created and thrown away along with the voice session.  Callbacks arrive on
// that thread, and must not destroy the VoiceWebsocket they came from.
class VoiceWebsocket
{
public:
	class Listener
	{
	public:
		virtual ~Listener() {}

		virtual void OnWsOpen() = 0;
		virtual void OnWsMessage(const std::string& payload, bool bBinary) = 0;

		// The connection ended, or couldn't be established in the first place (in which case the
		// code is zero).  Called once at most.
		virtual void OnWsClosed(int closeCode, const std::string& reason) = 0;
	};

public:
	VoiceWebsocket();
	~VoiceWebsocket();

	// Starts connecting to wss://host/path.  The result is reported through the listener.
	bool Connect(const std::string& host, const std::string& path, Listener* pListener);

	void SendText(const std::string& text);
	void SendBinary(const uint8_t* data, size_t size);

	// Starts closing the connection.
	void Close(int code = websocketpp::close::status::normal);

	bool IsOpen() const { return m_bOpen; }

private:
	void Shutdown();
	void RunThread();

	AsioSslContextSharedPtr HandleTLSInit(websocketpp::connection_hdl hdl);
	void HandleSocketInit(websocketpp::connection_hdl hdl, AsioSocketType& socket);
	void HandleOpen(websocketpp::connection_hdl hdl);
	void HandleFail(websocketpp::connection_hdl hdl);
	void HandleClose(websocketpp::connection_hdl hdl);
	void HandleMessage(websocketpp::connection_hdl hdl, WSClient::message_ptr msg);

private:
	WSClient m_endpoint;
	WSThreadSharedPtr m_thread;
	websocketpp::connection_hdl m_hdl;
	Listener* m_pListener;
	volatile bool m_bOpen;
	volatile bool m_bRunDone;
	volatile bool m_bReported;
	bool m_bStarted;
};
