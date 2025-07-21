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

#include <iostream>
#include <sstream>
#include <string>
#include <cassert>
#include <memory>

#include "ucx_gaudi_backend.h"

using namespace std;

class UcxGaudiBackendTest {
private:
    std::unique_ptr<nixlUcxGaudiEngine> engine;
    nixlBackendInitParams init_params;

public:
    UcxGaudiBackendTest() {
        // Initialize test parameters
        nixl_b_params_t params = {
            {"gaudi_optimize", "true"},
            {"gaudi_transport", "gaudi"},
            {"num_workers", "1"}
        };
        init_params.params = &params;
    }

    ~UcxGaudiBackendTest() = default;

    bool testEngineCreation() {
        cout << "Testing UCX Gaudi engine creation..." << endl;
        
        try {
            engine = std::make_unique<nixlUcxGaudiEngine>(&init_params);
            
            // Verify engine was created
            if (!engine) {
                cerr << "Failed to create UCX Gaudi engine" << endl;
                return false;
            }
            
            cout << "✓ Engine creation successful" << endl;
            return true;
            
        } catch (const std::exception& e) {
            cerr << "Exception during engine creation: " << e.what() << endl;
            return false;
        }
    }

    bool testEngineCapabilities() {
        if (!engine) {
            cerr << "Engine not initialized" << endl;
            return false;
        }

        cout << "Testing UCX Gaudi engine capabilities..." << endl;

        // Test basic capabilities
        bool remote_support = engine->supportsRemote();
        bool local_support = engine->supportsLocal();
        bool notif_support = engine->supportsNotif();
        bool prog_th_support = engine->supportsProgTh();

        cout << "  Remote support: " << (remote_support ? "Yes" : "No") << endl;
        cout << "  Local support: " << (local_support ? "Yes" : "No") << endl;
        cout << "  Notification support: " << (notif_support ? "Yes" : "No") << endl;
        cout << "  Progress thread support: " << (prog_th_support ? "Yes" : "No") << endl;

        // Verify expected capabilities
        if (!remote_support || !local_support || !notif_support) {
            cerr << "Missing expected capabilities" << endl;
            return false;
        }

        cout << "✓ Engine capabilities verified" << endl;
        return true;
    }

    bool testSupportedMemoryTypes() {
        if (!engine) {
            cerr << "Engine not initialized" << endl;
            return false;
        }

        cout << "Testing supported memory types..." << endl;

        nixl_mem_list_t supported_mems = engine->getSupportedMems();
        
        bool has_dram = false;
        bool has_vram = false;

        for (const auto& mem_type : supported_mems) {
            cout << "  Supported memory type: " << static_cast<int>(mem_type) << endl;
            if (mem_type == DRAM_SEG) has_dram = true;
            if (mem_type == VRAM_SEG) has_vram = true;
        }

        if (!has_dram || !has_vram) {
            cerr << "Missing expected memory types (DRAM/VRAM)" << endl;
            return false;
        }

        cout << "✓ Memory types verified" << endl;
        return true;
    }

    bool testConnectionInfo() {
        if (!engine) {
            cerr << "Engine not initialized" << endl;
            return false;
        }

        cout << "Testing connection info..." << endl;

        std::string conn_info;
        nixl_status_t status = engine->getConnInfo(conn_info);

        if (status != NIXL_SUCCESS) {
            cerr << "Failed to get connection info" << endl;
            return false;
        }

        if (conn_info.empty()) {
            cerr << "Connection info is empty" << endl;
            return false;
        }

        cout << "  Connection info length: " << conn_info.length() << endl;
        cout << "✓ Connection info retrieved" << endl;
        return true;
    }

    bool testGaudiOptimizations() {
        if (!engine) {
            cerr << "Engine not initialized" << endl;
            return false;
        }

        cout << "Testing Gaudi-specific optimizations..." << endl;

        // Test Gaudi optimization enablement
        bool gaudi_enabled = engine->isGaudiOptimizationEnabled();
        cout << "  Gaudi optimizations enabled: " << (gaudi_enabled ? "Yes" : "No") << endl;

        // Test Gaudi transport name
        const std::string& transport_name = engine->getGaudiTransportName();
        cout << "  Gaudi transport name: " << transport_name << endl;

        if (!gaudi_enabled) {
            cerr << "Gaudi optimizations should be enabled" << endl;
            return false;
        }

        if (transport_name != "gaudi") {
            cerr << "Unexpected Gaudi transport name: " << transport_name << endl;
            return false;
        }

        cout << "✓ Gaudi optimizations verified" << endl;
        return true;
    }

    bool testMemoryRegistration() {
        if (!engine) {
            cerr << "Engine not initialized" << endl;
            return false;
        }

        cout << "Testing memory registration..." << endl;

        // Create a small memory buffer
        const size_t buffer_size = 4096;
        void* buffer = malloc(buffer_size);
        if (!buffer) {
            cerr << "Failed to allocate test buffer" << endl;
            return false;
        }

        // Create memory descriptor
        nixlBlobDesc mem_desc;
        mem_desc.addr = buffer;
        mem_desc.len = buffer_size;

        nixl_mem_t nixl_mem = DRAM_SEG;
        nixlBackendMD* backend_md = nullptr;

        // Test memory registration
        nixl_status_t status = engine->registerMem(mem_desc, nixl_mem, backend_md);
        
        if (status != NIXL_SUCCESS) {
            cerr << "Failed to register memory" << endl;
            free(buffer);
            return false;
        }

        if (!backend_md) {
            cerr << "Backend metadata is null" << endl;
            free(buffer);
            return false;
        }

        // Test memory deregistration
        status = engine->deregisterMem(backend_md);
        if (status != NIXL_SUCCESS) {
            cerr << "Failed to deregister memory" << endl;
            free(buffer);
            return false;
        }

        free(buffer);
        cout << "✓ Memory registration/deregistration successful" << endl;
        return true;
    }

    bool runAllTests() {
        cout << "=== UCX Gaudi Backend Test Suite ===" << endl;
        
        bool all_passed = true;
        
        all_passed &= testEngineCreation();
        all_passed &= testEngineCapabilities();
        all_passed &= testSupportedMemoryTypes();
        all_passed &= testConnectionInfo();
        all_passed &= testGaudiOptimizations();
        all_passed &= testMemoryRegistration();
        
        cout << endl;
        if (all_passed) {
            cout << "✓ All tests passed!" << endl;
        } else {
            cout << "✗ Some tests failed!" << endl;
        }
        
        return all_passed;
    }
};

int main() {
    UcxGaudiBackendTest test;
    
    bool success = test.runAllTests();
    
    return success ? 0 : 1;
}