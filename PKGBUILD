# Maintainer: Alban MARTEL <alban.f.j.martel@laposte.net>
pkgname=gneomutt
pkgver=0.1
pkgrel=1
pkgdesc="Une interface GTK pour NeoMutt"
arch=('x86_64')
url="https://github.com/albanmartel/gneomutt" # À adapter
license=('GPL')
depends=('gtk3' 'vte3' 'neomutt')
makedepends=('clang' 'pkg-config' 'glib2')
source=("local") # Comme c'est un projet local

build() {
  cd "$srcdir/.."
  # On appelle ton Makefile pour compiler
  make
}

package() {
  cd "$srcdir/.."
  # On utilise le Makefile pour l'installation dans le répertoire du paquet
  # DESTDIR est crucial ici pour que pacman sache où isoler les fichiers
  make DESTDIR="$pkgdir" install
}
