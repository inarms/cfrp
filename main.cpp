/*
 * Copyright 2026 inarms
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
#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <wolfssl/wolfcrypt/logging.h>

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
    wolfSSL_Init();
    std::string exe_path = cfrp::common::GetExecutablePath();
    std::string exe_dir = exe_path.empty() ? "." : fs::path(exe_path).parent_path().string();
    std::string pid_path = (fs::path(exe_dir) / "cfrp.pid").string();
    std::string log_path = (fs::path(exe_dir) / "cfrp.log").string();
    std::string status_path = (fs::path(exe_dir) / "cfrp.status").string();

    bool is_daemon_worker = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--daemon-worker") {
            is_daemon_worker = true;
            break;
        }
    }

    if (argc >= 2 && std::string(argv[1]) == "status") {
        if (fs::exists(pid_path)) {
            std::ifstream ifs(pid_path);
            int pid;
            if (ifs >> pid) {
                if (cfrp::common::IsProcessRunning(pid)) {
                    std::cout << "---------------------------------------" << std::endl;
                    std::cout << "  cfrp status: Running" << std::endl;
                    std::cout << "  PID:         " << pid << std::endl;
                    if (fs::exists(status_path)) {
                        std::ifstream s_ifs(status_path);
                        std::string line;
                        while (std::getline(s_ifs, line)) {
                            std::cout << "  " << line << std::endl;
                        }
                    }
                    std::cout << "---------------------------------------" << std::endl;
                    return 0;
                } else {
                    std::cout << "cfrp is not running (stale PID: " << pid << ")." << std::endl;
                    return 0;
                }
            }
        }
        std::cout << "cfrp is not running." << std::endl;
        return 0;
    }

    if (argc >= 2 && std::string(argv[1]) == "stop") {
        if (fs::exists(pid_path)) {
            std::ifstream ifs(pid_path);
            int pid;
            if (ifs >> pid) {
                if (cfrp::common::IsProcessRunning(pid)) {
                    if (cfrp::common::StopProcess(pid)) {
                        std::cout << "Stopped cfrp process " << pid << std::endl;
                        if (fs::exists(pid_path)) fs::remove(pid_path);
                        if (fs::exists(status_path)) fs::remove(status_path);
                        return 0;
                    }
 else {
                        std::cerr << "Error: Failed to stop process " << pid << std::endl;
                        return 1;
                    }
                } else {
                    std::cout << "Process " << pid << " is not running. Cleaning up stale PID file." << std::endl;
                    if (fs::exists(pid_path)) fs::remove(pid_path);
                    if (fs::exists(status_path)) fs::remove(status_path);
                    return 0;
                }
            }
        }
        std::cout << "cfrp is not running." << std::endl;
        return 0;
    }

    std::string home = cfrp::common::GetHomeDirectory();
    std::string home_config_dir = home.empty() ? "" : (fs::path(home) / ".cfrp").string();
    std::string home_config_file = home_config_dir.empty() ? "" : (fs::path(home_config_dir) / "config").string();

    if (argc >= 2 && std::string(argv[1]) == "config") {
        if (argc < 3) {
            std::cerr << "Usage: cfrp config <get|set|ls> [args]" << std::endl;
            return 1;
        }
        std::string subcmd = argv[2];
        if (subcmd == "ls") {
            if (!home_config_file.empty() && fs::exists(home_config_file)) {
                try {
                    auto config = toml::parse_file(home_config_file);
                    for (auto&& [key, value] : config) {
                        std::cout << key << " = ";
                        if (value.is_string()) {
                            std::cout << "\"" << value.as_string()->get() << "\"" << std::endl;
                        } else if (value.is_integer()) {
                            std::cout << value.as_integer()->get() << std::endl;
                        } else if (value.is_boolean()) {
                            std::cout << (value.as_boolean()->get() ? "true" : "false") << std::endl;
                        } else if (value.is_floating_point()) {
                            std::cout << value.as_floating_point()->get() << std::endl;
                        } else {
                            std::cout << "<complex type>" << std::endl;
                        }
                    }
                } catch (...) {
                    return 1;
                }
            }
            return 0;
        } else if (subcmd == "get") {
            if (argc < 4) {
                std::cerr << "Usage: cfrp config get <key>" << std::endl;
                return 1;
            }
            std::string key = argv[3];
            if (!home_config_file.empty() && fs::exists(home_config_file)) {
                try {
                    auto config = toml::parse_file(home_config_file);
                    if (config.contains(key)) {
                        auto val = config[key];
                        if (val.is_string()) {
                            std::cout << val.as_string()->get() << std::endl;
                        } else if (val.is_integer()) {
                            std::cout << val.as_integer()->get() << std::endl;
                        } else if (val.is_boolean()) {
                            std::cout << (val.as_boolean()->get() ? "true" : "false") << std::endl;
                        } else if (val.is_floating_point()) {
                            std::cout << val.as_floating_point()->get() << std::endl;
                        }
                    }
                } catch (...) {
                    return 1;
                }
            }
            return 0;
        } else if (subcmd == "set") {
            if (argc < 5) {
                std::cerr << "Usage: cfrp config set <key> <value>" << std::endl;
                return 1;
            }
            std::string key = argv[3];
            std::string value = argv[4];
            if (home_config_file.empty()) return 1;

            std::vector<std::string> lines;
            bool found = false;
            std::string formatted_value = value;
            bool is_bool = (value == "true" || value == "false");
            bool is_num = !value.empty();
            if (is_num) {
                try {
                    size_t pos;
                    std::stoll(value, &pos);
                    if (pos != value.size()) is_num = false;
                } catch (...) { is_num = false; }
            }
            if (!is_bool && !is_num) formatted_value = "\"" + value + "\"";

            if (fs::exists(home_config_file)) {
                std::ifstream ifs(home_config_file);
                std::string line;
                while (std::getline(ifs, line)) {
                    if (!found) {
                        size_t eq_pos = line.find('=');
                        if (eq_pos != std::string::npos) {
                            std::string lk = line.substr(0, eq_pos);
                            lk.erase(0, lk.find_first_not_of(" \t"));
                            lk.erase(lk.find_last_not_of(" \t") + 1);
                            if (lk == key) {
                                lines.push_back(key + " = " + formatted_value);
                                found = true;
                                continue;
                            }
                        }
                    }
                    lines.push_back(line);
                }
            } else if (!home_config_dir.empty()) {
                fs::create_directories(home_config_dir);
            }
            if (!found) lines.push_back(key + " = " + formatted_value);
            std::ofstream ofs(home_config_file);
            for (const auto& l : lines) ofs << l << "\n";
            std::cout << "Updated global config: " << key << " = " << formatted_value << std::endl;
            return 0;
        }
    }

    std::string config_path;
    std::string ca_path;
    std::string cli_token;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--ca" || arg == "-c") && i + 1 < argc) {
            ca_path = argv[++i];
        } else if ((arg == "-t" || arg == "--token") && i + 1 < argc) {
            cli_token = argv[++i];
        } else if (arg == "-v" || arg == "--version") {
            std::cout << "cfrp version " << CFRP_VERSION << std::endl;
            return 0;
        } else if (arg == "-h" || arg == "--help") {
            std::cout << "cfrp - A C++ Fast Reverse Proxy (version " << CFRP_VERSION << ")" << std::endl;
            std::cout << "Usage: cfrp [config.toml] | [options] | [command]" << std::endl;
            std::cout << "Options:" << std::endl;
            std::cout << "  [config.toml]        Path to the configuration file (TOML). If provided, all other options are ignored." << std::endl;
            std::cout << "  -c, --ca PATH        Path to the CA file (only used when no config file is provided)" << std::endl;
            std::cout << "  -t, --token STRING   Authentication token (only used when no config file is provided)" << std::endl;
            std::cout << "  -v, --version        Show version information" << std::endl;
            std::cout << "  -h, --help           Show this help message" << std::endl;
            std::cout << "Commands:" << std::endl;
            std::cout << "  config set <key> <value>  Set global configuration" << std::endl;
            std::cout << "  config get <key>          Get global configuration" << std::endl;
            std::cout << "  config ls                 List all global configuration" << std::endl;
            std::cout << "  status                    Show current status" << std::endl;
            std::cout << "  stop                      Stop background process" << std::endl;
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

    // 1. Handle Global Tool Configuration (~/.cfrp/config)
    std::string working_mode = "foreground";
    if (!home_config_file.empty()) {
        if (!fs::exists(home_config_file)) {
            try {
                fs::create_directories(home_config_dir);
                std::ofstream ofs(home_config_file);
                if (ofs) {
                    ofs << "# cfrp global tool configuration" << std::endl;
                    ofs << "working_mode = \"foreground\"" << std::endl;
                    ofs.close();
                    std::cout << "Generated global tool configuration: " << home_config_file << std::endl;
                }
            } catch (...) {}
        } else {
            try {
                auto tool_config = toml::parse_file(home_config_file);
                working_mode = tool_config["working_mode"].value_or("foreground");
            } catch (...) {}
        }
    }

    if (working_mode == "background" || is_daemon_worker) {
        if (!is_daemon_worker && fs::exists(pid_path)) {
            std::ifstream ifs(pid_path);
            int existing_pid;
            if (ifs >> existing_pid) {
                if (cfrp::common::IsProcessRunning(existing_pid)) {
                    std::cerr << "Error: cfrp is already running with PID " << existing_pid << std::endl;
                    return 1;
                }
            }
        }

        if (working_mode == "background" && !is_daemon_worker) {
#ifndef _WIN32
            pid_t pid = fork();
            if (pid < 0) {
                std::cerr << "Error: Failed to fork" << std::endl;
                return 1;
            }
            if (pid > 0) {
                std::cout << "Starting in background mode (PID: " << pid << ")" << std::endl;
                return 0;
            }
            if (setsid() < 0) return 1;
            int fd = open(log_path.c_str(), O_RDWR | O_CREAT | O_APPEND, 0644);
            if (fd != -1) {
                dup2(fd, STDIN_FILENO);
                dup2(fd, STDOUT_FILENO);
                dup2(fd, STDERR_FILENO);
                if (fd > 2) close(fd);
            }
#else
            std::string cmd = "\"" + exe_path + "\"";
            for (int i = 1; i < argc; ++i) {
                cmd += " \"" + std::string(argv[i]) + "\"";
            }
            cmd += " --daemon-worker";

            STARTUPINFOA si = { sizeof(si) };
            PROCESS_INFORMATION pi;
            if (CreateProcessA(NULL, (char*)cmd.c_str(), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
                std::ofstream ofs(pid_path);
                ofs << pi.dwProcessId;
                std::cout << "Starting in background mode (PID: " << pi.dwProcessId << ")" << std::endl;
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
                return 0;
            } else {
                std::cerr << "Error: Failed to start background process" << std::endl;
                return 1;
            }
#endif
        }

        std::ofstream ofs(pid_path);
#ifdef _WIN32
        ofs << GetCurrentProcessId();
#else
        ofs << getpid();
#endif
    }

    // 2. Handle Functional Configuration (server.toml / client.toml)
    if (config_path.empty()) {
        if (fs::exists("server.toml")) {
            config_path = "server.toml";
        } else if (fs::exists("client.toml")) {
            config_path = "client.toml";
        }
    }

    if (!config_path.empty()) {
        ca_path.clear();
        cli_token.clear();
    }

    bool config_found = !config_path.empty();
    bool ca_provided = !ca_path.empty();
    bool token_provided = !cli_token.empty();

    if (ca_provided != token_provided) {
        std::cerr << "Error: -c/--ca and -t/--token must be used together." << std::endl;
        std::cerr << "Example: ./cfrp -c certs/ca.crt -t your_secret_token" << std::endl;
        return 1;
    }

    if (!config_found) {
        if (ca_provided) {
            std::cout << "No client configuration found. Generating default client.toml..." << std::endl;
            config_path = "client.toml";
            std::ofstream ofs(config_path);
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
            } else {
                std::cerr << "Error: Could not generate default client.toml" << std::endl;
                return 1;
            }
        } else {
            std::cout << "No functional configuration found. Generating default server.toml..." << std::endl;
            config_path = "server.toml";
            std::ofstream ofs(config_path);
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
enable = true
auto_generate = true
cert_file = "certs/server.crt"
key_file = "certs/server.key"
ca_file = "certs/ca.crt"
)" << std::endl;
                ofs.close();
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
            if (fs::exists(pid_path)) {
                std::ifstream ifs(pid_path);
                int saved_pid;
                if (ifs >> saved_pid) {
#ifdef _WIN32
                    if (saved_pid == (int)GetCurrentProcessId()) {
                        fs::remove(pid_path);
                        if (fs::exists(status_path)) fs::remove(status_path);
                    }
#else
                    if (saved_pid == (int)getpid()) {
                        fs::remove(pid_path);
                        if (fs::exists(status_path)) fs::remove(status_path);
                    }
#endif
                }
            }
            io_context.stop();
        });

        if (config["server"] && !ca_provided) {
            std::string bind_addr = config["server"]["bind_addr"].value_or("0.0.0.0");
            uint16_t bind_port = config["server"]["bind_port"].value_or(7000);
            std::string token = token_provided ? cli_token : config["server"]["token"].value_or("");
            std::string protocol = config["server"]["protocol"].value_or("auto");
            
            cfrp::server::SslConfig ssl_config;
            if (auto ssl = config["server"]["ssl"].as_table()) {
                ssl_config.enable = (*ssl)["enable"].value_or(true);
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

            std::ofstream status_ofs(status_path);
            if (status_ofs) {
                status_ofs << "Mode:        Server\n";
                status_ofs << "Config:      " << config_path << "\n";
            }

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
            int compression_level = static_cast<int>(client_node["compression_level"].value_or(1));

            cfrp::client::SslConfig ssl_config;
            if (auto ssl = client_node["ssl"].as_table()) {
                ssl_config.enable = (*ssl)["enable"].value_or(true);
                ssl_config.verify_peer = (*ssl)["verify_peer"].value_or(false);
                ssl_config.ca_file = (*ssl)["ca_file"].value_or("certs/ca.crt");
            }

            client = std::shared_ptr<cfrp::client::Client>(new cfrp::client::Client(io_context, server_addr, server_port, token, client_name, ssl_config, compression, compression_level, conf_d, protocol));

            std::ofstream status_ofs(status_path);
            if (status_ofs) {
                status_ofs << "Mode:        Client\n";
                status_ofs << "Config:      " << config_path << "\n";
            }

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
        wolfSSL_Cleanup();
        return 1;
    } catch (const std::exception& err) {
        std::cerr << "Error: " << err.what() << std::endl;
        wolfSSL_Cleanup();
        return 1;
    }

    wolfSSL_Cleanup();
    return 0;
}
