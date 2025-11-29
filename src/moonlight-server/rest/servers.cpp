#include <boost/property_tree/json_parser.hpp>
#include <boost/asio/steady_timer.hpp>
#include <events/events.hpp>
#include <immer/atom.hpp>
#include <immer/map_transient.hpp>
#include <monitoring/thread-monitor.hpp>
#include <rest/endpoints.hpp>

namespace HTTPServers {

/**
 * A bit of magic here, it'll load up the pin.html via Cmake (look for `make_includable`)
 */
constexpr char const *pin_html =
#include "html/pin.include.html"

    ;

namespace bt = boost::property_tree;
using namespace wolf::core;

/**
 * @brief Start the generic server on the specified port
 * @return std::thread: the thread where this server will run
 */
void startServer(HttpServer *server, const immer::box<state::AppState> state, int port) {
  server->config.port = port;
  server->config.address = "0.0.0.0";
  server->default_resource["GET"] = endpoints::not_found<SimpleWeb::HTTP>;
  server->default_resource["POST"] = endpoints::not_found<SimpleWeb::HTTP>;

  server->resource["^/serverinfo$"]["GET"] = [&state](auto resp, auto req) {
    endpoints::serverinfo<SimpleWeb::HTTP>(resp, req, {}, state);
  };

  server->resource["^/pair$"]["GET"] = [&state](auto resp, auto req) { endpoints::pair(resp, req, state); };

  auto pairing_atom = state->pairing_atom;

  server->resource["^/pin/$"]["GET"] = [](auto resp, auto req) { resp->write(pin_html); };
  server->resource["^/pin/$"]["POST"] = [pairing_atom](auto resp, auto req) {
    try {
      bt::ptree pt;

      read_json(req->content, pt);

      auto pin = pt.get<std::string>("pin");
      auto secret = pt.get<std::string>("secret");
      logs::log(logs::debug, "Received POST /pin/ pin:{} secret:{}", pin, secret);

      auto pair_request = pairing_atom->load()->at(secret);
      pair_request->user_pin->set_value(pin);
      resp->write("OK");
      pairing_atom->update([&secret](auto m) { return m.erase(secret); });
    } catch (const std::exception &e) {
      *resp << "HTTP/1.1 400 Bad Request\r\nContent-Length: " << strlen(e.what()) << "\r\n\r\n" << e.what();
    }
  };

  server->resource["^/unpair$"]["GET"] = [&state](auto resp, auto req) {
    SimpleWeb::CaseInsensitiveMultimap headers = req->parse_query_string();
    auto client_id = get_header(headers, "uniqueid");
    auto client_ip = req->remote_endpoint().address().to_string();
    auto cache_key = client_id.value() + "@" + client_ip;

    logs::log(logs::info, "Unpairing: {}", cache_key);
    auto client = state->pairing_cache->load()->at(cache_key);
    state::unpair(state->config, state::PairedClient{.client_cert = client.client_cert});

    XML xml;
    xml.put("root.<xmlattr>.status_code", 200);
    send_xml<SimpleWeb::HTTP>(resp, SimpleWeb::StatusCode::success_ok, xml);
  };

  auto pair_handler = state->event_bus->register_handler<immer::box<events::PairSignal>>(
      [pairing_atom](const immer::box<events::PairSignal> pair_sig) {
        // Check if auto-pairing PIN is set via environment variable
        auto auto_pin_env = utils::get_env("MOONLIGHT_INTERNAL_PAIRING_PIN", "");
        std::string auto_pin(auto_pin_env);

        // Only auto-pair for local clients:
        // - 172.x.x.x: docker bridge network (separated containers)
        // - 127.0.0.1: localhost (unified sandbox container)
        bool is_local_client = pair_sig->client_ip.rfind("172.", 0) == 0 ||
                               pair_sig->client_ip == "127.0.0.1";

        if (!auto_pin.empty() && is_local_client) {
          logs::log(logs::info, "Auto-pairing client {} with PIN from MOONLIGHT_INTERNAL_PAIRING_PIN", pair_sig->client_ip);
          // Automatically fulfill the PIN promise
          pair_sig->user_pin->set_value(auto_pin);
        } else {
          pairing_atom->update([&pair_sig](const immer::map<std::string, immer::box<events::PairSignal>> &m) {
            auto secret = crypto::str_to_hex(crypto::random(8));
            logs::log(logs::info, "Insert pin at http://{}:47989/pin/#{}", pair_sig->host_ip, secret);
            // filter out any other (dangling) pair request from the same client
            auto t_map = m.transient();
            for (auto [key, value] : m) {
              if (value->client_ip == pair_sig->client_ip) {
                t_map.erase(key);
              }
            }
            // insert the new pair request
            t_map.set(secret, pair_sig);
            return t_map.persistent();
          });
        }
      });

  // Start server (this initializes io_service)
  server->start([server](unsigned short port) {
    logs::log(logs::info, "HTTP server listening on port: {} ", port);

    // Add heartbeat timer to io_context (runs every 1 second)
    auto heartbeat_timer = std::make_shared<boost::asio::steady_timer>(*server->io_service);
    auto heartbeat_callback = std::make_shared<std::function<void(const boost::system::error_code&)>>();
    *heartbeat_callback = [heartbeat_timer, heartbeat_callback](const boost::system::error_code& ec) {
      if (!ec) {
        wolf::monitoring::ThreadMonitor::get().heartbeat();
        heartbeat_timer->expires_after(std::chrono::seconds(1));
        heartbeat_timer->async_wait(*heartbeat_callback);
      }
    };
    heartbeat_timer->expires_after(std::chrono::seconds(1));
    heartbeat_timer->async_wait(*heartbeat_callback);
  });

  pair_handler.unregister();
}

std::optional<state::PairedClient>
get_client_if_paired(const immer::box<state::AppState> state,
                     const std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Request> &request) {
  auto client_cert = SimpleWeb::Server<SimpleWeb::HTTPS>::get_client_cert(request);
  return state::get_client_via_ssl(state->config, std::move(client_cert));
}

void reply_unauthorized(const std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Request> &request,
                        const std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Response> &response) {
  logs::log(logs::warning, "Received HTTPS request from a client which wasn't previously paired.");

  XML xml;

  xml.put("root.<xmlattr>.status_code"s, 401);
  xml.put("root.<xmlattr>.query"s, request->path);
  xml.put("root.<xmlattr>.status_message"s, "The client is not authorized. Certificate verification failed."s);

  send_xml<SimpleWeb::HTTPS>(response, SimpleWeb::StatusCode::client_error_unauthorized, xml);
}

void startServer(HttpsServer *server, const immer::box<state::AppState> state, int port) {
  server->config.port = port;
  server->config.address = "0.0.0.0";
  server->default_resource["GET"] = endpoints::not_found<SimpleWeb::HTTPS>;
  server->default_resource["POST"] = endpoints::not_found<SimpleWeb::HTTPS>;

  server->resource["^/serverinfo$"]["GET"] = [&state](auto resp, auto req) {
    if (auto client = get_client_if_paired(state, req)) {
      auto client_session = state::get_session_by_client(state->running_sessions->load(), client.value());
      endpoints::serverinfo<SimpleWeb::HTTPS>(resp, req, client_session, state);
    } else {
      reply_unauthorized(req, resp);
    }
  };

  server->resource["^/pair$"]["GET"] = [&state](auto resp, auto req) {
    if (get_client_if_paired(state, req)) {
      endpoints::https::pair(resp, req);
    } else {
      reply_unauthorized(req, resp);
    }
  };

  server->resource["^/applist$"]["GET"] = [&state](auto resp, auto req) {
    if (get_client_if_paired(state, req)) {
      endpoints::https::applist(resp, req, state);
    } else {
      reply_unauthorized(req, resp);
    }
  };

  server->resource["^/launch"]["GET"] = [&state](auto resp, auto req) {
    if (auto client = get_client_if_paired(state, req)) {
      endpoints::https::launch(resp, req, client.value(), state);
    } else {
      reply_unauthorized(req, resp);
    }
  };

  server->resource["^/resume$"]["GET"] = [&state](auto resp, auto req) {
    if (auto client = get_client_if_paired(state, req)) {
      endpoints::https::resume(resp, req, client.value(), state);
    } else {
      reply_unauthorized(req, resp);
    }
  };

  server->resource["^/cancel$"]["GET"] = [&state](auto resp, auto req) {
    if (auto client = get_client_if_paired(state, req)) {
      endpoints::https::cancel(resp, req, client.value(), state);
    } else {
      reply_unauthorized(req, resp);
    }
  };

  server->resource["^/appasset$"]["GET"] = [&state](auto resp, auto req) {
    if (get_client_if_paired(state, req)) {
      endpoints::https::appasset(resp, req, state);
    } else {
      reply_unauthorized(req, resp);
    }
  };

  server->start([server](unsigned short port) {
    logs::log(logs::info, "HTTPS server listening on port: {} ", port);

    // Add heartbeat timer to io_context (runs every 1 second)
    auto heartbeat_timer = std::make_shared<boost::asio::steady_timer>(*server->io_service);
    auto heartbeat_callback = std::make_shared<std::function<void(const boost::system::error_code&)>>();
    *heartbeat_callback = [heartbeat_timer, heartbeat_callback](const boost::system::error_code& ec) {
      if (!ec) {
        wolf::monitoring::ThreadMonitor::get().heartbeat();
        heartbeat_timer->expires_after(std::chrono::seconds(1));
        heartbeat_timer->async_wait(*heartbeat_callback);
      }
    };
    heartbeat_timer->expires_after(std::chrono::seconds(1));
    heartbeat_timer->async_wait(*heartbeat_callback);
  });
}

} // namespace HTTPServers