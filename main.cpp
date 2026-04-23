#include <iostream>
#include <string>
#include <CLI/CLI.hpp>
#include <toml++/toml.h>
#include "server/server.h"
#include "client/client.h"
#include "common/quic_ngtcp2.h"

static cfrp::server::PortRange ParsePortRange(const std::string& s) {
    size_t dash = s.find('-');
    if (dash != std::string::npos) {
        uint16_t start = static_cast<uint16_t>(std::stoi(s.substr(0, dash)));
        uint16_t end = static_cast<uint16_t>(std::stoi(s.substr(dash + 1)));
        return {start, end};
    } else {
        uint16_t port = static_cast<uint16_t>(std::stoi(s));
        return {port, port};
    }
}

int main(int argc, char** argv) {
    CLI::App app{"cfrp - A C++ Fast Reverse Proxy"};

    std::string config_path = "config_server.toml";
    app.add_option("-c,--config", config_path, "Path to the configuration file (TOML)")->capture_default_str();

    CLI11_PARSE(app, argc, argv);

    try {
        auto config = toml::parse_file(config_path);
        asio::io_context io_context;

        std::shared_ptr<cfrp::server::Server> server;
        std::shared_ptr<cfrp::client::Client> client;

        asio::signal_set signals(io_context, SIGINT, SIGTERM);
        signals.async_wait([&](std::error_code /*ec*/, int /*signo*/) {
            std::cout << "\nCaught signal, exiting..." << std::endl;
            if (server) server->Stop();
            if (client) client->Stop();
            io_context.stop();
        });

        if (config["server"]) {
            std::string bind_addr = config["server"]["bind_addr"].value_or("0.0.0.0");
            uint16_t bind_port = config["server"]["bind_port"].value_or(7000);
            std::string token = config["server"]["token"].value_or("");
            std::string protocol = config["server"]["protocol"].value_or("auto");
            
            cfrp::server::SslConfig ssl_config;
            if (auto ssl = config["server"]["ssl"].as_table()) {
                ssl_config.enable = (*ssl)["enable"].value_or(false);
                ssl_config.auto_generate = (*ssl)["auto_generate"].value_or(true);
                ssl_config.cert_file = (*ssl)["cert_file"].value_or("certs/server.crt");
                ssl_config.key_file = (*ssl)["key_file"].value_or("certs/server.key");
                ssl_config.ca_file = (*ssl)["ca_file"].value_or("certs/ca.crt");
            }

            std::vector<cfrp::server::PortRange> allowed_ports;
            if (auto ports = config["server"]["allowed_ports"].as_array()) {
                for (auto& elem : *ports) {
                    if (auto s = elem.as_string()) {
                        try {
                            allowed_ports.push_back(ParsePortRange(s->get()));
                        } catch (...) {}
                    } else if (auto i = elem.as_integer()) {
                        allowed_ports.push_back({static_cast<uint16_t>(i->get()), static_cast<uint16_t>(i->get())});
                    }
                }
            }
            
            std::vector<std::string> allowed_clients;
            if (auto clients = config["server"]["allowed_clients"].as_array()) {
                for (auto& elem : *clients) {
                    if (auto s = elem.as_string()) {
                        allowed_clients.push_back(s->get());
                    }
                }
            }
            
            uint16_t vhost_http_port = static_cast<uint16_t>(config["server"]["vhost_http_port"].value_or(0));
            uint16_t vhost_https_port = static_cast<uint16_t>(config["server"]["vhost_https_port"].value_or(0));

            server = std::shared_ptr<cfrp::server::Server>(new cfrp::server::Server(io_context, bind_addr, bind_port, token, ssl_config, protocol, allowed_ports, allowed_clients));
            server->SetVhostPorts(vhost_http_port, vhost_https_port);
            server->Run();
        } else if (config["client"]) {
            std::string server_addr = config["client"]["server_addr"].value_or("127.0.0.1");
            uint16_t server_port = config["client"]["server_port"].value_or(7001);
            std::string token = config["client"]["token"].value_or("");
            std::string client_name = config["client"]["name"].value_or("");
            std::string conf_d = config["client"]["conf_d"].value_or("");
            std::string protocol = config["client"]["protocol"].value_or("auto");
            bool compression = config["client"]["compression"].value_or(true);

            cfrp::client::SslConfig ssl_config;
            if (auto ssl = config["client"]["ssl"].as_table()) {
                ssl_config.enable = (*ssl)["enable"].value_or(false);
                ssl_config.verify_peer = (*ssl)["verify_peer"].value_or(false);
                ssl_config.ca_file = (*ssl)["ca_file"].value_or("certs/ca.crt");
            }

            client = std::shared_ptr<cfrp::client::Client>(new cfrp::client::Client(io_context, server_addr, server_port, token, client_name, ssl_config, compression, conf_d, protocol));

            if (auto proxies = config["client"]["proxies"].as_array()) {
                for (auto& elem : *proxies) {
                    if (auto table = elem.as_table()) {
                        cfrp::client::ProxyConfig pc;
                        pc.name = (*table)["name"].value_or("");
                        pc.type = (*table)["type"].value_or("tcp");
                        pc.local_ip = (*table)["local_ip"].value_or("127.0.0.1");
                        pc.local_port = static_cast<uint16_t>((*table)["local_port"].value_or(0));
                        pc.remote_port = static_cast<uint16_t>((*table)["remote_port"].value_or(0));
                        if (auto domains = (*table)["custom_domains"].as_array()) {
                            for (auto& d : *domains) {
                                if (auto s = d.as_string()) pc.custom_domains.push_back(s->get());
                            }
                        } else if (auto d = (*table)["custom_domains"].as_string()) {
                            pc.custom_domains.push_back(d->get());
                        }
                        client->AddProxy(pc);
                    }
                }
            }

            client->Run();
        } else {
            std::cerr << "Error: Configuration must contain either a [server] or [client] section." << std::endl;
            return 1;
        }

        io_context.run();

    } catch (const toml::parse_error& err) {
        std::cerr << "Parsing failed:\n" << err << std::endl;
        return 1;
    } catch (const std::exception& err) {
        std::cerr << "Error: " << err.what() << std::endl;
        return 1;
    }

    return 0;
}
