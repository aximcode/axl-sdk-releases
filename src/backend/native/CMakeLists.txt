# src/backend/native -- its sources of libaxl.a, in the archive's order.
target_sources(axl PRIVATE
    axl-backend-native.c
    axl-backend-native-efi1x.c
    axl-backend-native-nosh.c
    axl-backend-native-shell.c
    axl-backend-native-lines.c
    axl-backend-native-event.c
    axl-backend-native-mp.c
    axl-backend-native-timer.c)
