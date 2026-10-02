/*
 * D3D11 input for the Linux NVIDIA encoder
 *
 * The Linux driver only opens CUDA and OpenGL encode sessions, so a Windows application that
 * opens a DirectX session with an ID3D11Device gets NV_ENC_ERR_UNSUPPORTED_DEVICE. Under DXVK
 * a D3D11 texture is a Vulkan image, and Wine exports Vulkan memory to other APIs as an opaque
 * fd. This file turns a DirectX session into a CUDA session on the same GPU:
 *
 *   open session      a CUDA primary context on the GPU behind the D3D11 device
 *   register texture  a shared twin texture with the same size and format, imported into
 *                     CUDA through its memory fd and registered with the driver as a CUDA array
 *   map texture       a GPU copy from the application's texture into its twin, then a wait
 *                     until the copy has finished, so the encoder reads a complete frame
 *
 * The frames stay on the GPU. Only 8-bit four channel formats (BGRA, BGRX, RGBA) are handled.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "config.h"
#include <dlfcn.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define COBJMACROS
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "initguid.h"
#include "d3d11_4.h"
#include "dxgi1_2.h"
#include "wine/debug.h"
#include "wine/list.h"
#include "wine/server.h"

#include "nvencodeapi.h"
#include "nvencodeapi_d3d11.h"

WINE_DEFAULT_DEBUG_CHANNEL(nvencodeapi);

/* NVENC structures, as in the Video Codec SDK 12 and 13 headers. */
enum
{
    NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX   = 0x0,
    NV_ENC_INPUT_RESOURCE_TYPE_CUDAARRAY = 0x2,
};

struct _NV_ENC_REGISTER_RESOURCE
{
    uint32_t              version;
    uint32_t              resourceType;
    uint32_t              width;
    uint32_t              height;
    uint32_t              pitch;
    uint32_t              subResourceIndex;
    void                 *resourceToRegister;
    NV_ENC_REGISTERED_PTR registeredResource;
    NV_ENC_BUFFER_FORMAT  bufferFormat;
    uint32_t              bufferUsage;
    void                 *pInputFencePoint;
    uint32_t              chromaOffset[2];
    uint32_t              chromaOffsetIn[2];
    uint32_t              reserved1[244];
    void                 *reserved2[61];
};

struct _NV_ENC_MAP_INPUT_RESOURCE
{
    uint32_t              version;
    uint32_t              subResourceIndex;
    void                 *inputResource;
    NV_ENC_REGISTERED_PTR registeredResource;
    NV_ENC_INPUT_PTR      mappedResource;
    NV_ENC_BUFFER_FORMAT  mappedBufferFmt;
    uint32_t              reserved1[251];
    void                 *reserved2[63];
};

/* The CUDA driver API used here, from cuda.h. */
typedef int CUresult;
typedef int CUdevice;
typedef struct CUctx_st *CUcontext;
typedef struct CUextMemory_st *CUexternalMemory;
typedef struct CUmipmappedArray_st *CUmipmappedArray;
typedef struct CUarray_st *CUarray;
typedef struct { unsigned char bytes[16]; } CUuuid;

#define CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD 1
#define CUDA_EXTERNAL_MEMORY_DEDICATED           0x1
#define CU_AD_FORMAT_UNSIGNED_INT8               0x01
#define CUDA_ARRAY3D_SURFACE_LDST                0x02
#define CUDA_ARRAY3D_COLOR_ATTACHMENT            0x20

typedef struct
{
    int type;
    union
    {
        int fd;
        struct { void *handle; const void *name; } win32;
        const void *nvSciBufObject;
    } handle;
    unsigned long long size;
    unsigned int flags;
    unsigned int reserved[16];
} CUDA_EXTERNAL_MEMORY_HANDLE_DESC;

typedef struct
{
    size_t Width;
    size_t Height;
    size_t Depth;
    int Format;
    unsigned int NumChannels;
    unsigned int Flags;
} CUDA_ARRAY3D_DESCRIPTOR;

