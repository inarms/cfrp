# Dynamic Custom Domains Support Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `custom_domains` support to the dynamic client proxy configuration loader (hot reload) so that HTTP/HTTPS dynamic proxies can correctly parse and reload their custom domains.

**Architecture:** Update the TOML parser for files in the `conf.d` directory to extract `custom_domains` (both array and string format) and include them in the dynamic proxy change detection logic.

**Tech Stack:** C++17, tomlplusplus, CMake

## Global Constraints
- Naming of variables must match existing codebase style (snake_case).
- Keep existing code formatting and logger conventions.

---

### Task 1: Update Dynamic Configuration Parsing

**Files:**
- Modify: `src/client/client.cpp:777-792`

**Interfaces:**
- Consumes: `toml::parse_file` output (table)
- Produces: `ProxyConfig::custom_domains` populated in `new_proxies`

- [ ] **Step 1: Locate code block**
  Open [src/client/client.cpp](file:///Users/rukawaz/projects/cfrp/src/client/client.cpp#L777-L792) and locate where the table is parsed.

- [ ] **Step 2: Add custom_domains parsing**
  Insert the following parsing block after `pc.remote_port` parsing logic (around line 785):
  ```cpp
                        if (auto domains = data["custom_domains"].as_array()) {
                            for (auto& d : *domains) {
                                if (auto s = d.as_string()) pc.custom_domains.push_back(s->get());
                            }
                        } else if (auto d = data["custom_domains"].as_string()) {
                            pc.custom_domains.push_back(d->get());
                        }
  ```

- [ ] **Step 3: Update change detection comparison**
  Modify the change detection comparison check (around line 820) to check `custom_domains` equality:
  ```cpp
            bool changed = (it->second.type != pc.type || 
                            it->second.local_ip != pc.local_ip ||
                            it->second.local_port != pc.local_port ||
                            it->second.remote_port != pc.remote_port ||
                            it->second.custom_domains != pc.custom_domains);
  ```

---

### Task 2: Verification

**Files:**
- Create: Temporary workspace test file `conf.d/test_gitea.toml`
- Create: Temporary test script `tests/verify_dynamic_domains.sh`

- [ ] **Step 1: Build the project**
  Run CMake build in the `build` directory:
  ```bash
  cmake --build build --config Debug
  ```
  Verify the build completes with zero compilation errors.

- [ ] **Step 2: Write Verification Script**
  Create a temporary script to launch the server and client and verify config reloading:
  ```bash
  mkdir -p build/conf.d
  # Start server in background
  ./build/cfrp server.toml &
  SERVER_PID=$!
  sleep 1
  # Start client in background, using a modified local client config pointing to build/conf.d
  cp client.toml build/client_test.toml
  # Ensure conf_d points to build/conf.d
  # Run client
  ./build/cfrp build/client_test.toml &
  CLIENT_PID=$!
  sleep 1
  # Add dynamic config
  echo -e 'name = "gitea"\ntype = "http"\nlocal_port = 3001\ncustom_domains = ["git.cawret.com"]' > build/conf.d/gitea.toml
  sleep 6 # Wait for poll
  # Verify logs or reload status
  # Update dynamic config domains
  echo -e 'name = "gitea"\ntype = "http"\nlocal_port = 3001\ncustom_domains = ["git.cawret.com", "alt.cawret.com"]' > build/conf.d/gitea.toml
  sleep 6
  # Cleanup
  kill $CLIENT_PID
  kill $SERVER_PID
  rm -rf build/conf.d
  rm -f build/client_test.toml
  ```

- [ ] **Step 3: Execute verification and check logs**
  Verify the output logs show:
  1. `Adding dynamic proxy [gitea]` with custom domain `git.cawret.com`
  2. `Updating dynamic proxy [gitea]` with new custom domains list.
