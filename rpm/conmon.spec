%global with_debug 1

%if 0%{?with_debug}
%global _find_debuginfo_dwz_opts %{nil}
%global _dwz_low_mem_die_limit 0
%else
%global debug_package %{nil}
%endif

%if %{defined rhel}
%bcond_with docs
%else
%bcond_without docs
%endif

Name: conmon
%if %{defined rhel}
Epoch: 3
%else
Epoch: 2
%endif
Version: 2.2.1
License: Apache-2.0
Release: %autorelease
Summary: OCI container runtime monitor
URL: https://github.com/containers/%{name}
# Tarball fetched from upstream
Source0: %{url}/archive/v%{version}.tar.gz
%if %{with docs}
BuildRequires: go-md2man
%endif
BuildRequires: gcc
BuildRequires: git-core
BuildRequires: make
BuildRequires: pkgconfig(libsystemd)
BuildRequires: pkgconfig(glib-2.0)
BuildRequires: pkgconfig(libseccomp)

%description
%{summary}.

%prep
%autosetup -Sgit %{name}-%{version}
sed -i 's/install.bin: bin\/conmon/install.bin:/' Makefile

%build
%make_build bin/conmon CFLAGS="%{optflags}" LDFLAGS="%{build_ldflags}"

%if %{with docs}
%make_build GOMD2MAN=go-md2man -C docs
%endif

%install
%{__make} PREFIX=%{buildroot}%{_prefix} install.bin

%if %{with docs}
%{__make} PREFIX=%{buildroot}%{_prefix} -C docs install
%endif

%files
%license LICENSE
%doc README.md
%{_bindir}/%{name}

%if %{with docs}
%{_mandir}/man8/%{name}.8.gz
%endif

%changelog
%autochangelog