typedef struct
{
    unsigned long long offset;
    CUDA_ARRAY3D_DESCRIPTOR arrayDesc;
    unsigned int numLevels;
    unsigned int reserved[16];
} CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC;

static CUresult (*pcuInit)(unsigned int);
static CUresult (*pcuDeviceGetCount)(int *);
static CUresult (*pcuDeviceGet)(CUdevice *, int);
static CUresult (*pcuDeviceGetUuid)(CUuuid *, CUdevice);
static CUresult (*pcuDevicePrimaryCtxRetain)(CUcontext *, CUdevice);
static CUresult (*pcuDevicePrimaryCtxRelease)(CUdevice);
static CUresult (*pcuCtxPushCurrent)(CUcontext);
static CUresult (*pcuCtxPopCurrent)(CUcontext *);
static CUresult (*pcuImportExternalMemory)(CUexternalMemory *, const CUDA_EXTERNAL_MEMORY_HANDLE_DESC *);
static CUresult (*pcuExternalMemoryGetMappedMipmappedArray)(CUmipmappedArray *, CUexternalMemory,
                                                            const CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC *);
static CUresult (*pcuMipmappedArrayGetLevel)(CUarray *, CUmipmappedArray, unsigned int);
static CUresult (*pcuMipmappedArrayDestroy)(CUmipmappedArray);
static CUresult (*pcuDestroyExternalMemory)(CUexternalMemory);

/* The Vulkan API used here, through DXVK's handles and Wine's vulkan-1.dll. */
typedef struct VkInstance_T *VkInstance;
typedef struct VkPhysicalDevice_T *VkPhysicalDevice;
typedef struct VkDevice_T *VkDevice;
typedef UINT64 VkImage;

#define VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO           14
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 1000059001
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES 1000071004
#define VK_IMAGE_USAGE_STORAGE_BIT          0x08
#define VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT 0x10

typedef struct
{
    UINT64 size;
    UINT64 alignment;
    UINT32 memoryTypeBits;
} VkMemoryRequirements;

typedef struct
{
    UINT32 sType;
    const void *pNext;
    UINT32 flags;
    UINT32 imageType;
    UINT32 format;
    UINT32 extent[3];
    UINT32 mipLevels;
    UINT32 arrayLayers;
    UINT32 samples;
    UINT32 tiling;
    UINT32 usage;
    UINT32 sharingMode;
    UINT32 queueFamilyIndexCount;
    const UINT32 *pQueueFamilyIndices;
    UINT32 initialLayout;
} VkImageCreateInfo;

typedef struct
{
    UINT32 sType;
    void *pNext;
    unsigned char deviceUUID[16];
    unsigned char driverUUID[16];
    unsigned char deviceLUID[8];
    UINT32 deviceNodeMask;
    UINT32 deviceLUIDValid;
} VkPhysicalDeviceIDProperties;

typedef struct
{
    UINT32 sType;
    void *pNext;
    unsigned char properties[1024]; /* VkPhysicalDeviceProperties is 824 bytes on x86_64 */
} VkPhysicalDeviceProperties2;

static void (WINAPI *pvkGetImageMemoryRequirements)(VkDevice, VkImage, VkMemoryRequirements *);
static void (WINAPI *pvkGetPhysicalDeviceProperties2)(VkPhysicalDevice, VkPhysicalDeviceProperties2 *);

/* DXVK's interop interfaces, from dxvk/src/dxgi/dxgi_interfaces.h. */
DEFINE_GUID(IID_IDXGIVkInteropSurface, 0x5546cf8c, 0x77e7, 0x4341, 0xb0, 0x5d, 0x8d, 0x4d, 0x50, 0x00, 0xe7, 0x7d);
DEFINE_GUID(IID_IDXGIVkInteropDevice,  0xe2ef5fa5, 0xdc21, 0x4af7, 0x90, 0xc4, 0xf6, 0x7e, 0xf6, 0xa0, 0x93, 0x23);

