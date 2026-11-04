/* Test-provider FI address ABI shared by the adapter and interoperability app. */
#ifndef INTEROP_REF_ADDRESS_H
#define INTEROP_REF_ADDRESS_H
#include "uet_api.h"
#define REF_ADDRESS_MAGIC 0x55455455u
struct ref_address {
    uint32_t magic, raw_len;
    struct uet_addr uet;
    unsigned char raw[64];
};
#endif
