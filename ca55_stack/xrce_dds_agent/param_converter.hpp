/*
 * Copyright (c) 2025 Renesas Electronics Corporation and/or its affiliates
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file param_converter.hpp
 * @brief Transparent text ↔ BSON converter for PX4 parameters
 *
 * This allows users to edit params in text format (like rc.board_defaults.cmds)
 * while CR8 reads/writes them in BSON format transparently.
 */

#pragma once

#include <string>
#include <vector>
#include <map>
#include <cstdint>

namespace px4_param_converter {

/**
 * Parse text param file (format: "param set NAME VALUE")
 * and convert to BSON format
 *
 * @param text_content Text file content
 * @param bson_output Output buffer for BSON data
 * @return true on success, false on error
 */
bool text_to_bson(const std::string &text_content, std::vector<uint8_t> &bson_output);

/**
 * Parse BSON param data and convert to text format
 * (format: "param set NAME VALUE")
 *
 * @param bson_data BSON data buffer
 * @param bson_size Size of BSON data
 * @param text_output Output buffer for text content
 * @return true on success, false on error
 */
bool bson_to_text(const uint8_t *bson_data, size_t bson_size, std::string &text_output);

/**
 * Check if a file path should be handled with transparent conversion
 * @param path File path
 * @return true if this file should be converted
 */
bool should_convert_params(const char *path);

} // namespace px4_param_converter
