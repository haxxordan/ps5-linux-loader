ifndef PS5_PAYLOAD_SDK
$(error PS5_PAYLOAD_SDK is undefined)
endif

include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

CFLAGS := -std=c11 -Wall -Wextra -Werror -O2

fw1270_hv_page.elf: fw1270_hv_page.c
	$(CC) $(CFLAGS) -o $@ $<

# 0x6283d800 is a 13.60 VMEXIT-handler lead, not a verified 12.70 offset.
fw1270_hv_3d000.elf: fw1270_hv_page.c
	$(CC) $(CFLAGS) -DHV_PAGE_PA=0x6283d000ULL \
	    -DHV_PROBE_PA=0x6283d800ULL -o $@ $<

.PHONY: clean
clean:
	rm -f fw1270_hv_page.elf fw1270_hv_3d000.elf
