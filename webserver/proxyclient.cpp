#include "stdafx.h"
#include "proxyclient.h"
#include "request.hpp"
#include "reply.hpp"
#include "request_parser.hpp"
#include "../main/SQLHelper.h"
#include "../webserver/Base64.h"

extern std::string szAppVersion;
static std::string _instanceid;
#undef WITH_MUTEX
#ifdef WITH_MUTEX
static boost::mutex prefs_mutex;
#endif

namespace http {
	namespace server {

		CProxyClient::CProxyClient(boost::asio::io_service& io_service, boost::asio::ssl::context& context, http::server::cWebem *webEm)
			: _socket(io_service, context),
			_io_service(io_service),
			doStop(false)
		{
			_apikey = "";
			_instanceid = "";
			_password = "";
			m_sql.GetPreferencesVar("MyDomoticzInstanceId", _instanceid);
			m_sql.GetPreferencesVar("MyDomoticzUserId", _apikey);
			m_sql.GetPreferencesVar("MyDomoticzPassword", _password);
			if (_password != "") {
				_password = base64_decode(_password);
			}
			if (_apikey == "" || _password == "") {
				doStop = true;
				return;
			}
			m_pWebEm = webEm;
			Reconnect();
		}

		void CProxyClient::Reconnect()
		{

			std::string address = "my.domoticz.com";
			std::string port = "9999";

			boost::asio::ip::tcp::resolver resolver(_io_service);
			boost::asio::ip::tcp::resolver::query query(address, port);
			boost::asio::ip::tcp::resolver::iterator iterator = resolver.resolve(query);
			boost::asio::ip::tcp::endpoint endpoint = *iterator;
			_socket.lowest_layer().async_connect(endpoint,
				boost::bind(&CProxyClient::handle_connect, this,
					boost::asio::placeholders::error, iterator));
		}

		void CProxyClient::handle_connect(const boost::system::error_code& error, boost::asio::ip::tcp::resolver::iterator endpoint_iterator)
		{
			if (!error)
			{
				_socket.async_handshake(boost::asio::ssl::stream_base::client,
					boost::bind(&CProxyClient::handle_handshake, this,
						boost::asio::placeholders::error));
			}
			else if (endpoint_iterator != boost::asio::ip::tcp::resolver::iterator())
			{
				_socket.lowest_layer().close();
				boost::asio::ip::tcp::endpoint endpoint = *endpoint_iterator;
				_socket.lowest_layer().async_connect(endpoint,
					boost::bind(&CProxyClient::handle_connect, this,
						boost::asio::placeholders::error, ++endpoint_iterator));
			}
			else
			{
				_log.Log(LOG_ERROR, "PROXY: Connect failed, reconnecting: %s", error.message().c_str());
				if (!doStop) {
					boost::this_thread::sleep_for(boost::chrono::seconds(10));
					Reconnect();
				}
			}
		}

		void CProxyClient::MyWrite(pdu_type type, CValueLengthPart *parameters)
		{
			_writebuf.clear();
			writePdu = new ProxyPdu(type, parameters);

			_writebuf.push_back(boost::asio::buffer(writePdu->content(), writePdu->length()));

			boost::asio::async_write(_socket, _writebuf,
				boost::bind(&CProxyClient::handle_write, this,
					boost::asio::placeholders::error,
					boost::asio::placeholders::bytes_transferred));
		}

		void CProxyClient::LoginToService()
		{
			// send authenticate pdu
			int subsystems = 1;
			CValueLengthPart parameters;
			parameters.AddPart((void *)_apikey.c_str(), _apikey.length() + 1);
			parameters.AddPart((void *)_instanceid.c_str(), _instanceid.length() + 1);
			parameters.AddPart((void *)_password.c_str(), _password.length() + 1);
			parameters.AddPart((void *)szAppVersion.c_str(), szAppVersion.length() + 1);
			parameters.AddValue((void *)&subsystems, SIZE_INT);
			MyWrite(PDU_AUTHENTICATE, &parameters);
		}

		void CProxyClient::handle_handshake(const boost::system::error_code& error)
		{
			if (!error)
			{
#ifdef WITH_MUTEX
				// lock until we have a valid api id
				prefs_mutex.lock();
#endif
				LoginToService();
			}
			else
			{
				_log.Log(LOG_ERROR, "PROXY: Handshake failed, reconnecting: %s", error.message().c_str());
				_socket.lowest_layer().close();
				Reconnect();
			}
		}

		void CProxyClient::ReadMore()
		{
			// read chunks of max 4 KB
			boost::asio::streambuf::mutable_buffers_type buf = _readbuf.prepare(4096);
			_socket.async_read_some(buf,
				boost::bind(&CProxyClient::handle_read, this, boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred)
				);
		}

