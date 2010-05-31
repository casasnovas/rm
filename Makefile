include config.mk

all: $(BINARY_NAME)

print_src_dir:
	@echo $(SRCDIR)

$(BINARY_NAME):
	make ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) -C src/
	mv $(SRCDIR)/src/$(BINARY_NAME).ko ./

.PHONY: doc

doc:
	doxygen Doxyfile
	make -C doc/

install: all
	install ./$(BINARY_NAME).ko $(MODDIR)/misc/
	depmod -a
	@echo "dyn_hotplug ressource manager is now installed."

uninstall:
	rm -f $(MODDIR)/misc/$(BINARY_NAME).ko

tarball:
	git archive --format=tar --prefix=$(PROJECT_NAME)-$(PROJECT_VERSION)/ master > $(PROJECT_NAME)-$(PROJECT_VERSION).tar
	bzip2 $(PROJECT_NAME)-$(PROJECT_VERSION).tar

clean:
	make -C src/ clean
	make -C doc/ clean
	rm -f $(BINARY_NAME).ko
