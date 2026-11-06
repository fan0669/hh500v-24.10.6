include $(TOPDIR)/rules.mk

LUCI_TITLE:=LuCI panel for ZeroTier (status / join / leave)
LUCI_DEPENDS:=+zerotier +luci-base
LUCI_PKGARCH:=all

PKG_NAME:=luci-app-zerotier-q
PKG_VERSION:=1.0.0
PKG_RELEASE:=1

PKG_MAINTAINER:=local
PKG_LICENSE:=Apache-2.0

include $(TOPDIR)/feeds/luci/luci.mk

# call BuildPackage - OpenWrt buildroot signature