		void CProxyClient::handle_write(const boost::system::error_code& error, size_t bytes_transferred)
		{
			delete writePdu;
			if (!error)
			{
				// Write complete. Reading pdu.
				ReadMore();
			}
			else
			{
				_log.Log(LOG_ERROR, "PROXY: Write failed: %s", error.message().c_str());
			}
		}

		void CProxyClient::GetRequest(const std::string originatingip, boost::asio::mutable_buffers_1 _buf, http::server::reply &reply_)
		{
			/// The parser for the incoming request.
			http::server::request_parser request_parser_;
			http::server::request request_;

			boost::tribool result;
			try
			{
				size_t bufsize = boost::asio::buffer_size(_buf);
				const char *begin = boost::asio::buffer_cast<const char*>(_buf);
				boost::tie(result, boost::tuples::ignore) = request_parser_.parse(
					request_, begin, begin + bufsize);
			}
			catch (...)
			{
				_log.Log(LOG_ERROR, "PROXY: Exception during request parsing");
			}

			if (result)
			{
				request_.host = originatingip;
				m_pWebEm->myRequestHandler.handle_request(request_, reply_);
			}
			else if (!result)
			{
				reply_ = http::server::reply::stock_reply(http::server::reply::bad_request);
			}
			else
			{
				_log.Log(LOG_ERROR, "Could not parse request.");
			}
		}

		std::string CProxyClient::GetResponseHeaders(const http::server::reply &reply_)
		{
			std::string result = "";
			for (std::size_t i = 0; i < reply_.headers.size(); ++i) {
				result += reply_.headers[i].name;
				result += ": ";
				result += reply_.headers[i].value;
				result += "\r\n";
			}
			return result;
		}

		void CProxyClient::HandleRequest(ProxyPdu *pdu)
		{
			// response variables
			boost::asio::mutable_buffers_1 _buf(NULL, 0);
			/// The reply to be sent back to the client.
			http::server::reply reply_;

			// get common parts for the different subsystems
			CValueLengthPart part(pdu);

			// request parts
			char *originatingip;
			int subsystem;
			size_t thelen;
			std::string responseheaders;
			std::string request;
			char *requesturl;
			char *requestheaders;
			char *requestbody;
			size_t bodylen;
			CValueLengthPart parameters; // response parameters

			if (!part.GetNextPart((void **)&originatingip, &thelen)) {
				_log.Log(LOG_ERROR, "PROXY: Invalid request");
				return;
			}
			if (!part.GetNextValue((void **)&subsystem, &thelen)) {
				free(originatingip);
				_log.Log(LOG_ERROR, "PROXY: Invalid request");
				return;
			}

			switch (subsystem) {
			case 1:
				// "normal web request", get parameters
				if (!part.GetNextPart((void **)&requesturl, &thelen)) {
					_log.Log(LOG_ERROR, "PROXY: Invalid request");
					free(originatingip);
					return;
				}
				if (!part.GetNextPart((void **)&requestheaders, &thelen)) {
					_log.Log(LOG_ERROR, "PROXY: Invalid request");
					free(originatingip);
					free(requesturl);
					return;
				}
				if (!part.GetNextPart((void **)&requestbody, &bodylen)) {
					_log.Log(LOG_ERROR, "PROXY: Invalid request");
					free(originatingip);
					free(requesturl);
					free(requestheaders);
					return;
				}

				if (bodylen > 0) {
					request = "POST ";
				}
				else {
					request = "GET ";
				}
				request += requesturl;
				request += " HTTP/1.1\r\n";
				request += requestheaders;
				request += "\r\n";
				request += std::string(requestbody, bodylen);

				_buf = boost::asio::buffer((void *)request.c_str(), request.length());

				if (connectedips_.find(originatingip) == connectedips_.end())
				{
					//ok, this could get a very long list when running for years
					connectedips_.insert(originatingip);
					_log.Log(LOG_STATUS, "PROXY: Incoming connection from: %s", originatingip);
				}

				GetRequest(originatingip, _buf, reply_);
				free(originatingip);
				free(requesturl);
				free(requestheaders);
				free(requestbody);

				// assemble response
				responseheaders = GetResponseHeaders(reply_);


				parameters.AddValue((void *)&reply_.status, SIZE_INT);
				parameters.AddPart((void *)responseheaders.c_str(), responseheaders.length() + 1);
				parameters.AddPart((void *)reply_.content.c_str(), reply_.content.size());
				break;
			default:
				// unknown subsystem
				_log.Log(LOG_ERROR, "PROXY: Got pdu for unknown subsystem %d.", subsystem);
				break;
			}

			// send response to proxy
			MyWrite(PDU_RESPONSE, &parameters);
		}

