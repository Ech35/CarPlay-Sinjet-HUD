#ifndef HUD_RGI_COMMON_H
#define HUD_RGI_COMMON_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static inline uint16_t read_be16(const uint8_t *p) { return (uint16_t)((p[0]<<8)|p[1]); }
static inline uint32_t read_be32(const uint8_t *p) { return ((uint32_t)read_be16(p)<<16)|read_be16(p+2); }
static inline uint64_t read_be64(const uint8_t *p) { return ((uint64_t)read_be32(p)<<32)|read_be32(p+4); }
static inline void write_be16(uint8_t *p,uint16_t v) { p[0]=(uint8_t)(v>>8);p[1]=(uint8_t)v; }
#endif
