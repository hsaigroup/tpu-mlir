#include "backend_helper.h"
#include "common_def.h"
#include "api_common.h"
// 1. include head file of api function
#include "api_swapchannel.h"

// 2. global backend api functions
// e.g., IMPL_CUSTOM_API_GLB(op_name, param_t)
IMPL_CUSTOM_API_GLB(swapchannel, swapchannel_param_t)

// 3. local backend api functions
// e.g., IMPL_CUSTOM_API_LOC(op_name, param_t)
