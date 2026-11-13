# Put this directory under <sdk>/middleware/v2/sample/lichee_capture30,
# after building the official SDK middleware for LicheeRV Nano / SG2002.
SHELL = /bin/bash
ifeq ($(PARAM_FILE),)
PARAM_FILE := ../../Makefile.param
include $(PARAM_FILE)
endif
include ../sample.mk

SDIR := $(CURDIR)
SRCS := $(wildcard $(SDIR)/*.c)
INCS := -I$(MW_INC) -I$(ISP_INC) -I../common -I$(KERNEL_INC)
OBJS := $(SRCS:.c=.o)
DEPS := $(SRCS:.c=.d)

TARGET := lichee_capture30
PKG_CONFIG_PATH := $(MW_PATH)/pkgconfig
REQUIRES := cvi_common cvi_sample
MW_LIBS := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --libs --define-variable=mw_dir=$(MW_PATH) $(REQUIRES))
LIBS := $(MW_LIBS)
ifeq ($(MULTI_PROCESS_SUPPORT),1)
DEFS += -DRPC_MULTI_PROCESS
LIBS += -lnanomsg
endif
EXTRA_CFLAGS := $(INCS) $(DEFS)
EXTRA_LDFLAGS := $(LIBS) -lpthread -lm -lini

.PHONY: all clean
all: $(TARGET)

$(SDIR)/%.o: $(SDIR)/%.c
	@$(CC) $(DEPFLAGS) $(CFLAGS) $(EXTRA_CFLAGS) -o $@ -c $<
	@echo [$(notdir $(CC))] $(notdir $@)

$(TARGET): $(COMM_OBJ) $(OBJS) $(ISP_OBJ) $(MW_LIB)/libvpu.a $(MW_LIB)/libsys.a
	@$(CXX) -o $@ -Wl,--start-group $(OBJS) $(COMM_OBJS) -lsys $(MW_LIB)/libsys.a -Wl,--end-group $(ELFFLAGS) $(EXTRA_LDFLAGS)
	@echo [LINK] $(TARGET)

clean:
	@rm -f $(OBJS) $(DEPS) $(TARGET)

-include $(DEPS)
