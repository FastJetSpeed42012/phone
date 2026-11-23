#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
uint8_t ti_Open(const char *name, const char *mode);
uint8_t ti_OpenVar(const char *name, const char *mode, uint8_t type);
int ti_Close(uint8_t h);
size_t ti_Read(void *data, size_t size, size_t count, uint8_t h);
size_t ti_Write(const void *data, size_t size, size_t count, uint8_t h);
uint16_t ti_GetSize(uint8_t h);
int ti_IsArchived(uint8_t h);
int ti_SetArchiveStatus(bool a, uint8_t h);
int ti_Delete(const char *name);
int ti_DeleteVar(const char *name, uint8_t type);
char *ti_DetectAny(void **vat_ptr, const char *detect, uint8_t *type);
