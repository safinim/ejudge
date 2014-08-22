Name: ejudge
Version: 3.0
Release: 1%{?dist}
Summary: A programming contest management system
Source: %{name}-%{version}.tgz
License: GPL
URL: http://ejudge.ru
BuildArch: i386
BuildArch: x86_64
#BuildRequires:
#Requires:

%description
A programming contest management system. http://ejudge.ru

%prep
%autosetup -n %{name}

%build
%configure --enable-charset=utf-8 --with-httpd-cgi-bin-dir=/var/www/cgi-bin --with-httpd-htdocs-dir=/var/www/html --enable-ajax --enable-local-dir=/var/lib/ejudge --enable-hidden-server-bins --disable-rpath
make %{?_smp_mflags}

%install
%make_install

%files
%{_bindir}/*
%{_libdir}/*
%{_datadir}/%{name}/
%{_libexecdir}/%{name}/
%{_includedir}/%{name}/