typedef struct IDXGIVkInteropSurface IDXGIVkInteropSurface;
typedef struct IDXGIVkInteropDevice IDXGIVkInteropDevice;

typedef struct
{
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IDXGIVkInteropSurface *, REFIID, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IDXGIVkInteropSurface *);
    ULONG   (STDMETHODCALLTYPE *Release)(IDXGIVkInteropSurface *);
    HRESULT (STDMETHODCALLTYPE *GetDevice)(IDXGIVkInteropSurface *, IDXGIVkInteropDevice **);
    HRESULT (STDMETHODCALLTYPE *GetVulkanImageInfo)(IDXGIVkInteropSurface *, VkImage *, UINT32 *, VkImageCreateInfo *);
} IDXGIVkInteropSurfaceVtbl;
struct IDXGIVkInteropSurface { const IDXGIVkInteropSurfaceVtbl *lpVtbl; };

typedef struct
{
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IDXGIVkInteropDevice *, REFIID, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IDXGIVkInteropDevice *);
    ULONG   (STDMETHODCALLTYPE *Release)(IDXGIVkInteropDevice *);
    void    (STDMETHODCALLTYPE *GetVulkanHandles)(IDXGIVkInteropDevice *, VkInstance *, VkPhysicalDevice *, VkDevice *);
} IDXGIVkInteropDeviceVtbl;
struct IDXGIVkInteropDevice { const IDXGIVkInteropDeviceVtbl *lpVtbl; };

struct d3d11_session
{
    struct list entry;
    void *encoder;                 /* the Linux encoder, also the handle the application holds */
    ID3D11Device *device;
    ID3D11DeviceContext *context;
    ID3D11Multithread *multithread;
    ID3D11Query *query;
    VkDevice vk_device;
    CUdevice cu_device;
    CUcontext cu_context;
    struct list resources;
};

struct d3d11_resource
{
    struct list entry;
    ID3D11Texture2D *source;
    UINT subresource;
    ID3D11Texture2D *twin;
    CUexternalMemory memory;
    CUmipmappedArray mipmap;
    NV_ENC_REGISTERED_PTR registered; /* the driver's registration of the CUDA array */
};

static struct list sessions = LIST_INIT(sessions);
static CRITICAL_SECTION sessions_cs;
static CRITICAL_SECTION_DEBUG sessions_cs_debug =
{
    0, 0, &sessions_cs,
    { &sessions_cs_debug.ProcessLocksList, &sessions_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": sessions_cs") }
};
static CRITICAL_SECTION sessions_cs = { &sessions_cs_debug, -1, 0, 0, 0, 0 };

