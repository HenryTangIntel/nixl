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
#ifndef NIXL_SRC_PLUGINS_UCX_GAUDI_UCX_GAUDI_BACKEND_H
#define NIXL_SRC_PLUGINS_UCX_GAUDI_UCX_GAUDI_BACKEND_H

#include <vector>
#include <cstring>
#include <iostream>
#include <thread>
#include <mutex>
#include <memory>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <poll.h>

#include "nixl.h"
#include "backend/backend_engine.h"
#include "common/str_tools.h"

// Local includes
#include "common/nixl_time.h"
#include "ucx/rkey.h"
#include "ucx/ucx_utils.h"
#include "common/list_elem.h"

// Gaudi-specific includes would go here
// For now, we'll inherit from the base UCX implementation

class nixlUcxGaudiConnection : public nixlBackendConnMD {
    private:
        std::string remoteAgent;
        std::vector<std::unique_ptr<nixlUcxEp>> eps;
        bool gaudiOptimized;

    public:
        [[nodiscard]] const std::unique_ptr<nixlUcxEp>& getEp(size_t ep_id) const noexcept {
            return eps[ep_id];
        }

        [[nodiscard]] bool isGaudiOptimized() const noexcept {
            return gaudiOptimized;
        }

    friend class nixlUcxGaudiEngine;
};

using ucx_gaudi_connection_ptr_t = std::shared_ptr<nixlUcxGaudiConnection>;

// Gaudi-specific private metadata
class nixlUcxGaudiPrivateMetadata : public nixlBackendMD {
    private:
        nixlUcxMem mem;
        nixl_blob_t rkeyStr;
        bool isGaudiMemory;
        uint32_t gaudiDeviceId;

    public:
        nixlUcxGaudiPrivateMetadata() : nixlBackendMD(true), isGaudiMemory(false), gaudiDeviceId(0) {
        }

        [[nodiscard]] const std::string& get() const noexcept {
            return rkeyStr;
        }

        [[nodiscard]] bool isOnGaudiDevice() const noexcept {
            return isGaudiMemory;
        }

        [[nodiscard]] uint32_t getGaudiDeviceId() const noexcept {
            return gaudiDeviceId;
        }

    friend class nixlUcxGaudiEngine;
};

// Gaudi-specific public metadata
class nixlUcxGaudiPublicMetadata : public nixlBackendMD {
public:
    nixlUcxGaudiPublicMetadata() : nixlBackendMD(false) {}

    [[nodiscard]] const nixl::ucx::rkey &
    getRkey(size_t id) const {
        return *rkeys_[id];
    }

    void
    addRkey(const nixlUcxEp &ep, const void *rkey_buffer) {
        rkeys_.emplace_back(std::make_unique<nixl::ucx::rkey>(ep, rkey_buffer));
    }

    ucx_gaudi_connection_ptr_t conn;

private:
    std::vector<std::unique_ptr<nixl::ucx::rkey>> rkeys_;
};

// Forward declarations for Gaudi context management
class nixlUcxGaudiCtx;
class nixlUcxGaudiDeviceCtx;
using nixlUcxGaudiDeviceCtxPtr = std::shared_ptr<nixlUcxGaudiDeviceCtx>;

