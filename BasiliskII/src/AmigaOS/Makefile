# AmigaOS makefile for Basilisk II (GeekGadgets tool chain)

## System specific configuration
CC = gcc
CXX = c++
CXXFLAGS = -g -O1 -noixemul -m68020 -msmall-code -Wno-multichar
CPPFLAGS = -I../include -I../native_cpu -I. -I../../../shared/include
DEFS =
LDFLAGS = -noixemul
LIBS = /gg/lib/libnix/swapstack.o
AS = PhxAss
ASFLAGS = OPT ! INCPATH GG:os-include FPU=1

## Files
SRCS = ../main.cpp main_amiga.cpp ../../../shared/prefs.cpp ../prefs_items.cpp \
    prefs_amiga.cpp prefs_editor_amiga.cpp sys_amiga.cpp ../rom_patches.cpp \
    ../slot_rom.cpp ../rsrc_patches.cpp ../emul_op.cpp \
    ../macos_util.cpp ../../../shared/xpram.cpp xpram_amiga.cpp ../../../shared/timer.cpp \
    timer_amiga.cpp clip_amiga.cpp ../../../shared/adb.cpp ../serial.cpp \
    serial_amiga.cpp ../ether.cpp ether_amiga.cpp ../../../shared/sony.cpp ../../../shared/disk.cpp \
    ../../../shared/cdrom.cpp ../../../shared/scsi.cpp scsi_amiga.cpp ../video.cpp video_amiga.cpp \
    ../../../shared/audio.cpp audio_amiga.cpp ../../../shared/extfs.cpp extfs_amiga.cpp \
    ../user_strings.cpp user_strings_amiga.cpp asm_support.asm
APP = BasiliskII

## Rules
.PHONY: clean distclean
.SUFFIXES:
.SUFFIXES: .c .cpp .asm .o .h

all: $(APP)

OBJ_DIR = obj
$(OBJ_DIR)::
	@[ -d $(OBJ_DIR) ] || mkdir $(OBJ_DIR) > /dev/null 2>&1

define SRCS_LIST_TO_OBJS
	$(addprefix $(OBJ_DIR)/, $(addsuffix .o, $(foreach file, $(SRCS), \
	$(basename $(notdir $(file))))))
endef
OBJS = $(SRCS_LIST_TO_OBJS)

SRC_PATHS += $(sort $(foreach file, $(SRCS), $(dir $(file))))
VPATH :=
VPATH += $(addprefix :, $(subst  ,:, $(filter-out $($(subst, :, ,$(VPATH))), $(SRC_PATHS))))

$(APP): $(OBJ_DIR) $(OBJS)
	$(CXX) -o $(APP) $(LDFLAGS) $(LIBS) $(OBJS)

clean:
	rm -f $(APP) $(OBJ_DIR)/* *~ *.bak obj.0000.*

distclean: clean
	rm -rf $(OBJ_DIR)

$(OBJ_DIR)/%.o : %.cpp
	$(CXX) $(CPPFLAGS) $(DEFS) $(CXXFLAGS) -c $< -o $@
$(OBJ_DIR)/%.o : %.asm
	$(AS) $(ASFLAGS) $< TO $(OBJ_DIR)/$*.obj
	hunk2aout $(OBJ_DIR)/$*.obj >/dev/null
	mv obj.0000.* $@

#-------------------------------------------------------------------------
# DO NOT DELETE THIS LINE -- make depend depends on it.
