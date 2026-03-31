CC      = gcc
CFLAGS  = -c -g -O2 -Wall -Wno-pointer-sign
LDFLAGS = -luuid

GIT2LOG	:= $(shell if [ -x ./git2log ] ; then echo ./git2log --update ; else echo true ; fi)
GITDEPS	:= $(shell [ -d .git ] && echo .git/HEAD .git/refs/heads .git/refs/tags)
VERSION	:= $(shell $(GIT2LOG) --version VERSION ; cat VERSION)
BRANCH	:= $(shell [ -d .git ] && git branch | perl -ne 'print $$_ if s/^\*\s*//')
BINDIR	 = /usr/bin
LIBDIR	 = /usr/lib

all: changelog isohybrid parti

isohybrid:
	@make -C tools/isohybrid

parti:
	@make -C tools/parti

# make_package is in package linuxrc-devtools
archive:
	@make_package

changelog: $(GITDEPS)
	$(GIT2LOG) --changelog changelog

install: isohybrid parti doc
	@cp mkmedia mkmedia.tmp
	@perl -pi -e 's/0\.0/$(VERSION)/ if /VERSION = /' mkmedia.tmp
	@perl -pi -e 's#"(.*)"#"$(LIBDIR)"# if /LIBEXECDIR = /' mkmedia.tmp
	@cp verifymedia verifymedia.tmp
	@perl -pi -e 's/0\.0/$(VERSION)/ if /VERSION = /' verifymedia.tmp
	@perl -pi -e 's#"(.*)"#"$(LIBDIR)"# if /LIBEXECDIR = /' verifymedia.tmp
	@cp isozipl isozipl.tmp
	@perl -pi -e 's/0\.0/$(VERSION)/ if /VERSION = /' isozipl.tmp
	@cp tools/mnt/mnt mnt.tmp
	@perl -pi -e 's/0\.0/$(VERSION)/ if /VERSION = /' mnt.tmp
	@perl -pi -e 's#"(.*)"#"$(LIBDIR)"# if /LIBEXECDIR = /' mnt.tmp
	install -m 755 -D mkmedia.tmp $(DESTDIR)$(BINDIR)/mkmedia
	@ln -snf mkmedia $(DESTDIR)$(BINDIR)/mksusecd
	install -m 755 -D verifymedia.tmp $(DESTDIR)$(BINDIR)/verifymedia
	install -m 755 -D isozipl.tmp $(DESTDIR)$(BINDIR)/isozipl
	install -m 755 -D tools/isohybrid/isohybrid $(DESTDIR)$(LIBDIR)/mkmedia/isohybrid
	install -m 755 -D tools/parti/parti $(DESTDIR)$(LIBDIR)/mkmedia/parti
	install -m 755 -D mnt.tmp $(DESTDIR)$(LIBDIR)/mkmedia/mnt
	install -m 755 -D tools/mnt/umnt $(DESTDIR)$(LIBDIR)/mkmedia/umnt
	@rm -f mkmedia.tmp verifymedia.tmp isozipl.tmp mnt.tmp

%.1: %_man.adoc
	@if [ -x /usr/bin/asciidoctor ] ; then \
	  asciidoctor -b manpage -a version=$(VERSION) $< ; \
	fi

doc: mkmedia.1 mksusecd.1 verifymedia.1
	@if [ -x /usr/bin/asciidoctor ] ; then \
	  asciidoctor suse_blog.adoc ; \
	fi

# a2x -f docbook -a version=$(VERSION) mkmedia_man.adoc
# dblatex mkmedia_man.xml

clean:
	@make -C tools/isohybrid clean
	@make -C tools/parti clean
	@rm -f *.o *~ *.tmp */*~ mkmedia{_man.xml,_man.pdf} verifymedia{_man.xml,_man.pdf}
	@rm -rf package
