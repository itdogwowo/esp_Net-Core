// crc32（與 esp_rom_crc32_le 同一個多項式，init=0、不取反）
#pragma once
#include <stddef.h>
unsigned int esp_rom_crc32_le(unsigned int crc, const void *buf, unsigned int len);
