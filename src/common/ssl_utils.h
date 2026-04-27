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

#pragma once

#include <string>
#include <vector>

namespace cfrp {
namespace common {

struct CertConfig {
    std::string ca_cert_file = "certs/ca.crt";
    std::string ca_key_file = "certs/ca.key";
    std::string server_cert_file = "certs/server.crt";
    std::string server_key_file = "certs/server.key";
    int ca_expiry_days = 3650;      // 10 years
    int server_expiry_days = 365;   // 1 year
};

class SslUtils {
public:
    /**
     * @brief Ensures that valid CA and server certificates exist.
     *        If missing or expired, regenerates the chain.
     */
    static bool EnsureCertificates(const CertConfig& config);

    /**
     * @brief Checks if a certificate file exists and is not expired.
     * @param cert_file Path to the PEM certificate file.
     * @return true if valid, false if missing or expired.
     */
    static bool IsCertValid(const std::string& cert_file);

    /**
     * @brief Generates a new CA and a server certificate signed by that CA.
     */
    static bool GenerateFullChain(const CertConfig& config);

private:
    static bool CreateDirectoryIfNotExists(const std::string& path);
};

} // namespace common
} // namespace cfrp
