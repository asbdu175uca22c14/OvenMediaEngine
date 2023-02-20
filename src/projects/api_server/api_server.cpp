//==============================================================================
//
//  OvenMediaEngine
//
//  Created by Hyunjun Jang
//  Copyright (c) 2020 AirenSoft. All rights reserved.
//
//==============================================================================
#include "api_server.h"

#include <base/info/ome_version.h>
#include <orchestrator/orchestrator.h>
#include <sys/utsname.h>

#include "api_private.h"
#include "controllers/root_controller.h"

#define API_VERSION "1"

namespace api
{
	struct XmlWriter : pugi::xml_writer
	{
		ov::String result;

		void write(const void *data, size_t size) override
		{
			result.Append(static_cast<const char *>(data), size);
		}
	};

	bool Server::PrepareHttpServers(const std::vector<ov::String> &server_ip_list,
									const bool is_port_configured, const uint16_t port,
									const bool is_tls_port_configured, const uint16_t tls_port,
									const cfg::mgr::Managers &managers,
									const int worker_count)
	{
		auto http_server_manager = http::svr::HttpServerManager::GetInstance();
		auto http_interceptor = CreateInterceptor();

		auto certificate = is_tls_port_configured ? info::Certificate::CreateCertificate("api_server", managers.GetHost().GetNameList(), managers.GetHost().GetTls()) : nullptr;

		for (const auto &server_ip : server_ip_list)
		{
			std::vector<ov::SocketAddress> address_list;
			std::vector<ov::SocketAddress> tls_address_list;

			try
			{
				address_list = is_port_configured ? ov::SocketAddress::Create(server_ip, port) : std::vector<ov::SocketAddress>();
				tls_address_list = (is_tls_port_configured && (certificate != nullptr)) ? ov::SocketAddress::Create(server_ip, tls_port) : std::vector<ov::SocketAddress>();
			}
			catch (const ov::Error &e)
			{
				logte("Could not listen for API Server: %s", e.What());
				return false;
			}

			std::vector<ov::String> address_string_list;
			std::vector<ov::String> tls_address_string_list;

			for (const auto &address : address_list)
			{
				logtd("Attempting to create HTTP Server instance on %s...", address.ToString().CStr());
				auto http_server = http_server_manager->CreateHttpServer("APISvr", address, worker_count);

				if (http_server != nullptr)
				{
					http_server->AddInterceptor(http_interceptor);
					_http_server_list.push_back(http_server);
					address_string_list.emplace_back(address.ToString());
				}
				else
				{
					logte("Could not initialize HTTP Server on %s", address.ToString().CStr());
					return false;
				}
			}

			for (const auto &tls_address : tls_address_list)
			{
				logtd("Attempting to create HTTPS Server instance on %s...", tls_address.ToString().CStr());
				auto https_server = http_server_manager->CreateHttpsServer("APISvr", tls_address, certificate, false, worker_count);

				if (https_server != nullptr)
				{
					https_server->AddInterceptor(http_interceptor);
					_https_server_list.push_back(https_server);
					tls_address_string_list.emplace_back(tls_address.ToString());
				}
				else
				{
					logte("Could not initialize HTTPS Server on %s", tls_address.ToString().CStr());
					return false;
				}
			}

			auto tls_description = ov::String::Join(tls_address_string_list, ", ");

			if (tls_description.IsEmpty() == false)
			{
				tls_description.Prepend("(TLS: ");
				tls_description.Append(')');
			}

			logti("API Server is listening on %s%s...",
				  ov::String::Join(address_string_list, ", ").CStr(),
				  tls_address_list.empty() ? "" : tls_description.CStr());
		}

		return true;
	}

	void Server::SetupCors(const cfg::mgr::api::API &api_config)
	{
		bool is_cors_parsed;
		auto cross_domains = api_config.GetCrossDomainList(&is_cors_parsed);

		if (is_cors_parsed)
		{
			// API server doesn't have VHost, so use dummy VHost
			auto vhost_app_name = info::VHostAppName::InvalidVHostAppName();
			_cors_manager.SetCrossDomains(vhost_app_name, cross_domains);
		}
	}

	bool Server::SetupAccessToken(const cfg::mgr::api::API &api_config)
	{
		_access_token = api_config.GetAccessToken();

		if (_access_token.IsEmpty())
		{
#if DEBUG
			logtw("An empty <AccessToken> setting was found. This is only allowed on Debug builds for ease of development, and the Release build does not allow empty <AccessToken>.");
#else	// DEBUG
			logte("Empty <AccessToken> is not allowed");
			return false;
#endif	// DEBUG
		}

		return true;
	}

	bool Server::Start(const std::shared_ptr<const cfg::Server> &server_config)
	{
		// API Server configurations
		const auto &managers_config = server_config->GetManagers();
		const auto &api_config = managers_config.GetApi();

		// Port configurations
		const auto &api_bind_config = server_config->GetBind().GetManagers().GetApi();

		if (api_bind_config.IsParsed() == false)
		{
			logti("API Server is disabled by configuration");
			return true;
		}

		bool is_configured;
		auto worker_count = api_bind_config.GetWorkerCount(&is_configured);
		worker_count = is_configured ? worker_count : HTTP_SERVER_USE_DEFAULT_COUNT;

		bool is_port_configured;
		auto &port = api_bind_config.GetPort(&is_port_configured);

		bool is_tls_port_configured;
		auto &tls_port = api_bind_config.GetTlsPort(&is_tls_port_configured);

		if ((is_port_configured == false) && (is_tls_port_configured == false))
		{
			logtw("API Server is disabled - No port is configured");
			return true;
		}

		SetupCors(api_config);
		if (SetupAccessToken(api_config) == false)
		{
			return false;
		}

		auto result = PrepareHttpServers(
			server_config->GetIPList(),
			is_port_configured, port.GetPort(),
			is_tls_port_configured, tls_port.GetPort(),
			managers_config,
			worker_count);

		if (result == false)
		{
			auto http_server_manager = http::svr::HttpServerManager::GetInstance();

			while (_http_server_list.empty() == false)
			{
				auto http_server = _http_server_list.back();
				_http_server_list.pop_back();

				http_server_manager->ReleaseServer(http_server);
			}
			_http_server_list.clear();

			while (_https_server_list.empty() == false)
			{
				auto https_server = _https_server_list.back();
				_https_server_list.pop_back();

				http_server_manager->ReleaseServer(https_server);
			}
			_https_server_list.clear();
		}

		return result;
	}

