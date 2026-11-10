# /etc/profile.d/axl-sdk.sh -- from the axl-sdk-<version> .deb/.rpm
# (AXL-Distribution-Design.md §17.5). An alternatives slave of
# /opt/axl-sdk-package, so the newest installed package owns it.
#
# APPENDS the packages' bin directory: a login shell reads this before
# ~/.profile, which puts ~/.local/bin FIRST, so an SDK a user installed with
# install.sh always wins over the system package. Nothing is put in /usr/bin.
case ":${PATH}:" in
    *:/opt/axl-sdk-package/bin:*) ;;
    *) PATH="${PATH:+${PATH}:}/opt/axl-sdk-package/bin"; export PATH ;;
esac
