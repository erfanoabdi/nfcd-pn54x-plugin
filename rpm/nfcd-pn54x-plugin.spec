Name: nfcd-pn54x-plugin
Version: 1.0.0
Release: 0
Summary: NFC plugin for pn54x
Group: Development/Libraries
License: BSD
URL: https://github.com/mer-hybris/nfcd-pn54x-plugin
Source: %{name}-%{version}.tar.bz2

%define nfcd_version 1.0.20

BuildRequires: pkgconfig(libncicore)
BuildRequires: pkgconfig(libnciplugin)
BuildRequires: pkgconfig(nfcd-plugin) >= %{nfcd_version}
Requires: nfcd >= %{nfcd_version}

%description
NFC plugin that talks directly to pn54x driver.

%prep
%setup -q

%build
make %{_smp_mflags} KEEP_SYMBOLS=1 release

%install
rm -rf %{buildroot}
make install DESTDIR=%{buildroot}

%check
make test

%post
systemctl reload-or-try-restart nfcd.service ||:

%postun
systemctl reload-or-try-restart nfcd.service ||:

%files
%defattr(-,root,root,-)
%dir %{_libdir}/nfcd/plugins
%{_libdir}/nfcd/plugins/*.so