	std::shared_ptr<http::svr::RequestInterceptor> Server::CreateInterceptor()
	{
		auto http_interceptor = std::make_shared<http::svr::DefaultInterceptor>();

		// CORS header processor
		http_interceptor->Register(http::Method::All, R"(.+)", [=](const std::shared_ptr<http::svr::HttpExchange> &client) -> http::svr::NextHandler {
			auto response = client->GetResponse();
			auto request = client->GetRequest();

			// Set default headers
			response->SetHeader("Server", "OvenMediaEngine");
			response->SetHeader("Content-Type", "text/html");

			// API Server uses OPTIONS/GET/POST/PUT/PATCH/DELETE methods
			_cors_manager.SetupHttpCorsHeader(
				info::VHostAppName::InvalidVHostAppName(),
				request, response,
				{http::Method::Options, http::Method::Get, http::Method::Post, http::Method::Put, http::Method::Patch, http::Method::Delete});

			return http::svr::NextHandler::Call;
		});

		// Preflight request processor
		http_interceptor->Register(http::Method::Options, R"(.+)", [=](const std::shared_ptr<http::svr::HttpExchange> &client) -> http::svr::NextHandler {
			// Respond 204 No Content for preflight request
			client->GetResponse()->SetStatusCode(http::StatusCode::NoContent);

			// Do not call the next handler to prevent 404 Not Found
			return http::svr::NextHandler::DoNotCall;
		});

		// Request Handlers will be added to http_interceptor
		_root_controller = std::make_shared<RootController>(_access_token);
		_root_controller->SetServer(GetSharedPtr());
		_root_controller->SetInterceptor(http_interceptor);
		_root_controller->PrepareHandlers();

		return http_interceptor;
	}

	bool Server::Stop()
	{
		auto manager = http::svr::HttpServerManager::GetInstance();

		std::vector<std::shared_ptr<http::svr::HttpServer>> http_server_list = std::move(_http_server_list);
		std::vector<std::shared_ptr<http::svr::HttpsServer>> https_server_list = std::move(_https_server_list);

		bool http_result = true;
		bool https_result = true;

		for (auto &http_server : http_server_list)
		{
			if (manager->ReleaseServer(http_server) == false)
			{
				http_result = false;
			}
		}

		for (auto &https_server : https_server_list)
		{
			if (manager->ReleaseServer(https_server) == false)
			{
				https_result = false;
			}
		}

		_is_storage_path_initialized = false;
		_storage_path = "";

		_root_controller = nullptr;

		return http_result && https_result;
	}

	void Server::CreateVHost(const cfg::vhost::VirtualHost &vhost_config)
	{
		OV_ASSERT2(vhost_config.IsReadOnly() == false);

		switch (ocst::Orchestrator::GetInstance()->CreateVirtualHost(vhost_config))
		{
			case ocst::Result::Failed:
				throw http::HttpError(http::StatusCode::BadRequest,
									  "Failed to create the virtual host: [%s]",
									  vhost_config.GetName().CStr());

			case ocst::Result::Succeeded:
				break;

			case ocst::Result::Exists:
				throw http::HttpError(http::StatusCode::Conflict,
									  "The virtual host already exists: [%s]",
									  vhost_config.GetName().CStr());

			case ocst::Result::NotExists:
				// CreateVirtualHost() never returns NotExists
				OV_ASSERT2(false);
				throw http::HttpError(http::StatusCode::InternalServerError,
									  "Unknown error occurred: [%s]",
									  vhost_config.GetName().CStr());
		}
	}

	void Server::DeleteVHost(const info::Host &host_info)
	{
		OV_ASSERT2(host_info.IsReadOnly() == false);

		logti("Deleting virtual host: %s", host_info.GetName().CStr());

		switch (ocst::Orchestrator::GetInstance()->DeleteVirtualHost(host_info))
		{
			case ocst::Result::Failed:
				throw http::HttpError(http::StatusCode::BadRequest,
									  "Failed to delete the virtual host: [%s]",
									  host_info.GetName().CStr());

			case ocst::Result::Succeeded:
				break;

			case ocst::Result::Exists:
				// CreateVirtDeleteVirtualHostualHost() never returns Exists
				OV_ASSERT2(false);
				throw http::HttpError(http::StatusCode::InternalServerError,
									  "Unknown error occurred: [%s]",
									  host_info.GetName().CStr());

			case ocst::Result::NotExists:
				// CreateVirtualHost() never returns NotExists
				throw http::HttpError(http::StatusCode::NotFound,
									  "The virtual host not exists: [%s]",
									  host_info.GetName().CStr());
		}
	}
}  // namespace api