static BOOL load_functions(void)
{
    static int loaded; /* 0 not tried, 1 loaded, -1 failed */
    void *cuda;
    HMODULE vulkan;

    if (loaded) return loaded > 0;
    loaded = -1;

    if (!(cuda = dlopen("libcuda.so.1", RTLD_NOW)))
    {
        ERR("Can't load libcuda.so.1, D3D11 input is not available.\n");
        return FALSE;
    }

#define LOAD_CUDA(f, name) if (!(p##f = dlsym(cuda, name))) { ERR("Can't find %s.\n", name); return FALSE; }
    LOAD_CUDA(cuInit, "cuInit");
    LOAD_CUDA(cuDeviceGetCount, "cuDeviceGetCount");
    LOAD_CUDA(cuDeviceGet, "cuDeviceGet");
    LOAD_CUDA(cuDeviceGetUuid, "cuDeviceGetUuid");
    LOAD_CUDA(cuDevicePrimaryCtxRetain, "cuDevicePrimaryCtxRetain");
    LOAD_CUDA(cuDevicePrimaryCtxRelease, "cuDevicePrimaryCtxRelease_v2");
    LOAD_CUDA(cuCtxPushCurrent, "cuCtxPushCurrent_v2");
    LOAD_CUDA(cuCtxPopCurrent, "cuCtxPopCurrent_v2");
    LOAD_CUDA(cuImportExternalMemory, "cuImportExternalMemory");
    LOAD_CUDA(cuExternalMemoryGetMappedMipmappedArray, "cuExternalMemoryGetMappedMipmappedArray");
    LOAD_CUDA(cuMipmappedArrayGetLevel, "cuMipmappedArrayGetLevel");
    LOAD_CUDA(cuMipmappedArrayDestroy, "cuMipmappedArrayDestroy");
    LOAD_CUDA(cuDestroyExternalMemory, "cuDestroyExternalMemory");
#undef LOAD_CUDA

    if (!(vulkan = LoadLibraryA("vulkan-1.dll")))
    {
        ERR("Can't load vulkan-1.dll, D3D11 input is not available.\n");
        return FALSE;
    }
    pvkGetImageMemoryRequirements = (void *)GetProcAddress(vulkan, "vkGetImageMemoryRequirements");
    pvkGetPhysicalDeviceProperties2 = (void *)GetProcAddress(vulkan, "vkGetPhysicalDeviceProperties2");
    if (!pvkGetImageMemoryRequirements || !pvkGetPhysicalDeviceProperties2)
    {
        ERR("Can't find the Vulkan functions, D3D11 input is not available.\n");
        return FALSE;
    }

    if (pcuInit(0))
    {
        ERR("cuInit failed, D3D11 input is not available.\n");
        return FALSE;
    }

    loaded = 1;
    return TRUE;
}

/* The CUDA device whose UUID matches the Vulkan device DXVK runs the D3D11 device on. */
static BOOL find_cuda_device(VkPhysicalDevice physical_device, CUdevice *device)
{
    VkPhysicalDeviceIDProperties id = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
    VkPhysicalDeviceProperties2 properties = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id };
    int count, i;

    pvkGetPhysicalDeviceProperties2(physical_device, &properties);

    if (pcuDeviceGetCount(&count)) return FALSE;
    for (i = 0; i < count; i++)
    {
        CUuuid uuid;
        if (pcuDeviceGet(device, i) || pcuDeviceGetUuid(&uuid, *device)) continue;
        if (!memcmp(uuid.bytes, id.deviceUUID, sizeof(uuid.bytes))) return TRUE;
    }
    return FALSE;
}

static struct d3d11_session *find_session(void *encoder)
{
    struct d3d11_session *session;

    LIST_FOR_EACH_ENTRY(session, &sessions, struct d3d11_session, entry)
        if (session->encoder == encoder) return session;
    return NULL;
}

static struct d3d11_resource *find_resource(struct d3d11_session *session, NV_ENC_REGISTERED_PTR ptr)
{
    struct d3d11_resource *resource;

    LIST_FOR_EACH_ENTRY(resource, &session->resources, struct d3d11_resource, entry)
        if ((NV_ENC_REGISTERED_PTR)resource == ptr) return resource;
    return NULL;
}

/* The host fd of the memory behind a Wine NT shared resource handle. */
static int shared_handle_to_fd(HANDLE shared)
{
    obj_handle_t object = 0;
    NTSTATUS status;
    int fd = -1;

    SERVER_START_REQ(d3dkmt_object_open)
    {
        req->type = D3DKMT_RESOURCE;
        req->global = 0;
        req->handle = wine_server_obj_handle(shared);
        if (!(status = wine_server_call(req))) object = reply->handle;
    }
    SERVER_END_REQ;

    if (status)
    {
        ERR("Can't open the D3DKMT resource behind %p, status %#lx.\n", shared, (long)status);
        return -1;
    }

    status = wine_server_handle_to_fd(wine_server_ptr_handle(object), GENERIC_ALL, &fd, NULL);
    NtClose(wine_server_ptr_handle(object));
    if (status)
    {
        ERR("Can't get the fd of the D3DKMT resource behind %p, status %#lx.\n", shared, (long)status);
        return -1;
    }
    return fd;
}

