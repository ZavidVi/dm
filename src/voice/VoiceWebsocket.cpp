#include "VoiceWebsocket.hpp"
#include "config/DiscordClientConfig.hpp"
#include "config/LocalSettings.hpp"
#include "utils/Util.hpp"

#include <asio/ssl/context.hpp>

#ifdef _WIN32
void LoadSystemCertsOnWindows(SSL_CTX* ctx); // NetworkerThread.cpp
#endif

VoiceWebsocket::VoiceWebsocket() :
	m_pListener(nullptr),
	m_bOpen(false),
	m_bRunDone(false),
	m_bReported(false),
	m_bStarted(false)
{
}

VoiceWebsocket::~VoiceWebsocket()
{
	Shutdown();
}

AsioSslContextSharedPtr VoiceWebsocket::HandleTLSInit(websocketpp::connection_hdl hdl)
{
	AsioSslContextSharedPtr ctx = std::make_shared<AsioSslContext>(AsioSslContext::tls);

	try
	{
		ctx->set_options(
			websocketpp::lib::asio::ssl::context::default_workarounds |
			websocketpp::lib::asio::ssl::context::single_dh_use
		);

		if (GetLocalSettings()->EnableTLSVerification())
		{
			ctx->set_default_verify_paths();
#ifdef _WIN32
			LoadSystemCertsOnWindows(ctx->native_handle());
#endif
		}
	}
	catch (std::exception& e)
	{
		DbgPrintF("VoiceWebsocket::HandleTLSInit: Error in context pointer: %s", e.what());
	}

	return ctx;
}

void VoiceWebsocket::HandleSocketInit(websocketpp::connection_hdl hdl, AsioSocketType& socket)
{
	if (!GetLocalSettings()->EnableTLSVerification())
		return;

	WSClient::connection_ptr pConn = m_endpoint.get_con_from_hdl(hdl);
	std::string host = pConn->get_uri()->get_host();

	socket.set_verify_mode(websocketpp::lib::asio::ssl::verify_peer);

	if (!SSL_set_tlsext_host_name(reinterpret_cast<SSL*>(socket.native_handle()), host.c_str()))
		DbgPrintF("VoiceWebsocket: Failed to set SNI host name... this might go awry");
}

void VoiceWebsocket::HandleOpen(websocketpp::connection_hdl hdl)
{
	m_bOpen = true;

	Listener* pListener = m_pListener;
	if (pListener)
		pListener->OnWsOpen();
}

void VoiceWebsocket::HandleFail(websocketpp::connection_hdl hdl)
{
	m_bOpen = false;

	WSClient::connection_ptr pConn = m_endpoint.get_con_from_hdl(hdl);
	std::string reason = pConn->get_ec().message();

	auto transportEc = pConn->get_transport_ec();
	if (transportEc)
		reason += " (" + transportEc.message() + ")";

	DbgPrintF("VoiceWebsocket: connection failed: %s", reason.c_str());

	Listener* pListener = m_pListener;
	if (!m_bReported && pListener) {
		m_bReported = true;
		pListener->OnWsClosed(0, reason);
	}
}

void VoiceWebsocket::HandleClose(websocketpp::connection_hdl hdl)
{
	m_bOpen = false;

	WSClient::connection_ptr pConn = m_endpoint.get_con_from_hdl(hdl);
	int code = pConn->get_remote_close_code();
	std::string reason = pConn->get_remote_close_reason();

	DbgPrintF("VoiceWebsocket: closed with code %d (%s)", code, reason.c_str());

	Listener* pListener = m_pListener;
	if (!m_bReported && pListener) {
		m_bReported = true;
		pListener->OnWsClosed(code, reason);
	}
}

void VoiceWebsocket::HandleMessage(websocketpp::connection_hdl hdl, WSClient::message_ptr msg)
{
	Listener* pListener = m_pListener;
	if (!pListener)
		return;

	bool bBinary = msg->get_opcode() == websocketpp::frame::opcode::binary;
	pListener->OnWsMessage(msg->get_payload(), bBinary);
}

