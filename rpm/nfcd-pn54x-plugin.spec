Name: nfcd-pn54x-plugin
Version: 1.0.2
Release: 0
Summary: NFC plugin for pn54x
License: BSD
URL: https://github.com/mer-hybris/nfcd-pn54x-plugin
Source: %{name}-%{version}.tar.bz2

%define nfcd_version 1.0.20

BuildRequires: pkgconfig(libncicore)
BuildRequires: pkgconfig(libnciplugin)
BuildRequires: pkgconfig(nfcd-plugin) >= %{nfcd_version}
Requires: nfcd >= %{nfcd_version}

%define plugin_dir %{_libdir}/nfcd/plugins

%description
NFC plugin that talks directly to pn54x driver.

%prep
%setup -q

%build
make %{_smp_mflags} KEEP_SYMBOLS=1 release

%install
rm -rf %{buildroot}
make DESTDIR=%{buildroot} PLUGIN_DIR=%{plugin_dir} install

%check
make test

%post
systemctl reload-or-try-restart nfcd.service ||:

%postun
systemctl reload-or-try-restart nfcd.service ||:

%files
%defattr(-,root,root,-)
%dir %{plugin_dir}
%{plugin_dir}/*.so
