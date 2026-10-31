# G814Keys.kext: a Lilu plugin, plain clang against acidanthera's MacKernelSDK and Lilu's plugin SDK (no Xcode project).
# MKSDK = a MacKernelSDK checkout, LILU = a Lilu.kext DEBUG build (its Contents/Resources holds the plugin SDK).
MKSDK    ?= MacKernelSDK
LILU     ?= Lilu.kext
SDK      ?= $(shell xcrun --sdk macosx --show-sdk-path)
# Apple's clang: another clang (Homebrew's LLVM) registers the kext's global destructors through __cxa_atexit, which the kernel
# does not export, and the kext then fails to link at load.
CC       ?= $(shell xcrun -f clang)
CXX      ?= $(shell xcrun -f clang++)
OUT      ?= build
VERSION  := 1.0.1
KEXT     := $(OUT)/G814Keys.kext
DEFS     := -DPRODUCT_NAME=G814Keys -DMODULE_VERSION=$(VERSION) -D__KERNEL__ -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT
INC      := -nostdinc -I$(MKSDK)/Headers -I$(LILU)/Contents/Resources
CFLAGS   := -arch x86_64 -mkernel -fno-builtin -fno-common -mmacosx-version-min=10.15 $(INC) $(DEFS) -O2
CXXFLAGS := -arch x86_64 -mkernel -fno-builtin -fno-common -fno-exceptions -fno-rtti -fapple-kext -mmacosx-version-min=10.15 \
            $(INC) $(DEFS) -std=gnu++17 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-inconsistent-missing-override
LDFLAGS  := -arch x86_64 -isysroot $(SDK) -L$(MKSDK)/Library/x86_64 -Xlinker -kext -nostdlib -lkmodc++ -lkmod -lcc_kext \
            -mmacosx-version-min=10.15

all: $(KEXT)

$(OUT)/G814Keys.o: G814Keys/G814Keys.cpp
	mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUT)/plugin_start.o: $(LILU)/Contents/Resources/Library/plugin_start.cpp
	mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUT)/G814Keys_info.o: G814Keys/G814Keys_info.c
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@

$(KEXT): $(OUT)/G814Keys.o $(OUT)/plugin_start.o $(OUT)/G814Keys_info.o G814Keys/Info.plist
	rm -rf $(KEXT); mkdir -p $(KEXT)/Contents/MacOS
	$(CXX) $(LDFLAGS) $(OUT)/G814Keys.o $(OUT)/plugin_start.o $(OUT)/G814Keys_info.o -o $(KEXT)/Contents/MacOS/G814Keys
	cp G814Keys/Info.plist $(KEXT)/Contents/Info.plist
	chmod -R 755 $(KEXT)
	shasum -a 256 $(KEXT)/Contents/MacOS/G814Keys

clean:
	rm -rf $(OUT)
.PHONY: all clean
