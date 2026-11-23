#pragma once
#include <stdint.h>
#include <stddef.h>
typedef struct system_info {
    size_t size; uint8_t hardwareVersion, hardwareType, hardwareType2;
    uint8_t osMajorVersion, osMinorVersion, osRevisionVersion; unsigned osBuildVersion;
    uint8_t bootMajorVersion, bootMinorVersion, bootRevisionVersion; unsigned bootBuildVersion;
    uint8_t unknown[10]; uint8_t calcid[8]; char ti[2]; uint16_t language;
} system_info_t;
const system_info_t *os_GetSystemInfo(void);
