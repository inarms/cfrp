#include <iostream>
#include <string>
#include <CLI/CLI.hpp>
#include <toml++/toml.h>
#include "server/server.h"
#include "client/client.h"

int main(int argc, char** argv) {
    CLI::App app{"cfrp - A C++ Fast Reverse Proxy"};

    std::string config_path = "config_server.toml";
    app.add_option("-c,--config", config_path, "Path to the configuration file (TOML)")->capture_default_str();

    CLI11_PARSE(app, argc, argv);

    try {
        auto config = toml::parse_file(config_path);

        if (config["server"]) {
            std::string bind_addr = config["server"]["bind_addr"].value_or("0.0.0.0");
            uint16_t bind_port = config["server"]["bind_port"].value_or(7000);
            std::string token = config["server"]["token"].value_or("");
            
            cfrp::server::SslConfig ssl_config;
            if (auto ssl = config["server"]["ssl"].as_table()) {
                ssl_config.enable = (*ssl)["enable"].value_or(false);
                ssl_config.cert_file = (*ssl)["cert_file"].value_or("");
                ssl_config.key_file = (*ssl)["key_file"].value_or("");
            }
            
            cfrp::server::Server server(bind_addr, bind_port, token, ssl_config);
            server.Run();
        } else if (config["client"]) {
            std::string server_addr = config["client"]["server_addr"].value_or("127.0.0.1");
            uint16_t server_port = config["client"]["server_port"].value_or(7001);
            std::string token = config["client"]["token"].value_or("");

            cfrp::client::SslConfig ssl_config;
            if (auto ssl = config["client"]["ssl"].as_table()) {
                ssl_config.enable = (*ssl)["enable"].value_or(false);
                ssl_config.verify_peer = (*ssl)["verify_peer"].value_or(false);
                ssl_config.ca_file = (*ssl)["ca_file"].value_or("");
            }

            cfrp::client::Client client(server_addr, server_port, token, ssl_config);

            if (auto proxies = config["client"]["proxies"].as_array()) {
                for (auto& elem : *proxies) {
                    if (auto table = elem.as_table()) {
                        cfrp::client::ProxyConfig pc;
                        pc.name = (*table)["name"].value_or("");
                        pc.type = (*table)["type"].value_or("tcp");
                        pc.local_ip = (*table)["local_ip"].value_or("127.0.0.1");
                        pc.local_port = static_cast<uint16_t>((*table)["local_port"].value_or(0));
                        pc.remote_port = static_cast<uint16_t>((*table)["remote_port"].value_or(0));
                        client.AddProxy(pc);
                    }
                }
            }

            client.Run();
        } else {
            std::cerr << "Error: Configuration must contain either a [server] or [client] section." << std::endl;
            return 1;
        }

    } catch (const toml::parse_error& err) {
        std::cerr << "Parsing failed:\n" << err << std::endl;
        return 1;
    } catch (const std::exception& err) {
        std::cerr << "Error: " << err.what() << std::endl;
        return 1;
    }

    return 0;
}
