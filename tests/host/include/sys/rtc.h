#pragma once
#include <stdint.h>
#include <stdbool.h>
void boot_SetDate(uint8_t day, uint8_t month, uint16_t year);
void boot_GetDate(uint8_t *day, uint8_t *month, uint16_t *year);
void boot_SetTime(uint8_t s, uint8_t m, uint8_t h);
void boot_GetTime(uint8_t *s, uint8_t *m, uint8_t *h);
