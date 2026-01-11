PROJECT_NAME := UsbMitm
USB_MITM_TID := 010000000000bc00

TARGETS := usb_mitm

all: $(TARGETS)

usb_mitm:
	$(MAKE) -C $@

clean:
	$(MAKE) -C usb_mitm clean
	rm -rf dist

dist: all
	rm -rf dist

	mkdir -p dist/atmosphere/contents/$(USB_MITM_TID)
	cp usb_mitm/out/nintendo_nx_arm64_armv8a/release/usb_mitm.nsp dist/atmosphere/contents/$(USB_MITM_TID)/exefs.nsp

	mkdir -p dist/atmosphere/contents/$(USB_MITM_TID)/flags
	touch dist/atmosphere/contents/$(USB_MITM_TID)/flags/boot2.flag
	echo "usb:hs" >> dist/atmosphere/contents/$(USB_MITM_TID)/mitm.lst

	cd dist; zip -r $(PROJECT_NAME)-$(BUILD_VERSION).zip ./*; cd ../;

.PHONY: all clean dist $(TARGETS)
