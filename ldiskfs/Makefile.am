AUTOMAKE_OPTIONS = foreign
SUBDIRS = ldiskfs
EXTRA_DIST = @PACKAGE_TARNAME@.spec
EXTRA_DIST += kernel_patches
EXTRA_DIST += config
EXTRA_DIST += META

dist-hook:
	find $(distdir) -name .deps -o \
			-name .git -o \
			-name .#* | xargs rm -rf

srpm: dist
	srpmpkg=$(PACKAGE_NAME)-$(PACKAGE_VERSION)-$(PACKAGE_RELEASE).src.rpm; \
	rpmspec=$(PACKAGE_NAME).spec; \
	rpmbuild=`mktemp -t -d $(PACKAGE_NAME)-build-$$USER-XXXXXXXX`; \
	$(MAKE) $(AM_MAKEFLAGS) \
		rpmbuild="$$rpmbuild" \
		rpmspec="$$rpmspec" \
		rpm-local || exit 1; \
	$(RPMBUILD) \
		--define "_tmppath $$rpmbuild/TMP" \
		--define "_topdir $$rpmbuild" \
		--define "build_src_rpm 1" \
		--define "dist %{nil}" \
		--nodeps -bs $$rpmbuild/SPECS/$$rpmspec || exit 1; \
	cp $$rpmbuild/SRPMS/$$srpmpkg . || exit 1; \
	$(RM) -R $$rpmbuild

rpm: srpm
	srpmpkg=$(PACKAGE_NAME)-$(PACKAGE_VERSION)-$(PACKAGE_RELEASE).src.rpm; \
	rpmspec=$(PACKAGE_NAME).spec; \
	rpmbuild=`mktemp -t -d $(PACKAGE_NAME)-build-$$USER-XXXXXXXX`; \
	$(MAKE) $(AM_MAKEFLAGS) \
		rpmbuild="$$rpmbuild" \
		rpmspec="$$rpmspec" \
		rpm-local || exit 1; \
	$(RPMBUILD) \
		--define "_tmppath $$rpmbuild/TMP" \
		--define "_topdir $$rpmbuild" \
		--define "dist %{nil}" \
		--define "require_kdir $(LINUX)" \
		--define "require_kobj $(LINUX_OBJ)" \
		--define "require_kver $(LINUX_VERSION)" \
		--nodeps --rebuild $$srpmpkg || exit 1; \
	cp $$rpmbuild/RPMS/*/* . || exit 1; \
	$(RM) -R $$rpmbuild

# rpm-local - this target is used by the rpm and srpm targets to
# create a temporary working tree for the rpmbuild command.
rpm-local:
	@(if test "${HAVE_RPMBUILD}" = "no"; then \
		echo -e "\n" \
	"*** Required util ${RPMBUILD} missing.  Please install the\n" \
	"*** package for your distribution which provides ${RPMBUILD},\n" \
	"*** re-run configure, and try again.\n"; \
		exit 1; \
	fi; \
	mkdir -p $(rpmbuild)/TMP && \
	mkdir -p $(rpmbuild)/BUILD && \
	mkdir -p $(rpmbuild)/RPMS && \
	mkdir -p $(rpmbuild)/SRPMS && \
	mkdir -p $(rpmbuild)/SPECS && \
	cp $(rpmspec) $(rpmbuild)/SPECS && \
	mkdir -p $(rpmbuild)/SOURCES && \
	cp $(distdir).tar.gz $(rpmbuild)/SOURCES)
