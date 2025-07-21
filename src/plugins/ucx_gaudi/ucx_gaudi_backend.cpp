/*
 * SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "ucx_gaudi_backend.h"
#include "common/nixl_log.h"
#include "serdes/serdes.h"
#include "ucx/config.h"

#include <optional>
#include <limits>
#include <string.h>
#include <unistd.h>
#include "absl/strings/numbers.h"

// Gaudi-specific includes would go here in a real implementation
// For now, we'll provide a base implementation that extends UCX

namespace {
    void moveNotifList(notif_list_t &src, notif_list_t &tgt)
    {
        if (src.size() > 0) {
            std::move(src.begin(), src.end(), std::back_inserter(tgt));
            src.clear();
        }
    }
}

/****************************************
 * Gaudi context management
 *****************************************/

class nixlUcxGaudiCtx {
public:
    std::string gaudiDeviceId;
    bool gaudiInitialized;

    nixlUcxGaudiCtx() : gaudiInitialized(false) {
        // Initialize Gaudi context
        NIXL_INFO << "Initializing Gaudi context";
    }

    ~nixlUcxGaudiCtx() {
        if (gaudiInitialized) {
            NIXL_INFO << "Cleaning up Gaudi context";
        }
    }

    bool isGaudiMemory(void* ptr) {
        // Placeholder for Gaudi memory detection
        // In a real implementation, this would check if the pointer
        // references Gaudi device memory
        return false;
    }

    uint32_t getGaudiDeviceId(void* ptr) {
        // Placeholder for getting Gaudi device ID from memory pointer
        return 0;
    }
};

class nixlUcxGaudiDeviceCtx {
public:
    uint32_t deviceId;
    bool isActive;

    nixlUcxGaudiDeviceCtx(uint32_t id) : deviceId(id), isActive(false) {
        NIXL_DEBUG << "Creating Gaudi device context for device " << id;
    }

    bool activate() {
        // Placeholder for activating Gaudi device context
        isActive = true;
        return true;
    }

    void deactivate() {
        // Placeholder for deactivating Gaudi device context
        isActive = false;
    }
};

/****************************************
 * UCX Gaudi Backend implementation
 *****************************************/

[[nodiscard]] nixl_b_params_t get_ucx_gaudi_backend_common_options() {
    nixl_b_params_t params = {
        {"ucx_devices", ""},
        {"num_workers", "1"},
        {"gaudi_optimize", "true"},
        {"gaudi_transport", "gaudi"}
    };

    params.emplace(nixl_ucx_err_handling_param_name,
                   ucx_err_mode_to_string(UCP_ERR_HANDLING_MODE_PEER));
    return params;
}

nixlUcxGaudiEngine::nixlUcxGaudiEngine(const nixlBackendInitParams* init_params)
    : nixlBackendEngine(init_params), gaudiOptimizationsEnabled(false), gaudiTransportName("gaudi")
{
    NIXL_INFO << "Initializing UCX Gaudi backend engine";

    try {
        // Initialize Gaudi context
        gaudiInitCtx();

        // Check for Gaudi optimization enablement in parameters
        if (init_params && init_params->params) {
            auto it = init_params->params->find("gaudi_optimize");
            if (it != init_params->params->end() && it->second == "true") {
                gaudiOptimizationsEnabled = true;
                NIXL_INFO << "Gaudi optimizations enabled";
            }

            auto transport_it = init_params->params->find("gaudi_transport");
            if (transport_it != init_params->params->end()) {
                gaudiTransportName = transport_it->second;
                NIXL_DEBUG << "Using Gaudi transport: " << gaudiTransportName;
            }
        }

        // Initialize UCX context with Gaudi-specific configuration
        std::unique_ptr<nixlUcxConfig> cfg = std::make_unique<nixlUcxConfig>();
        
        if (gaudiOptimizationsEnabled) {
            // Configure UCX to prefer Gaudi transports
            cfg->set("TLS", "gaudi,rc_verbs,ud_verbs,rc_mlx5,ud_mlx5,tcp");
            NIXL_DEBUG << "Configured UCX with Gaudi transport preference";
        }

        uc = std::make_unique<nixlUcxContext>(*cfg);

        // Initialize workers
        size_t num_workers = 1;
        if (init_params && init_params->params) {
            auto workers_it = init_params->params->find("num_workers");
            if (workers_it != init_params->params->end()) {
                if (!absl::SimpleAtoi(workers_it->second, &num_workers) || num_workers == 0) {
                    num_workers = 1;
                }
            }
        }

        uws.reserve(num_workers);
        for (size_t i = 0; i < num_workers; ++i) {
            uws.emplace_back(std::make_unique<nixlUcxWorker>(*uc));
        }

        // Get worker address
        workerAddr = uws[0]->getWorkerAddress();
        NIXL_DEBUG << "UCX Gaudi worker address: " << workerAddr;

        // Initialize progress thread if needed
        pthrOn = false; // Start with progress thread disabled
        pthrActive = false;

        // Initialize notifications
        notifMainList.clear();
        notifPthrPriv.clear();
        notifPthr.clear();

        NIXL_INFO << "UCX Gaudi backend engine initialized successfully";

    } catch (const std::exception& e) {
        NIXL_ERROR << "Failed to initialize UCX Gaudi backend: " << e.what();
        throw;
    }
}

