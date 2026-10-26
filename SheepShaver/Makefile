# Source distributions retain the sibling SheepShaver/ and shared/ directories
VERSION := 2.3
SRCARCHIVE := SheepShaver-$(VERSION)-$(shell date +%Y%m%d).tar.gz

.PHONY: help clean tarball
default: help

help:
	@echo "Build from src/Unix, src/Windows, src/MacOSX, or with bash build_wahe.sh."
	@echo "Shared sources are in ../shared; no make links step is needed."
	@echo "  tarball  source archive ($(SRCARCHIVE)), including shared/"

tarball:
	sh ../tools/make-source-archive.sh SheepShaver $(VERSION) "$(CURDIR)/$(SRCARCHIVE)"

clean:
	rm -f $(SRCARCHIVE)
