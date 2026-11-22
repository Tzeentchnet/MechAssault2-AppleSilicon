/* Synthetic stand-ins: these tests do not compile or execute guest code. */
#include <stdint.h>
#include <stddef.h>
typedef void (*guest_fn)(void);
guest_fn recomp_lookup(uint32_t address) { (void)address; return NULL; }
guest_fn recomp_lookup_manual(uint32_t address) { (void)address; return NULL; }
void ma2_xact_on_free(uint32_t address, uint32_t size) { (void)address; (void)size; }