static void destroy_resource(struct d3d11_session *session, struct d3d11_resource *resource)
{
    CUcontext unused;

    list_remove(&resource->entry);

    pcuCtxPushCurrent(session->cu_context);
    if (resource->registered)
        linux_functions()->nvEncUnregisterResource(session->encoder, resource->registered);
    if (resource->mipmap) pcuMipmappedArrayDestroy(resource->mipmap);
    if (resource->memory) pcuDestroyExternalMemory(resource->memory);
    pcuCtxPopCurrent(&unused);

    if (resource->twin) ID3D11Texture2D_Release(resource->twin);
    if (resource->source) ID3D11Texture2D_Release(resource->source);
    free(resource);
}

static void destroy_session(struct d3d11_session *session)
{
    struct d3d11_resource *resource, *next;

    LIST_FOR_EACH_ENTRY_SAFE(resource, next, &session->resources, struct d3d11_resource, entry)
        destroy_resource(session, resource);

    if (session->cu_context) pcuDevicePrimaryCtxRelease(session->cu_device);
    if (session->query) ID3D11Query_Release(session->query);
    if (session->multithread) ID3D11Multithread_Release(session->multithread);
    if (session->context) ID3D11DeviceContext_Release(session->context);
    if (session->device) ID3D11Device_Release(session->device);
    free(session);
}

/* A CUDA session, unlike a DirectX one, needs its CUDA context current on the calling thread
 * for every encoder call, as FFmpeg's CUDA path does with cuCtxPushCurrent. */
BOOL d3d11_push_context(void *encoder)
{
    struct d3d11_session *session;
    CUcontext context = NULL;

    if (list_empty(&sessions)) return FALSE;
    EnterCriticalSection(&sessions_cs);
    if ((session = find_session(encoder))) context = session->cu_context;
    LeaveCriticalSection(&sessions_cs);
    return context && !pcuCtxPushCurrent(context);
}

void d3d11_pop_context(void)
{
    CUcontext unused;
    pcuCtxPopCurrent(&unused);
}

BOOL d3d11_open_session(NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS *params, void **encoder, NVENCSTATUS *status)
{
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS linux_params;
    D3D11_QUERY_DESC query_desc = { D3D11_QUERY_EVENT, 0 };
    IDXGIVkInteropDevice *interop = NULL;
    struct d3d11_session *session;
    CUcontext unused;
    VkPhysicalDevice physical_device;
    VkInstance instance;
    ID3D11Device *device;

    if (!params || params->deviceType != NV_ENC_DEVICE_TYPE_DIRECTX || !params->device) return FALSE;
    if (FAILED(IUnknown_QueryInterface((IUnknown *)params->device, &IID_ID3D11Device, (void **)&device)))
        return FALSE; /* a D3D9 or D3D10 device goes to the driver unchanged */

    *status = NV_ENC_ERR_UNSUPPORTED_DEVICE;
    if (!load_functions() || !(session = calloc(1, sizeof(*session))))
    {
        ID3D11Device_Release(device);
        return TRUE;
    }
    list_init(&session->resources);
    session->device = device;
    ID3D11Device_GetImmediateContext(device, &session->context);
    if (FAILED(ID3D11DeviceContext_QueryInterface(session->context, &IID_ID3D11Multithread, (void **)&session->multithread)))
        session->multithread = NULL;

    if (FAILED(ID3D11Device_QueryInterface(device, &IID_IDXGIVkInteropDevice, (void **)&interop)))
    {
        ERR("The D3D11 device is not a DXVK device, D3D11 input is not available.\n");
        goto failed;
    }
    interop->lpVtbl->GetVulkanHandles(interop, &instance, &physical_device, &session->vk_device);
    interop->lpVtbl->Release(interop);

    if (!find_cuda_device(physical_device, &session->cu_device))
    {
        ERR("No CUDA device matches the GPU of the D3D11 device.\n");
        goto failed;
    }
    if (pcuDevicePrimaryCtxRetain(&session->cu_context, session->cu_device))
    {
        ERR("Can't retain the CUDA primary context.\n");
        session->cu_context = NULL;
        goto failed;
    }
    if (FAILED(ID3D11Device_CreateQuery(device, &query_desc, &session->query)))
    {
        ERR("Can't create a D3D11 event query.\n");
        goto failed;
    }

    linux_params = *params;
    linux_params.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
    linux_params.device = session->cu_context;
    pcuCtxPushCurrent(session->cu_context);
    *status = linux_functions()->nvEncOpenEncodeSessionEx(&linux_params, encoder);
    pcuCtxPopCurrent(&unused);
    if (*status)
    {
        ERR("The driver refused the CUDA session, status %d.\n", *status);
        goto failed;
    }

    session->encoder = *encoder;
    EnterCriticalSection(&sessions_cs);
    list_add_tail(&sessions, &session->entry);
    LeaveCriticalSection(&sessions_cs);
    TRACE("D3D11 device %p encodes through CUDA device %d, encoder %p.\n", device, session->cu_device, *encoder);
    return TRUE;

failed:
    destroy_session(session);
    return TRUE;
}

