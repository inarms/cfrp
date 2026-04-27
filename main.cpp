/*
 * Copyright 2026 neesonqk
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <iostream>
#include <string>
#include <filesystem>
#include <fstream>
#include <csignal>
#define TOML_IMPLEMENTATION
#include <toml++/toml.h>
#include "server/server.h"
#include "client/client.h"
#include "common/quic_ngtcp2.h"
#include "common/utils.h"

namespace fs = std::filesystem;

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
    std::string config_path;
    std::string ca_path;
    std::string cli_token;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--ca" || arg == "-c") && i + 1 < argc) {
            ca_path = argv[++i];
        } else if ((arg == "-t" || arg == "--token") && i + 1 < argc) {
            cli_token = argv[++i];
        } else if (arg == "-h" || arg == "--help") {
            std::cout << "cfrp - A C++ Fast Reverse Proxy" << std::endl;
            std::cout << "Usage: cfrp [config.toml] | [options]" << std::endl;
            std::cout << "Options:" << std::endl;
            std::cout << "  [config.toml]        Path to the configuration file (TOML). If provided, all other options are ignored." << std::endl;
            std::cout << "  -c, --ca PATH        Path to the CA file (only used when no config file is provided)" << std::endl;
            std::cout << "  -t, --token STRING   Authentication token (only used when no config file is provided)" << std::endl;
            std::cout << "  -h, --help           Show this help message" << std::endl;
            return 0;
        } else if (!arg.empty() && arg[0] != '-') {
            if (config_path.empty()) {
                config_path = arg;
            } else {
                std::cerr << "Error: Multiple configuration files specified: " << config_path << " and " << arg << std::endl;
                return 1;
            }
        }
    }

    if (fs::exists("server.toml")) {
        config_path = "server.toml";
    } else if (fs::exists("client.toml")) {
        config_path = "client.toml";
    }

    if (!config_path.empty()) {
        ca_path.clear();
        cli_token.clear();
    }

    bool config_provided = !config_path.empty();
    bool ca_provided = !ca_path.empty();
    bool token_provided = !cli_token.empty();

    if (ca_provided != token_provided) {
        std::cerr << "Error: -c/--ca and -t/--token must be used together." << std::endl;
        std::cerr << "Example: ./cfrp -c certs/ca.crt -t your_secret_token" << std::endl;
        return 1;
    }

    if (!config_provided) {
        if (ca_provided) {
            std::cout << "No client configuration found. Generating default client.toml..." << std::endl;
            std::ofstream ofs("client.toml");
            if (ofs) {
                ofs << R"(# Default Client Configuration
[client]
server_addr = "127.0.0.1"
server_port = 7001
token = ")" << (token_provided ? cli_token : "secret_token") << R"("
name = "my-client"
protocol = "auto"
compression = true

[client.ssl]
enable = true
verify_peer = true
ca_file = ")" << ca_path << R"("

[[client.proxies]]
name = "ssh"
type = "tcp"
local_ip = "127.0.0.1"
local_port = 22
remote_port = 6000
)" << std::endl;
                ofs.close();
                config_path = "client.toml";
            } else {
                std::cerr << "Error: Could not generate default client.toml" << std::endl;
                return 1;
            }
        } else {
            std::cout << "No configuration file found. Generating default server.toml..." << std::endl;
            std::ofstream ofs("server.toml");
            if (ofs) {
                ofs << R"(# Default Server Configuration
[server]
bind_addr = "0.0.0.0"
bind_port = 7001
token = ")" << (token_provided ? cli_token : "secret_token") << R"("

# Virtual Host ports for HTTP and HTTPS (SNI routing)
vhost_http_port = 8080
vhost_https_port = 8443

[server.ssl]
enable = false
auto_generate = true
cert_file = "certs/server.crt"
key_file = "certs/server.key"
ca_file = "certs/ca.crt"
)" << std::endl;
                ofs.close();
                config_path = "server.toml";
            } else {
                std::cerr << "Error: Could not generate default server.toml" << std::endl;
                return 1;
            }
        }
    }

    if (!fs::exists(config_path)) {
        std::cerr << "Error: Configuration file not found: " << config_path << std::endl;
        return 1;
    }

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

        if (config["server"] && !ca_provided) {
            std::string bind_addr = config["server"]["bind_addr"].value_or("0.0.0.0");
            uint16_t bind_port = config["server"]["bind_port"].value_or(7000);
            std::string token = token_provided ? cli_token : config["server"]["token"].value_or("");
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
        } else if (config["client"] || (config["server"] && ca_provided)) {
            // Force client mode if -ca is provided, even if config has [server]
            auto client_node = config["client"];
            if (!client_node) client_node = config["server"]; // Use server node as base if client node is missing (unlikely but safe)

            std::string server_addr = client_node["server_addr"].value_or("127.0.0.1");
            uint16_t server_port = static_cast<uint16_t>(client_node["server_port"].value_or(7001));
            std::string token = token_provided ? cli_token : client_node["token"].value_or("");
            std::string client_name = client_node["name"].value_or("");
            std::string conf_d = client_node["conf_d"].value_or("");
            std::string protocol = client_node["protocol"].value_or("auto");
            bool compression = client_node["compression"].value_or(true);

            cfrp::client::SslConfig ssl_config;
            if (auto ssl = client_node["ssl"].as_table()) {
                ssl_config.enable = (*ssl)["enable"].value_or(false);
                ssl_config.verify_peer = (*ssl)["verify_peer"].value_or(false);
                ssl_config.ca_file = (*ssl)["ca_file"].value_or("certs/ca.crt");
            }

            client = std::shared_ptr<cfrp::client::Client>(new cfrp::client::Client(io_context, server_addr, server_port, token, client_name, ssl_config, compression, conf_d, protocol));

            if (auto proxies = client_node["proxies"].as_array()) {
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
                        if (auto bw = (*table)["bandwidth_limit"].as_string()) {
                            pc.bandwidth_limit = cfrp::common::ParseBandwidth(bw->get());
                        } else if (auto bw_int = (*table)["bandwidth_limit"].as_integer()) {
                            pc.bandwidth_limit = bw_int->get();
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