class nixlUcxGaudiEngine
    : public nixlBackendEngine {
    private:
        /* UCX data */
        std::unique_ptr<nixlUcxContext> uc;
        std::vector<std::unique_ptr<nixlUcxWorker>> uws;
        std::string workerAddr;

        /* Progress thread data */
        std::mutex pthrActiveLock;
        std::condition_variable pthrActiveCV;
        bool pthrActive;
        bool pthrOn;
        std::thread pthr;
        std::chrono::milliseconds pthrDelay;
        int pthrControlPipe[2];
        std::vector<pollfd> pollFds;

        /* Gaudi-specific data */
        std::unique_ptr<nixlUcxGaudiCtx> gaudiCtx;
        std::vector<nixlUcxGaudiDeviceCtxPtr> gaudiDeviceContexts;
        bool gaudiOptimizationsEnabled;
        std::string gaudiTransportName;

        /* Notifications */
        notif_list_t notifMainList;
        std::mutex  notifMtx;
        notif_list_t notifPthrPriv, notifPthr;

        // Map of agent name to saved nixlUcxGaudiConnection info
        std::unordered_map<std::string, ucx_gaudi_connection_ptr_t,
                           std::hash<std::string>, strEqual> remoteConnMap;

        // Gaudi-specific helper methods
        void gaudiInitCtx();
        void gaudiFiniCtx();
        int gaudiUpdateCtx(void *address, uint64_t devId, bool &restart_reqd);
        int gaudiApplyCtx();
        bool detectGaudiMemory(void* ptr, uint32_t& deviceId);
        nixl_status_t optimizeGaudiTransfer(const nixl_xfer_op_t &operation,
                                           const nixl_meta_dlist_t &local,
                                           const nixl_meta_dlist_t &remote,
                                           const std::string &remote_agent);

        // Threading infrastructure (inherited from UCX)
        void progressFunc();
        void progressThreadStart();
        void progressThreadStop();
        void progressThreadRestart();
        bool isProgressThread() const noexcept {
            return std::this_thread::get_id() == pthr.get_id();
        }

        // Connection helper callbacks
        static ucs_status_t
        connectionCheckAmCb(void *arg, const void *header,
                            size_t header_length, void *data,
                            size_t length,
                            const ucp_am_recv_param_t *param);

        static ucs_status_t
        connectionTermAmCb(void *arg, const void *header,
                           size_t header_length, void *data,
                           size_t length,
                           const ucp_am_recv_param_t *param);

        // Memory management helpers
        nixl_status_t internalMDHelper (const nixl_blob_t &blob,
                                        const std::string &agent,
                                        nixlBackendMD* &output);

        // Notifications
        static ucs_status_t notifAmCb(void *arg, const void *header,
                                      size_t header_length, void *data,
                                      size_t length,
                                      const ucp_am_recv_param_t *param);
        nixl_status_t notifSendPriv(const std::string &remote_agent,
                                    const std::string &msg,
                                    nixlUcxReq &req,
                                    size_t worker_id) const;
        void notifProgress();
        void notifProgressCombineHelper(notif_list_t &src, notif_list_t &tgt);

    public:
        nixlUcxGaudiEngine(const nixlBackendInitParams* init_params);
        ~nixlUcxGaudiEngine();

        bool supportsRemote() const override { return true; }
        bool supportsLocal() const override { return true; }
        bool supportsNotif() const override { return true; }
        bool supportsProgTh() const override { return pthrOn; }

        nixl_mem_list_t getSupportedMems() const override;

        /* Object management */
        nixl_status_t getPublicData (const nixlBackendMD* meta,
                                     std::string &str) const override;
        nixl_status_t getConnInfo(std::string &str) const override;
        nixl_status_t loadRemoteConnInfo (const std::string &remote_agent,
                                          const std::string &remote_conn_info) override;

        nixl_status_t connect(const std::string &remote_agent) override;
        nixl_status_t disconnect(const std::string &remote_agent) override;

        nixl_status_t registerMem (const nixlBlobDesc &mem,
                                   const nixl_mem_t &nixl_mem,
                                   nixlBackendMD* &out) override;
        nixl_status_t deregisterMem (nixlBackendMD* meta) override;

        nixl_status_t loadLocalMD (nixlBackendMD* input,
                                   nixlBackendMD* &output) override;

        nixl_status_t loadRemoteMD (const nixlBlobDesc &input,
                                    const nixl_mem_t &nixl_mem,
                                    const std::string &remote_agent,
                                    nixlBackendMD* &output) override;
        nixl_status_t unloadMD (nixlBackendMD* input) override;

        // Data transfer with Gaudi optimizations
        nixl_status_t prepXfer (const nixl_xfer_op_t &operation,
                                const nixl_meta_dlist_t &local,
                                const nixl_meta_dlist_t &remote,
                                const std::string &remote_agent,
                                nixlBackendReqH* &handle,
                                const nixl_opt_b_args_t* opt_args=nullptr) const override;

        nixl_status_t estimateXferCost(const nixl_xfer_op_t &operation,
                                       const nixl_meta_dlist_t &local,
                                       const nixl_meta_dlist_t &remote,
                                       const std::string &remote_agent,
                                       nixlBackendReqH* const &handle,
                                       std::chrono::microseconds &duration,
                                       std::chrono::microseconds &err_margin,
                                       nixl_cost_t &method,
                                       const nixl_opt_args_t* opt_args=nullptr) const override;

        nixl_status_t postXfer (const nixl_xfer_op_t &operation,
                                const nixl_meta_dlist_t &local,
                                const nixl_meta_dlist_t &remote,
                                const std::string &remote_agent,
                                nixlBackendReqH* &handle,
                                const nixl_opt_b_args_t* opt_args=nullptr) const override;

        nixl_status_t checkXfer (nixlBackendReqH* handle) const override;
        nixl_status_t releaseReqH(nixlBackendReqH* handle) const override;

        int progress() override;

        nixl_status_t getNotifs(notif_list_t &notif_list);
        nixl_status_t genNotif(const std::string &remote_agent, const std::string &msg) const override;

        //public function for UCX worker to mark connections as connected
        nixl_status_t checkConn(const std::string &remote_agent);
        nixl_status_t endConn(const std::string &remote_agent);

        const std::unique_ptr<nixlUcxWorker> &getWorker(size_t worker_id) const {
            return uws[worker_id];
        }

        size_t getWorkerId() const {
            return std::hash<std::thread::id>{}(std::this_thread::get_id()) % uws.size();
        }

        // Gaudi-specific public methods
        bool isGaudiOptimizationEnabled() const { return gaudiOptimizationsEnabled; }
        const std::string& getGaudiTransportName() const { return gaudiTransportName; }
};

// Function to get UCX Gaudi backend options
[[nodiscard]] nixl_b_params_t get_ucx_gaudi_backend_common_options();

#endif