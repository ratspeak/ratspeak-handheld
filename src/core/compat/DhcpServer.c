#if !defined(RSCROWPANEL)
// Compile the shared SDK backport before liblwip.a is searched by the linker.
#include "../../../vendor/esp_idf_compat/dhcpserver.c"

#endif
