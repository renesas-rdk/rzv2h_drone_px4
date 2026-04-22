/*
 * Copyright (c) 2025 Renesas Electronics Corporation and/or its affiliates
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file param_converter.cpp
 * @brief Implementation of transparent text ↔ BSON parameter converter
 */

#include "param_converter.hpp"
#include <cstring>
#include <cstdio>
#include <sstream>
#include <iomanip>
#include <regex>
#include <iostream>

namespace px4_param_converter {

// BSON type constants
constexpr uint8_t BSON_DOUBLE = 0x01;
constexpr uint8_t BSON_STRING = 0x02;
constexpr uint8_t BSON_INT32  = 0x10;
constexpr uint8_t BSON_EOO    = 0x00;

/**
 * Write little-endian int32 to buffer
 */
static void write_int32_le(std::vector<uint8_t> &buf, int32_t value) {
    buf.push_back((value) & 0xFF);
    buf.push_back((value >> 8) & 0xFF);
    buf.push_back((value >> 16) & 0xFF);
    buf.push_back((value >> 24) & 0xFF);
}

/**
 * Write double to buffer (little-endian IEEE 754)
 */
static void write_double_le(std::vector<uint8_t> &buf, double value) {
    uint64_t bits;
    std::memcpy(&bits, &value, sizeof(double));
    for (int i = 0; i < 8; i++) {
        buf.push_back((bits >> (i * 8)) & 0xFF);
    }
}

/**
 * Write C-string (null-terminated) to buffer
 */
static void write_cstring(std::vector<uint8_t> &buf, const std::string &str) {
    buf.insert(buf.end(), str.begin(), str.end());
    buf.push_back(0);
}

/**
 * Detect value type and parse
 */
struct ParamValue {
    enum Type { INT, FLOAT, STRING } type;
    union {
        int32_t i;
        double f;
    };
    std::string s;
};

static ParamValue parse_value(const std::string &value_str) {
    ParamValue result;

    // If value has decimal point or scientific notation, always treat as float
    if (value_str.find('.') != std::string::npos ||
        value_str.find('e') != std::string::npos ||
        value_str.find('E') != std::string::npos) {
        try {
            result.f = std::stod(value_str);
            result.type = ParamValue::FLOAT;
            return result;
        } catch (...) {}
    }

    // Try to parse as number (integer or float)
    bool is_hex = (value_str.find("0x") == 0) || (value_str.find("-0x") == 1);

    try {
        size_t pos;
        int32_t int_val;

        if (is_hex) {
            int_val = std::stoi(value_str, &pos, 16);
        } else {
            int_val = std::stoi(value_str, &pos);
        }

        if (pos == value_str.length()) {
            // Successfully parsed as integer.
            // Always encode as float to avoid per-parameter type lists.
            result.type = ParamValue::FLOAT;
            result.f = static_cast<double>(int_val);
            return result;
        }
    } catch (...) {}

    // Fall back to string
    result.type = ParamValue::STRING;
    result.s = value_str;
    return result;
}

bool text_to_bson(const std::string &text_content, std::vector<uint8_t> &bson_output) {
    bson_output.clear();

    // Reserve space for document size (will be filled at the end)
    bson_output.resize(4);

    // Parse text line by line
    std::istringstream iss(text_content);
    std::string line;
    std::regex param_regex(R"(^\s*param\s+set\s+(\S+)\s+(.+))");

    while (std::getline(iss, line)) {
        // Remove comments
        size_t comment_pos = line.find('#');
        if (comment_pos != std::string::npos) {
            line = line.substr(0, comment_pos);
        }

        // Trim whitespace
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);

        if (line.empty()) continue;

        // Match "param set NAME VALUE"
        std::smatch match;
        if (!std::regex_match(line, match, param_regex)) {
            continue;
        }

        std::string param_name = match[1];
        std::string value_str = match[2];

        // Trim value
        value_str.erase(0, value_str.find_first_not_of(" \t"));
        value_str.erase(value_str.find_last_not_of(" \t") + 1);

        // Parse value and determine type (numeric values encode as double)
        ParamValue value = parse_value(value_str);

        // Encode BSON element
        if (value.type == ParamValue::INT) {
            bson_output.push_back(BSON_INT32);
            write_cstring(bson_output, param_name);
            write_int32_le(bson_output, value.i);
        } else if (value.type == ParamValue::FLOAT) {
            bson_output.push_back(BSON_DOUBLE);
            write_cstring(bson_output, param_name);
            write_double_le(bson_output, value.f);
        } else {
            bson_output.push_back(BSON_STRING);
            write_cstring(bson_output, param_name);
            write_int32_le(bson_output, value.s.length() + 1);
            write_cstring(bson_output, value.s);
        }
    }

