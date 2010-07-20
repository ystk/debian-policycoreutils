############################ -*- Mode: Makefile -*- ###########################
## local.mk --- 
## Author           : Manoj Srivastava ( srivasta@glaurung.green-gryphon.com ) 
## Created On       : Sat Nov 15 10:42:10 2003
## Created On Node  : glaurung.green-gryphon.com
## Last Modified By : Manoj Srivastava
## Last Modified On : Wed Apr  2 23:02:24 2008
## Last Machine Used: anzu.internal.golden-gryphon.com
## Update Count     : 33
## Status           : Unknown, Use with caution!
## HISTORY          : 
## Description      : 
## 
## arch-tag: b07b1015-30ba-4b46-915f-78c776a808f4
## 
###############################################################################

testdir:
	$(testdir)

debian/stamp/BUILD/policycoreutils: debian/stamp/build/policycoreutils
debian/stamp/INST/policycoreutils:  debian/stamp/install/policycoreutils
debian/stamp/BIN/policycoreutils:   debian/stamp/binary/policycoreutils


CLN-common::
	$(REASON)
	-test ! -f Makefile || $(MAKE) clean

CLEAN/policycoreutils::
	-rm -rf $(TMPTOP)

debian/stamp/build/policycoreutils:
	$(checkdir)
	$(REASON)
	@test -d debian/stamp/build || mkdir -p debian/stamp/build
	bash -n debian/preinst
	bash -n debian/postinst
	bash -n debian/prerm
	bash -n debian/postrm
	$(MAKE) CC="$(CC)" CFLAGS="$(CFLAGS)" LDFLAGS="$(LDFLAGS)" \
	     PYLIBVER=$(PYDEFAULT) PYTHONLIBDIR=$(MODULES_DIR)
# Can not use the canned sequence, since there is an error
ifeq (,$(strip $(filter nocheck,$(DEB_BUILD_OPTIONS))))
  ifeq ($(DEB_BUILD_GNU_TYPE),$(DEB_HOST_GNU_TYPE))
	@echo Checking libs
	extra=$$($(SHELL) debian/common/checklibs); \
	 if [ -n "$$extra" ]; then                  \
	   echo "Extra libraries: $$extra";         \
           echo "/lib/libdl.so.2 is spurious";      \
	 fi
  endif
endif
	@echo done > $@


