/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 *
 * Test-only rename fault injection for transactional CLI output checks.
 */
#include <errno.h>
#include <stdio.h>

int rename(const char *old_path, const char *new_path) {
    (void)old_path;
    (void)new_path;
    errno = EIO;
    return -1;
}
