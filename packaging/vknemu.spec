Name:           vknemu
Version:        __VERSION__
Release:        1%{?dist}
Summary:        input based idle frame limiter vulkan layer

License:        GPL-3.0-or-later
URL:            https://github.com/reakjra/vkNemu
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  meson
BuildRequires:  ninja-build
BuildRequires:  gcc
BuildRequires:  vulkan-loader-devel
BuildRequires:  libX11-devel
BuildRequires:  libxcb-devel
BuildRequires:  wayland-devel
BuildRequires:  wayland-protocols-devel
BuildRequires:  pkgconf-pkg-config

%description
input based idle frame limiter vulkan layer

%prep
%autosetup -n vknemu-%{version}

%build
%meson -Drelocatable_layer=true
%meson_build

%install
%meson_install

%files
%license LICENSE
%{_libdir}/libVkLayer_vknemu.so
%{_datadir}/vulkan/implicit_layer.d/vknemu.json

%changelog
* Wed Oct 07 2026 vkNemu - 0.1.0-1
- initial spec