nixlUcxGaudiEngine::~nixlUcxGaudiEngine() {
    NIXL_INFO << "Destroying UCX Gaudi backend engine";

    // Stop progress thread
    if (pthrOn) {
        progressThreadStop();
    }

    // Clean up connections
    remoteConnMap.clear();

    // Clean up workers
    uws.clear();

    // Clean up UCX context
    uc.reset();

    // Clean up Gaudi context
    gaudiFiniCtx();
}

void nixlUcxGaudiEngine::gaudiInitCtx() {
    NIXL_DEBUG << "Initializing Gaudi context";
    gaudiCtx = std::make_unique<nixlUcxGaudiCtx>();
}

void nixlUcxGaudiEngine::gaudiFiniCtx() {
    NIXL_DEBUG << "Cleaning up Gaudi context";
    gaudiDeviceContexts.clear();
    gaudiCtx.reset();
}

int nixlUcxGaudiEngine::gaudiUpdateCtx(void *address, uint64_t devId, bool &restart_reqd) {
    // Placeholder for Gaudi context update
    restart_reqd = false;
    return 0;
}

int nixlUcxGaudiEngine::gaudiApplyCtx() {
    // Placeholder for applying Gaudi context
    return 0;
}

bool nixlUcxGaudiEngine::detectGaudiMemory(void* ptr, uint32_t& deviceId) {
    if (gaudiCtx) {
        if (gaudiCtx->isGaudiMemory(ptr)) {
            deviceId = gaudiCtx->getGaudiDeviceId(ptr);
            return true;
        }
    }
    return false;
}

nixl_status_t nixlUcxGaudiEngine::optimizeGaudiTransfer(const nixl_xfer_op_t &operation,
                                                       const nixl_meta_dlist_t &local,
                                                       const nixl_meta_dlist_t &remote,
                                                       const std::string &remote_agent) {
    if (!gaudiOptimizationsEnabled) {
        return NIXL_SUCCESS; // No optimization applied
    }

    // Placeholder for Gaudi-specific transfer optimizations
    NIXL_DEBUG << "Applying Gaudi optimizations for transfer to " << remote_agent;
    
    // Check if source and destination are both on Gaudi devices
    bool localIsGaudi = false;
    bool remoteIsGaudi = false;
    
    // In a real implementation, we would analyze the memory descriptors
    // to determine if they reference Gaudi memory and apply appropriate
    // optimizations such as:
    // - Direct Gaudi-to-Gaudi transfers
    // - Optimal transport selection
    // - Memory mapping optimizations
    
    if (localIsGaudi && remoteIsGaudi) {
        NIXL_INFO << "Detected Gaudi-to-Gaudi transfer, applying optimized path";
    }

    return NIXL_SUCCESS;
}

nixl_mem_list_t nixlUcxGaudiEngine::getSupportedMems() const {
    return {DRAM_SEG, VRAM_SEG};
}

nixl_status_t nixlUcxGaudiEngine::getPublicData(const nixlBackendMD* meta, std::string &str) const {
    // Delegate to base UCX implementation for now
    // In a full implementation, this might include Gaudi-specific metadata
    const nixlUcxGaudiPrivateMetadata* gaudi_meta = 
        dynamic_cast<const nixlUcxGaudiPrivateMetadata*>(meta);
    if (gaudi_meta) {
        str = gaudi_meta->get();
        return NIXL_SUCCESS;
    }
    return NIXL_ERR_INVALID_ARG;
}

nixl_status_t nixlUcxGaudiEngine::getConnInfo(std::string &str) const {
    str = workerAddr;
    return NIXL_SUCCESS;
}

