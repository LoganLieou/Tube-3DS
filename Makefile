#---------------------------------------------------------------------------------
# Tube3DS - YouTube client for the Nintendo 3DS
#
#   make deps   build third-party libs + CIA tools into ./deps (first time only)
#   make        build Tube3DS.3dsx (Homebrew Launcher)
#   make cia    build Tube3DS.cia  (install with FBI / any CIA installer)
#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

TARGET		:=	Tube3DS
BUILD		:=	build
SOURCES		:=	source source/ui source/net source/yt source/media source/third_party
DATA		:=	data
INCLUDES	:=	source
GRAPHICS	:=	gfx
GFXBUILD	:=	$(BUILD)
ROMFS		:=	romfs

APP_TITLE		:=	Tube3DS
APP_DESCRIPTION	:=	YouTube for your 3DS
APP_AUTHOR		:=	Tube3DS
ICON			:=	assets/icon.png

# CIA metadata
CIA_BANNER_IMAGE	:=	assets/banner.png
CIA_BANNER_AUDIO	:=	assets/banner.wav
CIA_RSF				:=	assets/app.rsf
CIA_TOOLS			:=	$(CURDIR)/deps/tools/bin
MAKEROM				?=	$(if $(wildcard $(CIA_TOOLS)/makerom),$(CIA_TOOLS)/makerom,makerom)
BANNERTOOL			?=	$(if $(wildcard $(CIA_TOOLS)/bannertool),$(CIA_TOOLS)/bannertool,bannertool)

# Locally built dependencies (see scripts/build_deps.sh)
DEPS_PREFIX	:=	$(CURDIR)/deps/prefix

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH	:=	-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS	:=	-g -Wall -O2 -mword-relocations \
			-ffunction-sections -fdata-sections \
			$(ARCH)

CFLAGS	+=	$(INCLUDE) -D__3DS__

CXXFLAGS	:= $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++17

ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map) -Wl,--gc-sections

LIBS	:= -lavformat -lavcodec -lavutil -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz \
		   -lcitro2d -lcitro3d -lctru -lm

#---------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level containing
# include and lib
#---------------------------------------------------------------------------------
LIBDIRS	:= $(DEPS_PREFIX) $(CTRULIB)


#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
			$(foreach dir,$(GRAPHICS),$(CURDIR)/$(dir)) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
PICAFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.v.pica)))
SHLISTFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.shlist)))
GFXFILES	:=	$(foreach dir,$(GRAPHICS),$(notdir $(wildcard $(dir)/*.t3s)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

export LD	:=	$(CXX)

export T3XFILES :=  $(GFXFILES:.t3s=.t3x)

export OFILES_SOURCES 	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)

export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES)) \
			$(PICAFILES:.v.pica=.shbin.o) $(SHLISTFILES:.shlist=.shbin.o) \
			$(addsuffix .o,$(T3XFILES))

export OFILES := $(OFILES_BIN) $(OFILES_SOURCES)

export HFILES	:=	$(PICAFILES:.v.pica=_shbin.h) $(SHLISTFILES:.shlist=_shbin.h) \
			$(addsuffix .h,$(subst .,_,$(BINFILES))) \
			$(GFXFILES:.t3s=.h)

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export _3DSXDEPS	:=	$(OUTPUT).smdh

export APP_ICON := $(TOPDIR)/$(ICON)

export _3DSXFLAGS += --smdh=$(CURDIR)/$(TARGET).smdh
export _3DSXFLAGS += --romfs=$(CURDIR)/$(ROMFS)

.PHONY: all clean cia deps distclean

#---------------------------------------------------------------------------------
all: $(DEPS_PREFIX)/lib/libavcodec.a $(BUILD) $(DEPSDIR)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

$(DEPS_PREFIX)/lib/libavcodec.a:
	@echo "Dependencies missing - building them first (one-time, takes a few minutes)..."
	@./scripts/build_deps.sh

deps:
	@./scripts/build_deps.sh

$(BUILD):
	@mkdir -p $@

#---------------------------------------------------------------------------------
# CIA packaging
#---------------------------------------------------------------------------------
cia: all
	@echo "packaging $(TARGET).cia ..."
	@mkdir -p $(BUILD)
	@$(BANNERTOOL) makebanner -i $(CIA_BANNER_IMAGE) -a $(CIA_BANNER_AUDIO) -o $(BUILD)/banner.bnr > /dev/null
	@$(BANNERTOOL) makesmdh -s "$(APP_TITLE)" -l "$(APP_DESCRIPTION)" -p "$(APP_AUTHOR)" \
		-i $(ICON) -f visible,allow3d,recordusage -o $(BUILD)/icon.icn > /dev/null
	@$(MAKEROM) -f cia -o $(TARGET).cia -target t -exefslogo \
		-elf $(TARGET).elf -rsf $(CIA_RSF) \
		-banner $(BUILD)/banner.bnr -icon $(BUILD)/icon.icn \
		-DAPP_TITLE="$(APP_TITLE)" -DAPP_ROMFS="$(ROMFS)"
	@echo "built ... $(TARGET).cia"

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(OUTPUT).smdh $(TARGET).elf $(TARGET).cia

distclean: clean
	@rm -rf deps

#---------------------------------------------------------------------------------
else

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
$(OUTPUT).3dsx	:	$(OUTPUT).elf $(_3DSXDEPS)

$(OFILES_SOURCES) : $(HFILES)

$(OUTPUT).elf	:	$(OFILES)

%.bin.o	%_bin.h :	%.bin
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPSDIR)/*.d

#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------
