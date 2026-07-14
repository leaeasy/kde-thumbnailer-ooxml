pkgname=kdegraphics-thumbnailer-ooxml
_pkgname=kde-thumbnailer-ooxml
pkgver=0.1.0
pkgrel=1
pkgdesc="KDE thumbnail plugin for Microsoft Office Open XML documents"
arch=('x86_64')
url="https://github.com/leaeasy/kde-thumbnailer-ooxml"
license=('GPL-2.0-or-later' 'LGPL-2.0-only')
depends=('karchive' 'kcoreaddons' 'kio' 'qt6-base')
makedepends=('cmake' 'extra-cmake-modules' 'ninja')
source=("${_pkgname}-${pkgver}.tar.gz::${url}/archive/refs/tags/${pkgver}.tar.gz")
sha256sums=('563f87ee11562dcfae22bb81f61419a2393204ae4befcf96bf2211c7b72ebb3a')

build() {
  cmake -S "${_pkgname}-${pkgver}" -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=None \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DBUILD_TESTING=OFF
  cmake --build build
}

package() {
  DESTDIR="$pkgdir" cmake --install build
}