		void CProxyClient::HandleAssignkey(ProxyPdu *pdu)
		{
			// get our new api key
			CValueLengthPart parameters(pdu);
			char *newapi;
			size_t newapilen;

			if (!parameters.GetNextPart((void **)&newapi, &newapilen)) {
				_log.Log(LOG_ERROR, "PROXY: Invalid request while obtaining API key");
				return;
			}
			_log.Log(LOG_NORM, "PROXY: We were assigned an instance id: %s.\n", newapi);
			_instanceid = newapi;
			m_sql.UpdatePreferencesVar("MyDomoticzInstanceId", _instanceid);
			free(newapi);
			// re-login with the new instance id
			LoginToService();
		}

		void CProxyClient::HandleEnquire(ProxyPdu *pdu)
		{
			// assemble response
			CValueLengthPart parameters;

			// send response to proxy
			MyWrite(PDU_ENQUIRE, &parameters);
		}

		void CProxyClient::HandleAuthresp(ProxyPdu *pdu)
		{
			// get auth response (0 or 1)
			size_t authlen, reasonlen;
			int auth;
			char *reason;
			CValueLengthPart part(pdu);

#ifdef WITH_MUTEX
			// unlock prefs mutex
			prefs_mutex.unlock();
#endif
			if (!part.GetNextValue((void **)&auth, &authlen)) {
				_log.Log(LOG_ERROR, "PROXY: Invalid pdu while receiving authentication response");
				return;
			}
			if (!part.GetNextPart((void **)&reason, &reasonlen)) {
				_log.Log(LOG_ERROR, "PROXY: Invalid pdu while receiving authentication response");
				return;
			}
			_log.Log(LOG_STATUS, "PROXY: Authenticate result: %s.", auth ? "success" : reason);
			free(reason);
			if (!auth) {
				Stop();
				return;
			}
			ReadMore();
		}

		void CProxyClient::handle_read(const boost::system::error_code& error, size_t bytes_transferred)
		{
			if (!error)
			{
				_readbuf.commit(bytes_transferred);
				const char *data = boost::asio::buffer_cast<const char*>(_readbuf.data());
				ProxyPdu pdu(data, _readbuf.size());
				if (pdu.Disconnected()) {
					ReadMore();
					return;
				}
				_readbuf.consume(pdu.length() + 9); // 9 is header size

				switch (pdu._type) {
				case PDU_REQUEST:
					HandleRequest(&pdu);
					break;
				case PDU_ASSIGNKEY:
					HandleAssignkey(&pdu);
					break;
				case PDU_ENQUIRE:
					HandleEnquire(&pdu);
					break;
				case PDU_AUTHRESP:
					HandleAuthresp(&pdu);
					break;
				default:
					_log.Log(LOG_ERROR, "PROXY: pdu type: %d not expected.", pdu._type);
					ReadMore();
					break;
				}
			}
			else
			{
				_log.Log(LOG_ERROR, "PROXY: Read failed, reconnecting: %s", error.message().c_str());
				// Initiate graceful connection closure.
				_socket.lowest_layer().close();
				if (doStop) {
					return;
				}
				// we are disconnected, reconnect
				boost::this_thread::sleep_for(boost::chrono::seconds(10));
				Reconnect();
			}
		}

		void CProxyClient::Stop()
		{
#ifdef WITH_MUTEX
			// todo: check if this gives the proper results
			while (!prefs_mutex.try_lock()) {
			}
			prefs_mutex.unlock();
#endif
			doStop = true;
			_socket.lowest_layer().close();
		}

		CProxyClient::~CProxyClient()
		{
		}

		CProxyManager::CProxyManager(const std::string& doc_root, http::server::cWebem *webEm)
		{
			proxyclient = NULL;
			m_pWebEm = webEm;
		}

		CProxyManager::~CProxyManager()
		{
			//end_mutex.lock();
			// todo: throws access violation
			if (proxyclient) delete proxyclient;
		}

		int CProxyManager::Start()
		{
			end_mutex.lock();
			m_thread = new boost::thread(boost::bind(&CProxyManager::StartThread, this));
			return 1;
		}

		void CProxyManager::StartThread()
		{
			try {
				boost::asio::ssl::context ctx(io_service, boost::asio::ssl::context::sslv23);
				ctx.set_verify_mode(boost::asio::ssl::verify_none);

				proxyclient = new CProxyClient(io_service, ctx, m_pWebEm);

				io_service.run();
			}
			catch (std::exception& e)
			{
				_log.Log(LOG_ERROR, "PROXY: StartThread(): Exception: %s", e.what());
			}
		}

		void CProxyManager::Stop()
		{
			proxyclient->Stop();
			io_service.stop();
			m_thread->interrupt();
			m_thread->join();
		}

	}
}