nixl_status_t nixlUcxGaudiEngine::loadRemoteConnInfo(const std::string &remote_agent,
                                                    const std::string &remote_conn_info) {
    NIXL_DEBUG << "Loading remote connection info for agent: " << remote_agent;
    
    // Create Gaudi connection object
    auto conn = std::make_shared<nixlUcxGaudiConnection>();
    conn->remoteAgent = remote_agent;
    conn->gaudiOptimized = gaudiOptimizationsEnabled;
    
    remoteConnMap[remote_agent] = conn;
    
    return NIXL_SUCCESS;
}

nixl_status_t nixlUcxGaudiEngine::connect(const std::string &remote_agent) {
    NIXL_INFO << "Connecting to remote agent: " << remote_agent;
    
    auto it = remoteConnMap.find(remote_agent);
    if (it == remoteConnMap.end()) {
        return NIXL_ERR_NOT_FOUND;
    }
    
    // In a real implementation, this would establish UCX endpoints
    // with Gaudi-specific optimizations
    
    return NIXL_SUCCESS;
}

nixl_status_t nixlUcxGaudiEngine::disconnect(const std::string &remote_agent) {
    NIXL_INFO << "Disconnecting from remote agent: " << remote_agent;
    
    auto it = remoteConnMap.find(remote_agent);
    if (it != remoteConnMap.end()) {
        remoteConnMap.erase(it);
    }
    
    return NIXL_SUCCESS;
}

// Placeholder implementations for required interface methods
// In a real implementation, these would delegate to the base UCX implementation
// with Gaudi-specific enhancements

nixl_status_t nixlUcxGaudiEngine::registerMem(const nixlBlobDesc &mem,
                                             const nixl_mem_t &nixl_mem,
                                             nixlBackendMD* &out) {
    // Create Gaudi-aware metadata
    auto gaudi_meta = new nixlUcxGaudiPrivateMetadata();
    
    // Check if this is Gaudi memory
    uint32_t deviceId;
    gaudi_meta->isGaudiMemory = detectGaudiMemory(mem.addr, deviceId);
    gaudi_meta->gaudiDeviceId = deviceId;
    
    // Register memory with UCX (placeholder)
    // In real implementation, would call UCX memory registration
    
    out = gaudi_meta;
    return NIXL_SUCCESS;
}

nixl_status_t nixlUcxGaudiEngine::deregisterMem(nixlBackendMD* meta) {
    delete meta;
    return NIXL_SUCCESS;
}

nixl_status_t nixlUcxGaudiEngine::loadLocalMD(nixlBackendMD* input, nixlBackendMD* &output) {
    // Placeholder implementation
    output = input;
    return NIXL_SUCCESS;
}

nixl_status_t nixlUcxGaudiEngine::loadRemoteMD(const nixlBlobDesc &input,
                                              const nixl_mem_t &nixl_mem,
                                              const std::string &remote_agent,
                                              nixlBackendMD* &output) {
    // Create public metadata for remote access
    auto gaudi_meta = new nixlUcxGaudiPublicMetadata();
    
    // Find connection
    auto it = remoteConnMap.find(remote_agent);
    if (it != remoteConnMap.end()) {
        gaudi_meta->conn = it->second;
    }
    
    output = gaudi_meta;
    return NIXL_SUCCESS;
}

nixl_status_t nixlUcxGaudiEngine::unloadMD(nixlBackendMD* input) {
    delete input;
    return NIXL_SUCCESS;
}

nixl_status_t nixlUcxGaudiEngine::prepXfer(const nixl_xfer_op_t &operation,
                                          const nixl_meta_dlist_t &local,
                                          const nixl_meta_dlist_t &remote,
                                          const std::string &remote_agent,
                                          nixlBackendReqH* &handle,
                                          const nixl_opt_b_args_t* opt_args) const {
    // Apply Gaudi optimizations before transfer
    const_cast<nixlUcxGaudiEngine*>(this)->optimizeGaudiTransfer(operation, local, remote, remote_agent);
    
    // Placeholder for transfer preparation
    handle = nullptr;
    return NIXL_SUCCESS;
}

nixl_status_t nixlUcxGaudiEngine::postXfer(const nixl_xfer_op_t &operation,
                                          const nixl_meta_dlist_t &local,
                                          const nixl_meta_dlist_t &remote,
                                          const std::string &remote_agent,
                                          nixlBackendReqH* &handle,
                                          const nixl_opt_b_args_t* opt_args) const {
    // Placeholder for transfer posting
    handle = nullptr;
    return NIXL_SUCCESS;
}