    // Add EOO marker
    bson_output.push_back(BSON_EOO);

    // Write document size at the beginning
    int32_t doc_size = bson_output.size();
    bson_output[0] = (doc_size) & 0xFF;
    bson_output[1] = (doc_size >> 8) & 0xFF;
    bson_output[2] = (doc_size >> 16) & 0xFF;
    bson_output[3] = (doc_size >> 24) & 0xFF;

    return true;
}

/**
 * Read little-endian int32 from buffer
 */
static int32_t read_int32_le(const uint8_t *buf) {
    return buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);
}

/**
 * Read double from buffer (little-endian IEEE 754)
 */
static double read_double_le(const uint8_t *buf) {
    uint64_t bits = 0;
    for (int i = 0; i < 8; i++) {
        bits |= (static_cast<uint64_t>(buf[i]) << (i * 8));
    }
    double value;
    std::memcpy(&value, &bits, sizeof(double));
    return value;
}

bool bson_to_text(const uint8_t *bson_data, size_t bson_size, std::string &text_output) {
    if (bson_size < 5) {
        return false; // Minimum BSON document size
    }

    int32_t doc_size = read_int32_le(bson_data);
    if (doc_size != static_cast<int32_t>(bson_size)) {
        std::cerr << "BSON size mismatch: header=" << doc_size << " actual=" << bson_size << std::endl;
        return false;
    }

    text_output.clear();
    text_output += "# PX4 Parameters (auto-generated from BSON)\n";
    text_output += "# Edit this file and it will be converted to BSON when CR8 reads it\n\n";

    size_t pos = 4; // Skip document size

    while (pos < bson_size) {
        uint8_t type = bson_data[pos++];

        if (type == BSON_EOO) {
            break; // End of document
        }

        // Read parameter name (null-terminated string)
        std::string param_name;
        while (pos < bson_size && bson_data[pos] != 0) {
            param_name += static_cast<char>(bson_data[pos++]);
        }
        pos++; // Skip null terminator

        // Read value based on type
        if (type == BSON_INT32) {
            if (pos + 4 > bson_size) return false;
            int32_t value = read_int32_le(&bson_data[pos]);
            pos += 4;
            text_output += "param set " + param_name + " " + std::to_string(value) + "\n";
        } else if (type == BSON_DOUBLE) {
            if (pos + 8 > bson_size) return false;
            double value = read_double_le(&bson_data[pos]);
            pos += 8;
            // Preserve precision so round-trip survives param_verify (float epsilon)
            std::ostringstream oss;
            oss << std::setprecision(15) << value;
            std::string value_str = oss.str();
            text_output += "param set " + param_name + " " + value_str + "\n";
        } else if (type == BSON_STRING) {
            if (pos + 4 > bson_size) return false;
            int32_t str_len = read_int32_le(&bson_data[pos]);
            pos += 4;
            if (pos + str_len > bson_size) return false;
            std::string value(reinterpret_cast<const char *>(&bson_data[pos]), str_len - 1);
            pos += str_len;
            text_output += "param set " + param_name + " " + value + "\n";
        } else {
            std::cerr << "Unknown BSON type: " << static_cast<int>(type) << std::endl;
            return false;
        }
    }

    return true;
}

bool should_convert_params(const char *path) {
    if (!path) return false;

    // CR8 uses upstream /fs/microsd/ convention; RPC server translates to /drone-data/cr8_data/.
    // Only the binary BSON paths trigger conversion — .txt virtual paths are dead code.
    return std::strcmp(path, "/fs/microsd/params") == 0
           || std::strcmp(path, "/fs/microsd/etc/params") == 0;
}

} // namespace px4_param_converter