void VoiceWebsocket::RunThread()
{
	m_endpoint.run();
	m_bRunDone = true;
}

bool VoiceWebsocket::Connect(const std::string& host, const std::string& path, Listener* pListener)
{
	if (m_bStarted)
		return false;

	m_pListener = pListener;

	m_endpoint.clear_access_channels(websocketpp::log::alevel::all);
	m_endpoint.clear_error_channels(websocketpp::log::elevel::all);
	m_endpoint.init_asio();
	m_endpoint.start_perpetual();

	m_endpoint.set_tls_init_handler(websocketpp::lib::bind(
		&VoiceWebsocket::HandleTLSInit, this, websocketpp::lib::placeholders::_1));
	m_endpoint.set_socket_init_handler(websocketpp::lib::bind(
		&VoiceWebsocket::HandleSocketInit, this,
		websocketpp::lib::placeholders::_1, websocketpp::lib::placeholders::_2));

	std::string uri = "wss://" + host + path;
	websocketpp::lib::error_code ec;
	WSClient::connection_ptr con = m_endpoint.get_connection(uri, ec);
	if (ec) {
		DbgPrintF("VoiceWebsocket: could not initialize connection to %s: %s", uri.c_str(), ec.message().c_str());
		m_endpoint.stop_perpetual();
		return false;
	}

	con->append_header("User-Agent", GetClientConfig()->GetUserAgent());
	con->append_header("Origin", "https://discord.com");

	m_hdl = con->get_handle();

	con->set_open_handler(websocketpp::lib::bind(
		&VoiceWebsocket::HandleOpen, this, websocketpp::lib::placeholders::_1));
	con->set_fail_handler(websocketpp::lib::bind(
		&VoiceWebsocket::HandleFail, this, websocketpp::lib::placeholders::_1));
	con->set_close_handler(websocketpp::lib::bind(
		&VoiceWebsocket::HandleClose, this, websocketpp::lib::placeholders::_1));
	con->set_message_handler(websocketpp::lib::bind(
		&VoiceWebsocket::HandleMessage, this,
		websocketpp::lib::placeholders::_1, websocketpp::lib::placeholders::_2));

	m_endpoint.connect(con);

	m_bStarted = true;
	m_thread.reset(new websocketpp::lib::thread(&VoiceWebsocket::RunThread, this));
	return true;
}

void VoiceWebsocket::SendText(const std::string& text)
{
	if (!m_bOpen)
		return;

	websocketpp::lib::error_code ec;
	m_endpoint.send(m_hdl, text, websocketpp::frame::opcode::text, ec);
	if (ec)
		DbgPrintF("VoiceWebsocket: failed to send text message: %s", ec.message().c_str());
}

void VoiceWebsocket::SendBinary(const uint8_t* data, size_t size)
{
	if (!m_bOpen)
		return;

	websocketpp::lib::error_code ec;
	m_endpoint.send(m_hdl, data, size, websocketpp::frame::opcode::binary, ec);
	if (ec)
		DbgPrintF("VoiceWebsocket: failed to send binary message: %s", ec.message().c_str());
}

void VoiceWebsocket::Close(int code)
{
	if (!m_bStarted)
		return;

	websocketpp::lib::error_code ec;
	m_endpoint.close(m_hdl, code, "", ec);
}

void VoiceWebsocket::Shutdown()
{
	if (!m_bStarted)
		return;

	// Nobody wants to hear about this anymore.
	m_pListener = nullptr;

	m_endpoint.stop_perpetual();

	if (m_bOpen)
	{
		websocketpp::lib::error_code ec;
		m_endpoint.close(m_hdl, websocketpp::close::status::going_away, "", ec);
	}

	// Give the closing handshake a moment, but don't wait forever for a dead server.
	for (int i = 0; i < 50 && !m_bRunDone; i++)
		Sleep(10);

	if (!m_bRunDone)
		m_endpoint.stop();

	if (m_thread)
		m_thread->join();

	m_thread.reset();
	m_bStarted = false;
	m_bOpen = false;
}