nixl_status_t nixlUcxGaudiEngine::estimateXferCost(const nixl_xfer_op_t &operation,
                                                  const nixl_meta_dlist_t &local,
                                                  const nixl_meta_dlist_t &remote,
                                                  const std::string &remote_agent,
                                                  nixlBackendReqH* const &handle,
                                                  std::chrono::microseconds &duration,
                                                  std::chrono::microseconds &err_margin,
                                                  nixl_cost_t &method,
                                                  const nixl_opt_args_t* opt_args) const {
    // Placeholder cost estimation with Gaudi optimizations considered
    duration = std::chrono::microseconds(1000);
    err_margin = std::chrono::microseconds(100);
    method = NIXL_COST_UNKNOWN;
    return NIXL_SUCCESS;
}

nixl_status_t nixlUcxGaudiEngine::checkXfer(nixlBackendReqH* handle) const {
    // Placeholder for transfer status check
    return NIXL_SUCCESS;
}

nixl_status_t nixlUcxGaudiEngine::releaseReqH(nixlBackendReqH* handle) const {
    // Placeholder for request handle cleanup
    return NIXL_SUCCESS;
}

int nixlUcxGaudiEngine::progress() {
    // Placeholder progress implementation
    // Would call UCX progress with Gaudi-specific handling
    return 0;
}

nixl_status_t nixlUcxGaudiEngine::getNotifs(notif_list_t &notif_list) {
    // Placeholder notification handling
    std::lock_guard<std::mutex> lock(notifMtx);
    moveNotifList(notifMainList, notif_list);
    return NIXL_SUCCESS;
}

nixl_status_t nixlUcxGaudiEngine::genNotif(const std::string &remote_agent, const std::string &msg) const {
    NIXL_DEBUG << "Generating notification to " << remote_agent << ": " << msg;
    // Placeholder notification generation
    return NIXL_SUCCESS;
}

nixl_status_t nixlUcxGaudiEngine::checkConn(const std::string &remote_agent) {
    auto it = remoteConnMap.find(remote_agent);
    return (it != remoteConnMap.end()) ? NIXL_SUCCESS : NIXL_ERR_NOT_FOUND;
}

nixl_status_t nixlUcxGaudiEngine::endConn(const std::string &remote_agent) {
    return disconnect(remote_agent);
}

// Placeholder progress thread methods
void nixlUcxGaudiEngine::progressFunc() {
    // Placeholder progress thread function
}

void nixlUcxGaudiEngine::progressThreadStart() {
    // Placeholder progress thread start
}

void nixlUcxGaudiEngine::progressThreadStop() {
    // Placeholder progress thread stop
}

void nixlUcxGaudiEngine::progressThreadRestart() {
    // Placeholder progress thread restart
}

nixl_status_t nixlUcxGaudiEngine::internalMDHelper(const nixl_blob_t &blob,
                                                  const std::string &agent,
                                                  nixlBackendMD* &output) {
    // Placeholder metadata helper
    output = nullptr;
    return NIXL_SUCCESS;
}

ucs_status_t nixlUcxGaudiEngine::connectionCheckAmCb(void *arg, const void *header,
                                                    size_t header_length, void *data,
                                                    size_t length,
                                                    const ucp_am_recv_param_t *param) {
    // Placeholder connection check callback
    return UCS_OK;
}

ucs_status_t nixlUcxGaudiEngine::connectionTermAmCb(void *arg, const void *header,
                                                   size_t header_length, void *data,
                                                   size_t length,
                                                   const ucp_am_recv_param_t *param) {
    // Placeholder connection termination callback
    return UCS_OK;
}

ucs_status_t nixlUcxGaudiEngine::notifAmCb(void *arg, const void *header,
                                          size_t header_length, void *data,
                                          size_t length,
                                          const ucp_am_recv_param_t *param) {
    // Placeholder notification callback
    return UCS_OK;
}

nixl_status_t nixlUcxGaudiEngine::notifSendPriv(const std::string &remote_agent,
                                               const std::string &msg,
                                               nixlUcxReq &req,
                                               size_t worker_id) const {
    // Placeholder notification send
    return NIXL_SUCCESS;
}

void nixlUcxGaudiEngine::notifProgress() {
    // Placeholder notification progress
}

void nixlUcxGaudiEngine::notifProgressCombineHelper(notif_list_t &src, notif_list_t &tgt) {
    moveNotifList(src, tgt);
}