static BOOL is_supported_format(DXGI_FORMAT format)
{
    return format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8X8_UNORM
        || format == DXGI_FORMAT_R8G8B8A8_UNORM;
}

static NVENCSTATUS create_resource(struct d3d11_session *session, NV_ENC_REGISTER_RESOURCE *params,
                                   ID3D11Texture2D *source, struct d3d11_resource **out)
{
    CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC array_desc = { 0 };
    CUDA_EXTERNAL_MEMORY_HANDLE_DESC memory_desc = { 0 };
    VkImageCreateInfo image_info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    NV_ENC_REGISTER_RESOURCE linux_params;
    IDXGIVkInteropSurface *surface = NULL;
    IDXGIResource1 *dxgi_resource = NULL;
    struct d3d11_resource *resource;
    D3D11_TEXTURE2D_DESC desc;
    VkMemoryRequirements requirements;
    NVENCSTATUS status = NV_ENC_ERR_UNSUPPORTED_PARAM;
    CUcontext unused;
    HANDLE shared = NULL;
    CUarray array;
    VkImage image;
    UINT32 layout;
    int fd;

    ID3D11Texture2D_GetDesc(source, &desc);
    if (!is_supported_format(desc.Format))
    {
        FIXME("DXGI format %u is not supported for D3D11 input.\n", desc.Format);
        return NV_ENC_ERR_UNSUPPORTED_PARAM;
    }

    if (!(resource = calloc(1, sizeof(*resource)))) return NV_ENC_ERR_UNSUPPORTED_PARAM;
    list_add_tail(&session->resources, &resource->entry);
    resource->source = source;
    ID3D11Texture2D_AddRef(source);
    resource->subresource = params->subResourceIndex;

    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    if (FAILED(ID3D11Device_CreateTexture2D(session->device, &desc, NULL, &resource->twin)))
    {
        ERR("Can't create a shared %ux%u texture.\n", desc.Width, desc.Height);
        goto failed;
    }

    if (FAILED(ID3D11Texture2D_QueryInterface(resource->twin, &IID_IDXGIVkInteropSurface, (void **)&surface))
        || FAILED(surface->lpVtbl->GetVulkanImageInfo(surface, &image, &layout, &image_info)))
    {
        ERR("Can't get the Vulkan image of the shared texture.\n");
        goto failed;
    }
    pvkGetImageMemoryRequirements(session->vk_device, image, &requirements);

    if (FAILED(ID3D11Texture2D_QueryInterface(resource->twin, &IID_IDXGIResource1, (void **)&dxgi_resource))
        || FAILED(IDXGIResource1_CreateSharedHandle(dxgi_resource, NULL, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                                     NULL, &shared)))
    {
        ERR("Can't create a shared handle for the texture.\n");
        goto failed;
    }
    fd = shared_handle_to_fd(shared);
    CloseHandle(shared);
    if (fd < 0) goto failed;

    memory_desc.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD;
    memory_desc.handle.fd = fd;
    memory_desc.size = requirements.size;
    memory_desc.flags = CUDA_EXTERNAL_MEMORY_DEDICATED;

    array_desc.arrayDesc.Width = desc.Width;
    array_desc.arrayDesc.Height = desc.Height;
    array_desc.arrayDesc.Format = CU_AD_FORMAT_UNSIGNED_INT8;
    array_desc.arrayDesc.NumChannels = 4;
    if (image_info.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) array_desc.arrayDesc.Flags |= CUDA_ARRAY3D_COLOR_ATTACHMENT;
    if (image_info.usage & VK_IMAGE_USAGE_STORAGE_BIT) array_desc.arrayDesc.Flags |= CUDA_ARRAY3D_SURFACE_LDST;
    array_desc.numLevels = 1;

    pcuCtxPushCurrent(session->cu_context);
    if (pcuImportExternalMemory(&resource->memory, &memory_desc))
    {
        ERR("CUDA can't import the memory of the shared texture (%s bytes).\n", wine_dbgstr_longlong(requirements.size));
        close(fd);
        resource->memory = NULL;
    }
    else if (pcuExternalMemoryGetMappedMipmappedArray(&resource->mipmap, resource->memory, &array_desc)
             || pcuMipmappedArrayGetLevel(&array, resource->mipmap, 0))
    {
        ERR("CUDA can't map the shared texture as an array (usage %#x).\n", image_info.usage);
    }
    if (!resource->mipmap)
    {
        pcuCtxPopCurrent(&unused);
        goto failed;
    }

    linux_params = *params;
    linux_params.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_CUDAARRAY;
    linux_params.resourceToRegister = array;
    linux_params.width = desc.Width;
    linux_params.height = desc.Height;
    linux_params.pitch = desc.Width * 4;
    linux_params.subResourceIndex = 0;
    status = linux_functions()->nvEncRegisterResource(session->encoder, &linux_params);
    pcuCtxPopCurrent(&unused);
    if (status)
    {
        ERR("The driver refused the CUDA array, status %d.\n", status);
        goto failed;
    }
    resource->registered = linux_params.registeredResource;

    TRACE("Texture %p/%u registered through CUDA array %p (%ux%u, layout %u, usage %#x).\n",
          source, resource->subresource, array, desc.Width, desc.Height, layout, image_info.usage);
    if (surface) surface->lpVtbl->Release(surface);
    if (dxgi_resource) IDXGIResource1_Release(dxgi_resource);
    *out = resource;
    return NV_ENC_SUCCESS;

failed:
    if (surface) surface->lpVtbl->Release(surface);
    if (dxgi_resource) IDXGIResource1_Release(dxgi_resource);
    destroy_resource(session, resource);
    return status ? status : NV_ENC_ERR_UNSUPPORTED_PARAM;
}

