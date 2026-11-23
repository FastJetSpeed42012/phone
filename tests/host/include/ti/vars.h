#pragma once
#include <stddef.h>
#include <stdint.h>
#define OS_TYPE_REAL 0x00
#define OS_TYPE_STR 0x04
#define OS_TYPE_PRGM 0x05
#define OS_TYPE_PROT_PRGM 0x06
#define OS_TYPE_APPVAR 0x15
extern uint32_t os_TempFreeArc;
size_t os_MemChk(void **free);
void os_ArcChk(void);
typedef int (*os_runprgm_callback_t)(void *data, int retval);
int os_RunPrgm(const char *prgm, void *data, size_t size, os_runprgm_callback_t cb);
