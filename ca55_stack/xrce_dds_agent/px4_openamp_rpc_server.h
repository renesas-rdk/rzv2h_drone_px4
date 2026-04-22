/*
 * Copyright (c) 2025 Renesas Electronics Corporation and/or its affiliates
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <cstddef>
#include <cstdint>

struct rpmsg_endpoint;

bool px4_openamp_rpc_server_handle(struct rpmsg_endpoint *ept, const void *data, std::size_t len);
bool px4_openamp_rpc_server_init_async(void);
void px4_openamp_rpc_server_shutdown_async(void);
bool px4_openamp_rpc_server_enqueue(struct rpmsg_endpoint *ept, const void *data, std::size_t len);