BOOL d3d11_register_resource(void *encoder, NV_ENC_REGISTER_RESOURCE *params, NVENCSTATUS *status)
{
    struct d3d11_resource *resource = NULL;
    struct d3d11_session *session;
    ID3D11Texture2D *source;

    if (!params || params->resourceType != NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX) return FALSE;

    EnterCriticalSection(&sessions_cs);
    if (!(session = find_session(encoder)))
    {
        LeaveCriticalSection(&sessions_cs);
        return FALSE;
    }

    if (FAILED(IUnknown_QueryInterface((IUnknown *)params->resourceToRegister, &IID_ID3D11Texture2D, (void **)&source)))
    {
        FIXME("Only D3D11 2D textures can be registered.\n");
        *status = NV_ENC_ERR_UNSUPPORTED_PARAM;
    }
    else
    {
        if (!(*status = create_resource(session, params, source, &resource)))
            params->registeredResource = resource;
        ID3D11Texture2D_Release(source);
    }
    LeaveCriticalSection(&sessions_cs);
    return TRUE;
}

BOOL d3d11_unregister_resource(void *encoder, NV_ENC_REGISTERED_PTR registered, NVENCSTATUS *status)
{
    struct d3d11_resource *resource;
    struct d3d11_session *session;

    EnterCriticalSection(&sessions_cs);
    if (!(session = find_session(encoder)) || !(resource = find_resource(session, registered)))
    {
        LeaveCriticalSection(&sessions_cs);
        return FALSE;
    }
    destroy_resource(session, resource);
    LeaveCriticalSection(&sessions_cs);
    *status = NV_ENC_SUCCESS;
    return TRUE;
}

