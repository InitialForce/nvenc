/*
 * D3D11 input for the Linux NVIDIA encoder, see nvencodeapi_d3d11.c.
 *
 * Each function returns FALSE when the call is not a D3D11 one, and the caller then passes
 * it to the driver unchanged. When it returns TRUE, *status holds the result.
 */
#ifndef __WINE_NVENCODEAPI_D3D11_H
#define __WINE_NVENCODEAPI_D3D11_H

const LINUX_NV_ENCODE_API_FUNCTION_LIST *linux_functions(void);

BOOL d3d11_open_session(NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS *params, void **encoder, NVENCSTATUS *status);
BOOL d3d11_register_resource(void *encoder, NV_ENC_REGISTER_RESOURCE *params, NVENCSTATUS *status);
BOOL d3d11_unregister_resource(void *encoder, NV_ENC_REGISTERED_PTR registered, NVENCSTATUS *status);
BOOL d3d11_map_input_resource(void *encoder, NV_ENC_MAP_INPUT_RESOURCE *params, NVENCSTATUS *status);
BOOL d3d11_destroy_encoder(void *encoder, NVENCSTATUS *status);
BOOL d3d11_push_context(void *encoder);
void d3d11_pop_context(void);

#endif
