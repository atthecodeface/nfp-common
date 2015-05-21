#a Copyright (C) 2015,  Gavin J Stark.  All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# @file        Makefile
# @brief       Top level makefile to set paths and include submakes
#

NFP_COMMON    := $(abspath $(CURDIR))
NETRONOME = /opt/netronome

SCRIPTS_DIR   = $(NFP_COMMON)/scripts
FIRMWARE_DIR  = $(NFP_COMMON)/firmware
HOST_DIR      = $(NFP_COMMON)/host
DOCS_DIR      = $(NFP_COMMON)/docs

include $(FIRMWARE_DIR)/Makefile
include $(HOST_DIR)/Makefile
include $(SCRIPTS_DIR)/Makefile
#include $(DOCS_DIR)/Makefile

ALL: firmware_all

clean: firmware_clean