BOOL d3d11_map_input_resource(void *encoder, NV_ENC_MAP_INPUT_RESOURCE *params, NVENCSTATUS *status)
{
    NV_ENC_MAP_INPUT_RESOURCE linux_params;
    struct d3d11_resource *resource;
    struct d3d11_session *session;
    CUcontext unused;
    HRESULT hr;

    if (!params) return FALSE;

    EnterCriticalSection(&sessions_cs);
    if (!(session = find_session(encoder)) || !(resource = find_resource(session, params->registeredResource)))
    {
        LeaveCriticalSection(&sessions_cs);
        return FALSE;
    }

    /* The application's texture is still in use by D3D11 when it asks for the map, so the
     * frame is copied on the GPU and the encoder only reads it once the copy has finished. */
    ID3D11DeviceContext_CopySubresourceRegion(session->context, (ID3D11Resource *)resource->twin, 0, 0, 0, 0,
                                              (ID3D11Resource *)resource->source, resource->subresource, NULL);
    ID3D11DeviceContext_End(session->context, (ID3D11Asynchronous *)session->query);
    ID3D11DeviceContext_Flush(session->context);
    while ((hr = ID3D11DeviceContext_GetData(session->context, (ID3D11Asynchronous *)session->query, NULL, 0,
                                             D3D11_ASYNC_GETDATA_DONOTFLUSH)) == S_FALSE)
        SwitchToThread();
    if (FAILED(hr)) ERR("Waiting for the texture copy failed, hr %#lx.\n", (long)hr);

    linux_params = *params;
    linux_params.registeredResource = resource->registered;
    pcuCtxPushCurrent(session->cu_context);
    *status = linux_functions()->nvEncMapInputResource(encoder, &linux_params);
    pcuCtxPopCurrent(&unused);
    params->mappedResource = linux_params.mappedResource;
    params->mappedBufferFmt = linux_params.mappedBufferFmt;
    LeaveCriticalSection(&sessions_cs);
    return TRUE;
}

BOOL d3d11_destroy_encoder(void *encoder, NVENCSTATUS *status)
{
    struct d3d11_resource *resource, *next;
    struct d3d11_session *session;
    CUcontext unused;

    EnterCriticalSection(&sessions_cs);
    if (!(session = find_session(encoder)))
    {
        LeaveCriticalSection(&sessions_cs);
        return FALSE;
    }
    list_remove(&session->entry);
    LeaveCriticalSection(&sessions_cs);

    /* The registrations go before the encoder they belong to, and the CUDA context after it. */
    LIST_FOR_EACH_ENTRY_SAFE(resource, next, &session->resources, struct d3d11_resource, entry)
        destroy_resource(session, resource);
    pcuCtxPushCurrent(session->cu_context);
    *status = linux_functions()->nvEncDestroyEncoder(encoder);
    pcuCtxPopCurrent(&unused);
    destroy_session(session);
    return TRUE;
}