debian/stamp/install/policycoreutils:
	$(checkdir)
	$(REASON)
	$(TESTROOT)
	@test -d debian/stamp/install || mkdir -p debian/stamp/install
	rm -rf               $(TMPTOP)
	$(make_directory)    $(TMPTOP)
	$(make_directory)    $(BINDIR)
	$(make_directory)    $(SBINDIR)
	$(make_directory)    $(SELINUXDIR)
	$(make_directory)    $(PAMDIR)
	$(make_directory)    $(MODULES_DIR)
	$(make_directory)    $(MAN1DIR)
	$(make_directory)    $(DOCDIR)
	$(make_directory)    $(LINTIANDIR)
	$(make_directory)    $(TMPTOP)/etc/selinux
	test ! -f            debian/$(package).lintian ||       \
	  $(install_file)    debian/$(package).lintian          \
                             $(LINTIANDIR)/$(package)
	chmod 500	     $(SELINUXDIR)
	$(MAKE)              $(INT_INSTALL_TARGET)  DESTDIR=$(TMPTOP)            \
                             INSTALL_PROGRAM="$(install_program)"                \
                             PYLIBVER=$(PYDEFAULT)  PYTHONLIBDIR=$(MODULES_DIR)
	mv -f                $(MODULES_DIR)/site-packages/*  $(MODULES_DIR)/;
	rmdir                $(MODULES_DIR)/site-packages;
	$(install_file)      debian/run_init.pam	$(PAMDIR)/run_init
	$(install_file)      debian/newrole.pam		$(PAMDIR)/newrole
	$(install_script)    debian/se_dpkg             $(SBINDIR)
	ln -s                se_dpkg                    $(SBINDIR)/se_apt-get
	ln -s                se_dpkg                    $(SBINDIR)/se_dselect
	ln -s                se_dpkg                    $(SBINDIR)/se_dpkg-reconfigure
	ln -s                se_dpkg                    $(SBINDIR)/se_aptitude
	ln -s                se_dpkg                    $(SBINDIR)/se_synaptic
	test ! -d            $(TMPTOP)/etc/cron.weekly ||                              \
           rm -rf            $(TMPTOP)/etc/cron.weekly
	$(install_file)      debian/changelog       $(DOCDIR)/changelog.Debian
	$(install_file)      ChangeLog              $(DOCDIR)/changelog
	$(install_file)      debian/NEWS.Debian     $(DOCDIR)/NEWS.Debian 
	$(install_file)      debian/config	    $(DOCDIR)/etc_selinux_config
	$(install_file)      debian/se_dpkg.8	    $(MAN8DIR)/
	echo                 ".so man8/se_dpkg.8"  >$(MAN8DIR)/se_apt-get.8
	echo                 ".so man8/se_dpkg.8"  >$(MAN8DIR)/se_aptitude.8
	echo                 ".so man8/se_dpkg.8"  >$(MAN8DIR)/se_dpkg-reconfigure.8
	echo                 ".so man8/se_dpkg.8"  >$(MAN8DIR)/se_dselect.8
	echo                 ".so man8/se_dpkg.8"  >$(MAN8DIR)/se_synaptic.8
	gzip -9frq           $(DOCDIR)/
# Make sure the copyright file is not compressed
	$(install_file)      debian/copyright       $(DOCDIR)/copyright
	gzip -9fqr           $(MANDIR)/
	chmod 0755           $(TMPTOP)/etc/init.d/$(package)
	test ! -d            $(TMPTOP)/usr/share/locale/     ||                           \
                               find $(TMPTOP)/usr/share/locale/ -type d -name LC_MESSAGES \
                                  -depth -exec rmdir -p --ignore-fail-on-non-empty {} \;
	test ! -d            $(TMPTOP)/usr/lib/  ||                                       \
                               find $(TMPTOP)/usr/lib/  -type d -name LC_MESSAGES         \
                                 -depth -exec rmdir -p --ignore-fail-on-non-empty {} \;
ifneq ($(PYVERSION),$(PYDEFAULT))
	find $(TMPTOP) -type f | xargs file -N -f - \
                | sed -nr 's/^([^:]+):.*\<python\>.*\<script\>.*$$/\1/ip;' \
                | xargs sed -ri '1s/python($$| )/python$(PYVERSION)\1/;'
endif
	$(strip-exec)
	$(strip-lib)
	@echo done > $@

debian/stamp/binary/policycoreutils:
	$(checkdir)
	$(REASON)
	$(TESTROOT)
	@test -d debian/stamp/binary || mkdir -p debian/stamp/binary
	$(make_directory)    $(TMPTOP)/DEBIAN
	$(install_file)      debian/conffiles	    $(TMPTOP)/DEBIAN/
	$(install_script)    debian/postinst        $(TMPTOP)/DEBIAN/postinst
	$(install_script)    debian/preinst         $(TMPTOP)/DEBIAN/preinst 
	$(install_script)    debian/prerm           $(TMPTOP)/DEBIAN/prerm
	$(install_script)    debian/postrm          $(TMPTOP)/DEBIAN/postrm
	$(get-shlib-deps)
	dpkg-gencontrol      -p$(package) -isp      -P$(TMPTOP)
	$(create_md5sum)     $(TMPTOP)
	chown -R root:root   $(TMPTOP)
	chmod -R u+w,go=rX   $(TMPTOP)
	dpkg --build         $(TMPTOP) ..
	@echo done > $@

