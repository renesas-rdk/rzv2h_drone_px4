/*
 * Copyright (c) 2025 Renesas Electronics Corporation and/or its affiliates
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */


#ifndef HELPER_H
#define HELPER_H

#ifdef __cplusplus
extern "C" {
#endif

/** flag SIGINT or SIGTERM have been received */
extern int force_stop;

/* External functions */
int init_system(void);
void cleanup_system(void);
void copy_data(uint8_t* dest, const volatile uint8_t* src, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* HELPER_H */
