# ----------------------------
# CE Command Prompt - makefile
# Build with the CE C toolchain (CEdev): https://ce-programming.github.io/toolchain/
# ----------------------------

NAME = CMD
ICON = icon.png
DESCRIPTION = "CE Command Prompt"
COMPRESSED = YES
ARCHIVED = YES

CFLAGS = -Wall -Wextra -Oz
CXXFLAGS = -Wall -Wextra -Oz

# ----------------------------

include $(shell cedev-config --makefile)
