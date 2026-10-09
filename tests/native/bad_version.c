/* a module built against an ABI version the host does not implement */
#include "flint.h"
int flint_module_init(FlModule *module, uint32_t abi_version);
int flint_module_init(FlModule *module, uint32_t abi_version)
{
	(void)module;
	/* deliberately wrong: the host must refuse before this returns */
	return abi_version == 999999u ? FL_INIT_OK : FL_INIT_ERROR;
}
