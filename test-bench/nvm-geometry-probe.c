/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "patch_config.h"
#include "nvm_emu.inc"

#define CHECK(x) do { if(!(x)) return __LINE__; } while(0)

int main(void){
    uint8_t byte=0;
    CHECK(nvm_init(&byte,1u,0u,0u)==-1);
    CHECK(sc_flash==NULL && sc_flash_n==0u && nvm_canary_bad()==0u);
    CHECK(nvm_init(NULL,0u,UINT32_MAX,0u)==-1);
    CHECK(sc_flash==NULL && sc_flash_n==0u && nvm_canary_bad()==0u);
    puts("nvm_geometry_contract=OK (from<=span + page-rounding headroom)");
    return 0;
}
