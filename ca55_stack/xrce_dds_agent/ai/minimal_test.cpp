/*
 * Copyright (c) 2025 Renesas Electronics Corporation and/or its affiliates
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

// Minimal test: EXACT same flow as run_inference_v2h.cpp
// Tests if our CMake build can produce a working DRP-AI binary
#include <linux/drpai.h>
#include <iostream>
#include <fstream>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include "MeraDrpRuntimeWrapper.h"

uint64_t get_drpai_start_addr()
{
    int fd = open("/dev/drpai0", O_RDWR);
    if (fd < 0) {
        std::cerr << "Failed to open /dev/drpai0: errno=" << errno << std::endl;
        return 0;
    }
    drpai_data_t drpai_data;
    int ret = ioctl(fd, DRPAI_GET_DRPAI_AREA, &drpai_data);
    if (ret == -1) {
        std::cerr << "Failed DRPAI_GET_DRPAI_AREA: errno=" << errno << std::endl;
        close(fd);
        return 0;
    }
    close(fd);
    return drpai_data.address;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cout << "Usage: minimal_test <model_dir>" << std::endl;
        return 1;
    }
    std::string model_dir = argv[1];

    MeraDrpRuntimeWrapper runtime;
    uint64_t addr = get_drpai_start_addr();
    if (addr == 0) return 1;
    std::cout << "DRP-AI start: 0x" << std::hex << addr << std::dec << std::endl;

    std::cout << "Loading model..." << std::endl;
    runtime.LoadModel(model_dir, addr);
    std::cout << "Model loaded OK!" << std::endl;

    // Load input from file (same as official)
    std::string input_file = model_dir + "/input_0.bin";
    std::ifstream file(input_file, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Cannot open " << input_file << std::endl;
        return 1;
    }
    file.seekg(0, file.end);
    uint32_t fsize = file.tellg();
    file.seekg(0, file.beg);
    std::vector<float> input(fsize / sizeof(float));
    file.read((char*)input.data(), fsize);
    file.close();
    std::cout << "Input loaded: " << input.size() << " floats" << std::endl;

    runtime.SetInput(0, input.data());

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    runtime.Run();
    clock_gettime(CLOCK_MONOTONIC, &t1);

    double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    std::cout << "Inference: " << ms << " ms" << std::endl;

    auto output = runtime.GetOutput(0);
    std::cout << "Output got, pointer: " << std::get<1>(output) << std::endl;
    std::cout << "SUCCESS!" << std::endl;
    return 0;
}
