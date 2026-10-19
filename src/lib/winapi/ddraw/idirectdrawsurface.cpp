#include <winapi/ddraw/idirectdrawsurface.h>
#include <winapi/ddraw/idirectdraw2.h>
#include <winapi/wrapper.h>
#include <lib/surface.h>
#include <lib/window.h>
#include <lib/thread.h>
#include <platform/perf.h>


namespace win32 { namespace ddraw
{

x86::reg32 IDirectDrawSurface::s_iDirectDrawSurfaceVtable = 0;

DEFINE_GUID( IID_IDirectDrawSurface,	0x6C14DB81,0xA733,0x11CE,0xA5,0x21,0x00,0x20,0xAF,0x0B,0xE5,0x60 );
DEFINE_GUID( IID_IDirectDrawSurface2,	0x57805885,0x6eec,0x11cf,0x94,0x41,0xa8,0x23,0x03,0xc1,0x0e,0x27 );
DEFINE_GUID( IID_IDirectDrawSurface3,	0xDA044E00,0x69B2,0x11D0,0xA1,0xD5,0x00,0xAA,0x00,0xB8,0xDF,0xBB );

IDirectDrawSurface::IDirectDrawSurface(WinApplication* app, DDSURFACEDESC* desc, Renderer* renderer, x86::reg32 surfaceIndex)
    :   IUnknown(IID_IDirectDrawSurface3)
{
    RC_USE(desc);
    m_vtable = s_iDirectDrawSurfaceVtable;
    m_resource = new Surface(renderer, surfaceIndex);
    app->allocateResource(m_resource);
}

BOOL IDirectDrawSurface::QueryInterface(WinApplication* app, x86::CPU& cpu,
                                        REFGUID refiid, x86::reg32* pvvObject)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(refiid);
    if (*refiid == IID_IDirectDrawSurface || *refiid == IID_IDirectDrawSurface2 || *refiid == IID_IDirectDrawSurface3
        || com::isUnknown(*refiid))
    {
        *pvvObject = ComGetIndex(app, this);
        m_refCount ++;
        return 0;
    }
    else
    {
        *pvvObject = 0;
        return 0x80004002;
    }
}

const void* IDirectDrawSurface::getPointer(WinApplication* winapp) const
{
    return &winapp->getMemory<const void>(dynamic_cast<Surface*>(m_resource)->lock());
}

HRESULT IDirectDrawSurface::AddAttachedSurface(WinApplication* app, x86::CPU& cpu,
                                               LPDIRECTDRAWSURFACE2 lpDDSAttachedSurface)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(lpDDSAttachedSurface);
    RC_ASSERT(false);
    return 1;
}

HRESULT IDirectDrawSurface::AddOverlayDirtyRect(WinApplication* app, x86::CPU& cpu,
                                                LPRECT lpRect)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(lpRect);
    RC_ASSERT(false);
    return 1;
}

HRESULT IDirectDrawSurface::Blt(WinApplication* app, x86::CPU& cpu,
                                LPRECT lpDestRect, LPDIRECTDRAWSURFACE2 lpDDSrcSurface, LPRECT lpSrcRect, DWORD dwFlags, LPDDBLTFX lpDDBltFx)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(dwFlags);
    RC_USE(lpDDBltFx);
    RC_ASSERT(lpDestRect->top == 0);
    RC_ASSERT(lpDestRect->bottom == 480);
    RC_ASSERT(lpDestRect->left == 0);
    RC_ASSERT(lpDestRect->right == 640);
    RC_ASSERT(lpSrcRect->top == 0);
    RC_ASSERT(lpSrcRect->bottom == 480);
    RC_ASSERT(lpSrcRect->left == 0);
    RC_ASSERT(lpSrcRect->right == 640);
    DDSURFACEDESC dstDesc;
    dstDesc.dwSize = sizeof(dstDesc);
    const void* srcData = lpDDSrcSurface->getPointer(app);
    this->Lock(app, cpu, 0, &dstDesc, 0, 0);
    memcpy(&app->getMemory<void>(dstDesc.lpSurface), srcData, dstDesc.dwWidth*dstDesc.dwHeight*dstDesc.ddpfPixelFormat.dwRGBBitCount / 8);
    lpDDSrcSurface->Unlock(app, cpu, nullptr);
    this->Unlock(app, cpu, nullptr);
    return 1;
}

HRESULT IDirectDrawSurface::BltBatch(WinApplication* app, x86::CPU& cpu,
                                     LPDDBLTBATCH lpDDBltBatch, DWORD dwCount, DWORD dwFlags)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(lpDDBltBatch);
    RC_USE(dwCount);
    RC_USE(dwFlags);
    RC_ASSERT(false);
    return 1;
}

HRESULT IDirectDrawSurface::BltFast(WinApplication* app, x86::CPU& cpu,
                                    DWORD dwX, DWORD dwY, LPDIRECTDRAWSURFACE2 lpDDSrcSurface, LPRECT lpSrcRect, DWORD dwTrans)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(dwX);
    RC_USE(dwY);
    RC_USE(lpDDSrcSurface);
    RC_USE(lpSrcRect);
    RC_USE(dwTrans);
    RC_ASSERT(false);
    return 1;
}

HRESULT IDirectDrawSurface::DeleteAttachedSurface(WinApplication* app, x86::CPU& cpu,
                                                  DWORD dwFlags, LPDIRECTDRAWSURFACE2 lpDDSAttachedSurface)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(dwFlags);
    RC_USE(lpDDSAttachedSurface);
    RC_ASSERT(false);
    return 1;
}

HRESULT IDirectDrawSurface::EnumAttachedSurfaces(WinApplication* app, x86::CPU& cpu,
                                                 LPVOID lpContext, LPDDENUMSURFACESCALLBACK lpEnumSurfacesCallback)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(lpContext);
    RC_USE(lpEnumSurfacesCallback);
    RC_ASSERT(false);
    return 1;
}

HRESULT IDirectDrawSurface::EnumOverlayZOrders(WinApplication* app, x86::CPU& cpu,
                                               DWORD dwFlags, LPVOID lpContext, LPDDENUMSURFACESCALLBACK lpfnCallback)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(dwFlags);
    RC_USE(lpContext);
    RC_USE(lpfnCallback);
    RC_ASSERT(false);
    return 1;
}

HRESULT IDirectDrawSurface::Flip(WinApplication* app, x86::CPU& cpu,
                                 LPDIRECTDRAWSURFACE2 lpDDSurfaceTargetOverride, DWORD dwFlags)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(dwFlags);
    RC_ASSERT(lpDDSurfaceTargetOverride == 0);
    pf::perf::blocking(pf::perf::BlockSwap);
    app->unlockContext(cpu);
    dynamic_cast<Surface*>(m_resource)->getRenderer()->swap();
    app->lockContext(cpu);
    return 0;
}

HRESULT IDirectDrawSurface::GetAttachedSurface(WinApplication* app, x86::CPU& cpu,
                                               LPDDSCAPS lpDDSCaps, Packed<LPDIRECTDRAWSURFACE2> *lplpDDAttachedSurface)
{
    RC_USE(cpu);
    RC_USE(lpDDSCaps);
    *lplpDDAttachedSurface = Packed<LPDIRECTDRAWSURFACE2>(com::ComAlloc(app));
    IDirectDrawSurface* result = &app->getMemory<IDirectDrawSurface>(*lplpDDAttachedSurface);
    new (result) IDirectDrawSurface(app, &app->getMemory<DDSURFACEDESC>(IDirectDraw2::g_currentDisplayMode),
                                    dynamic_cast<Surface*>(m_resource)->getRenderer(), 1);
    return 0;
}

HRESULT IDirectDrawSurface::GetBltStatus(WinApplication* app, x86::CPU& cpu,
                                         DWORD dwFlags)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(dwFlags);
    RC_ASSERT(false);
    return 1;
}

HRESULT IDirectDrawSurface::GetCaps(WinApplication* app, x86::CPU& cpu,
                                    LPDDSCAPS lpDDSCaps)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(lpDDSCaps);
    RC_ASSERT(false);
    return 1;
}

HRESULT IDirectDrawSurface::GetClipper(WinApplication* app, x86::CPU& cpu,
                                       Packed<LPDIRECTDRAWCLIPPER> lplpDDClipper)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(lplpDDClipper);
    RC_ASSERT(false);
    return 1;
}

HRESULT IDirectDrawSurface::GetColorKey(WinApplication* app, x86::CPU& cpu,
                                        DWORD dwFlags, LPDDCOLORKEY lpDDColorKey)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(dwFlags);
    RC_USE(lpDDColorKey);
    RC_ASSERT(false);
    return 1;
}

HRESULT IDirectDrawSurface::GetDC(WinApplication* app, x86::CPU& cpu,
                                  HDC *lphDC)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(lphDC);
    RC_ASSERT(false);
    return 1;
}

HRESULT IDirectDrawSurface::GetFlipStatus(WinApplication* app, x86::CPU& cpu,
                                          DWORD dwFlags)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(dwFlags);
    RC_ASSERT(false);
    return 1;
}

HRESULT IDirectDrawSurface::GetOverlayPosition(WinApplication* app, x86::CPU& cpu,
                                               LPLONG lplX, LPLONG lplY)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(lplX);
    RC_USE(lplY);
    RC_ASSERT(false);
    return 1;
}

HRESULT IDirectDrawSurface::GetPalette(WinApplication* app, x86::CPU& cpu,
                                       Packed<LPDIRECTDRAWPALETTE> lplpDDPalette)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(lplpDDPalette);
    RC_ASSERT(false);
    return 1;
}

HRESULT IDirectDrawSurface::GetPixelFormat(WinApplication* app, x86::CPU& cpu,
                                           LPDDPIXELFORMAT lpDDPixelFormat)
{
    RC_USE(cpu);
    DDSURFACEDESC* currentDisplayMode = &app->getMemory<DDSURFACEDESC>(IDirectDraw2::g_currentDisplayMode);
    memcpy(lpDDPixelFormat, &currentDisplayMode->ddpfPixelFormat, sizeof(DDPIXELFORMAT));
    return 0;
}

HRESULT IDirectDrawSurface::GetSurfaceDesc(WinApplication* app, x86::CPU& cpu,
                                           LPDDSURFACEDESC lpDDSurfaceDesc)
{
    RC_USE(cpu);
    DDSURFACEDESC* currentDisplayMode = &app->getMemory<DDSURFACEDESC>(IDirectDraw2::g_currentDisplayMode);
    memcpy(lpDDSurfaceDesc, currentDisplayMode, currentDisplayMode->dwSize);
    return 0;
}

HRESULT IDirectDrawSurface::Initialize(WinApplication* app, x86::CPU& cpu,
                                       LPDIRECTDRAW lpDD, LPDDSURFACEDESC lpDDSurfaceDesc)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(lpDD);
    RC_USE(lpDDSurfaceDesc);
    RC_ASSERT(false);
    return 1;
}

HRESULT IDirectDrawSurface::IsLost(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return 0;
}

HRESULT IDirectDrawSurface::Lock(WinApplication* app, x86::CPU& cpu,
                                 LPRECT lpDestRect, LPDDSURFACEDESC lpDDSurfaceDesc, DWORD dwFlags, HANDLE hEvent)
{
    RC_USE(cpu);
    RC_USE(dwFlags);
    RC_ASSERT(lpDestRect == 0);
    RC_ASSERT(hEvent == 0);
    DDSURFACEDESC* screenMode = &app->getMemory<DDSURFACEDESC>(IDirectDraw2::g_currentDisplayMode);
    memcpy(lpDDSurfaceDesc, screenMode, lpDDSurfaceDesc->dwSize);
    lpDDSurfaceDesc->lpSurface = Packed<void>(dynamic_cast<Surface*>(m_resource)->lock());
    return 0;
}

HRESULT IDirectDrawSurface::ReleaseDC(WinApplication* app, x86::CPU& cpu,
                                      HDC hDC)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(hDC);
    RC_ASSERT(false);
    return 1;
}

HRESULT IDirectDrawSurface::Restore(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_ASSERT(false);
    return 1;
}

HRESULT IDirectDrawSurface::SetClipper(WinApplication* app, x86::CPU& cpu,
                                       LPDIRECTDRAWCLIPPER lpDDClipper)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(lpDDClipper);
    RC_ASSERT(false);
    return 1;
}

HRESULT IDirectDrawSurface::SetColorKey(WinApplication* app, x86::CPU& cpu,
                                        DWORD dwFlags, LPDDCOLORKEY lpDDColorKey)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(dwFlags);
    RC_USE(lpDDColorKey);
    return 0;
}

HRESULT IDirectDrawSurface::SetOverlayPosition(WinApplication* app, x86::CPU& cpu,
                                               LONG lX, LONG lY)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(lX);
    RC_USE(lY);
    RC_ASSERT(false);
    return 1;
}

HRESULT IDirectDrawSurface::SetPalette(WinApplication* app, x86::CPU& cpu,
                                       LPDIRECTDRAWPALETTE lpDDPalette)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(lpDDPalette);
    return 0;
}

HRESULT IDirectDrawSurface::Unlock(WinApplication* app, x86::CPU& cpu,
                                   LPVOID lpSurfaceData)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(lpSurfaceData);
    app->unlockContext(cpu);
    dynamic_cast<Surface*>(m_resource)->unlock();
    app->lockContext(cpu);
    return 0;
}

HRESULT IDirectDrawSurface::UpdateOverlay(WinApplication* app, x86::CPU& cpu,
                                          LPRECT lpSrcRect, LPDIRECTDRAWSURFACE2 lpDDDestSurface, LPRECT lpDestRect, DWORD dwFlags, LPDDOVERLAYFX lpDDOverlayFx)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(lpSrcRect);
    RC_USE(lpDDDestSurface);
    RC_USE(lpDestRect);
    RC_USE(dwFlags);
    RC_USE(lpDDOverlayFx);
    RC_ASSERT(false);
    return 1;
}

HRESULT IDirectDrawSurface::UpdateOverlayDisplay(WinApplication* app, x86::CPU& cpu,
                                                 DWORD dwFlags)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(dwFlags);
    RC_ASSERT(false);
    return 1;
}

HRESULT IDirectDrawSurface::UpdateOverlayZOrder(WinApplication* app, x86::CPU& cpu,
                                                DWORD dwFlags, LPDIRECTDRAWSURFACE2 lpDDSReference)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(dwFlags);
    RC_USE(lpDDSReference);
    RC_ASSERT(false);
    return 1;
}

HRESULT IDirectDrawSurface::GetDDInterface(WinApplication* app, x86::CPU& cpu,
                                           Packed<LPVOID> lplpDD)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(lplpDD);
    RC_ASSERT(false);
    return 1;
}

HRESULT IDirectDrawSurface::PageLock(WinApplication* app, x86::CPU& cpu,
                                     DWORD dwFlags)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(dwFlags);
    RC_ASSERT(false);
    return 1;
}

HRESULT IDirectDrawSurface::PageUnlock(WinApplication* app, x86::CPU& cpu,
                                       DWORD dwFlags)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(dwFlags);
    RC_ASSERT(false);
    return 1;
}

x86::reg32 IDirectDrawSurface::init(WinApplication *app, x86::reg32 memblockIndex, x86::reg8* memBlock)
{
    x86::reg32* vtable = reinterpret_cast<x86::reg32*>(memBlock);
    app->registerMethod(0x40001d, {"IDirectDrawSurface::QueryInterface", &Wrapper<decltype(&IDirectDrawSurface::QueryInterface), &IDirectDrawSurface::QueryInterface>::stdcall});
    app->registerMethod(0x40001e, {"IDirectDrawSurface::AddAttachedSurface", &Wrapper<decltype(&IDirectDrawSurface::AddAttachedSurface), &IDirectDrawSurface::AddAttachedSurface>::stdcall});
    app->registerMethod(0x40001f, {"IDirectDrawSurface::AddOverlayDirtyRect", &Wrapper<decltype(&IDirectDrawSurface::AddOverlayDirtyRect), &IDirectDrawSurface::AddOverlayDirtyRect>::stdcall});
    app->registerMethod(0x400020, {"IDirectDrawSurface::Blt", &Wrapper<decltype(&IDirectDrawSurface::Blt), &IDirectDrawSurface::Blt>::stdcall});
    app->registerMethod(0x400021, {"IDirectDrawSurface::BltBatch", &Wrapper<decltype(&IDirectDrawSurface::BltBatch), &IDirectDrawSurface::BltBatch>::stdcall});
    app->registerMethod(0x400022, {"IDirectDrawSurface::BltFast", &Wrapper<decltype(&IDirectDrawSurface::BltFast), &IDirectDrawSurface::BltFast>::stdcall});
    app->registerMethod(0x400023, {"IDirectDrawSurface::DeleteAttachedSurface", &Wrapper<decltype(&IDirectDrawSurface::DeleteAttachedSurface), &IDirectDrawSurface::DeleteAttachedSurface>::stdcall});
    app->registerMethod(0x400024, {"IDirectDrawSurface::EnumAttachedSurfaces", &Wrapper<decltype(&IDirectDrawSurface::EnumAttachedSurfaces), &IDirectDrawSurface::EnumAttachedSurfaces>::stdcall});
    app->registerMethod(0x400025, {"IDirectDrawSurface::EnumOverlayZOrders", &Wrapper<decltype(&IDirectDrawSurface::EnumOverlayZOrders), &IDirectDrawSurface::EnumOverlayZOrders>::stdcall});
    app->registerMethod(0x400026, {"IDirectDrawSurface::Flip", &Wrapper<decltype(&IDirectDrawSurface::Flip), &IDirectDrawSurface::Flip>::stdcall});
    app->registerMethod(0x400027, {"IDirectDrawSurface::GetAttachedSurface", &Wrapper<decltype(&IDirectDrawSurface::GetAttachedSurface), &IDirectDrawSurface::GetAttachedSurface>::stdcall});
    app->registerMethod(0x400028, {"IDirectDrawSurface::GetBltStatus", &Wrapper<decltype(&IDirectDrawSurface::GetBltStatus), &IDirectDrawSurface::GetBltStatus>::stdcall});
    app->registerMethod(0x400029, {"IDirectDrawSurface::GetCaps", &Wrapper<decltype(&IDirectDrawSurface::GetCaps), &IDirectDrawSurface::GetCaps>::stdcall});
    app->registerMethod(0x40002a, {"IDirectDrawSurface::GetClipper", &Wrapper<decltype(&IDirectDrawSurface::GetClipper), &IDirectDrawSurface::GetClipper>::stdcall});
    app->registerMethod(0x40002b, {"IDirectDrawSurface::GetColorKey", &Wrapper<decltype(&IDirectDrawSurface::GetColorKey), &IDirectDrawSurface::GetColorKey>::stdcall});
    app->registerMethod(0x40002c, {"IDirectDrawSurface::GetDC", &Wrapper<decltype(&IDirectDrawSurface::GetDC), &IDirectDrawSurface::GetDC>::stdcall});
    app->registerMethod(0x40002d, {"IDirectDrawSurface::GetFlipStatus", &Wrapper<decltype(&IDirectDrawSurface::GetFlipStatus), &IDirectDrawSurface::GetFlipStatus>::stdcall});
    app->registerMethod(0x40002e, {"IDirectDrawSurface::GetOverlayPosition", &Wrapper<decltype(&IDirectDrawSurface::GetOverlayPosition), &IDirectDrawSurface::GetOverlayPosition>::stdcall});
    app->registerMethod(0x40002f, {"IDirectDrawSurface::GetPalette", &Wrapper<decltype(&IDirectDrawSurface::GetPalette), &IDirectDrawSurface::GetPalette>::stdcall});
    app->registerMethod(0x400030, {"IDirectDrawSurface::GetPixelFormat", &Wrapper<decltype(&IDirectDrawSurface::GetPixelFormat), &IDirectDrawSurface::GetPixelFormat>::stdcall});
    app->registerMethod(0x400031, {"IDirectDrawSurface::GetSurfaceDesc", &Wrapper<decltype(&IDirectDrawSurface::GetSurfaceDesc), &IDirectDrawSurface::GetSurfaceDesc>::stdcall});
    app->registerMethod(0x400032, {"IDirectDrawSurface::Initialize", &Wrapper<decltype(&IDirectDrawSurface::Initialize), &IDirectDrawSurface::Initialize>::stdcall});
    app->registerMethod(0x400033, {"IDirectDrawSurface::IsLost", &Wrapper<decltype(&IDirectDrawSurface::IsLost), &IDirectDrawSurface::IsLost>::stdcall});
    app->registerMethod(0x400034, {"IDirectDrawSurface::Lock", &Wrapper<decltype(&IDirectDrawSurface::Lock), &IDirectDrawSurface::Lock>::stdcall});
    app->registerMethod(0x400035, {"IDirectDrawSurface::ReleaseDC", &Wrapper<decltype(&IDirectDrawSurface::ReleaseDC), &IDirectDrawSurface::ReleaseDC>::stdcall});
    app->registerMethod(0x400036, {"IDirectDrawSurface::Restore", &Wrapper<decltype(&IDirectDrawSurface::Restore), &IDirectDrawSurface::Restore>::stdcall});
    app->registerMethod(0x400037, {"IDirectDrawSurface::SetClipper", &Wrapper<decltype(&IDirectDrawSurface::SetClipper), &IDirectDrawSurface::SetClipper>::stdcall});
    app->registerMethod(0x400038, {"IDirectDrawSurface::SetColorKey", &Wrapper<decltype(&IDirectDrawSurface::SetColorKey), &IDirectDrawSurface::SetColorKey>::stdcall});
    app->registerMethod(0x400039, {"IDirectDrawSurface::SetOverlayPosition", &Wrapper<decltype(&IDirectDrawSurface::SetOverlayPosition), &IDirectDrawSurface::SetOverlayPosition>::stdcall});
    app->registerMethod(0x40003a, {"IDirectDrawSurface::SetPalette", &Wrapper<decltype(&IDirectDrawSurface::SetPalette), &IDirectDrawSurface::SetPalette>::stdcall});
    app->registerMethod(0x40003b, {"IDirectDrawSurface::Unlock", &Wrapper<decltype(&IDirectDrawSurface::Unlock), &IDirectDrawSurface::Unlock>::stdcall});
    app->registerMethod(0x40003c, {"IDirectDrawSurface::UpdateOverlay", &Wrapper<decltype(&IDirectDrawSurface::UpdateOverlay), &IDirectDrawSurface::UpdateOverlay>::stdcall});
    app->registerMethod(0x40003d, {"IDirectDrawSurface::UpdateOverlayDisplay", &Wrapper<decltype(&IDirectDrawSurface::UpdateOverlayDisplay), &IDirectDrawSurface::UpdateOverlayDisplay>::stdcall});
    app->registerMethod(0x40003e, {"IDirectDrawSurface::UpdateOverlayZOrder", &Wrapper<decltype(&IDirectDrawSurface::UpdateOverlayZOrder), &IDirectDrawSurface::UpdateOverlayZOrder>::stdcall});
    app->registerMethod(0x40003f, {"IDirectDrawSurface::GetDDInterface", &Wrapper<decltype(&IDirectDrawSurface::GetDDInterface), &IDirectDrawSurface::GetDDInterface>::stdcall});
    app->registerMethod(0x400040, {"IDirectDrawSurface::PageLock", &Wrapper<decltype(&IDirectDrawSurface::PageLock), &IDirectDrawSurface::PageLock>::stdcall});
    app->registerMethod(0x400041, {"IDirectDrawSurface::PageUnlock", &Wrapper<decltype(&IDirectDrawSurface::PageUnlock), &IDirectDrawSurface::PageUnlock>::stdcall});

    s_iDirectDrawSurfaceVtable = memblockIndex;
    vtable[0] = 0x40001d;    // IDirectDrawSurface_QueryInterface
    vtable[1] = 0x400001;    // IUnknown_AddRef
    vtable[2] = 0x400002;    // IUnknwon_Release
    vtable[3] = 0x40001e;    // IDirectDrawSurface_AddAttachedSurface);
    vtable[4] = 0x40001f;    // IDirectDrawSurface_AddOverlayDirtyRect);
    vtable[5] = 0x400020;    // IDirectDrawSurface_Blt);
    vtable[6] = 0x400021;    // IDirectDrawSurface_BltBatch);
    vtable[7] = 0x400022;    // IDirectDrawSurface_BltFast);
    vtable[8] = 0x400023;    // IDirectDrawSurface_DeleteAttachedSurface);
    vtable[9] = 0x400024;    // IDirectDrawSurface_EnumAttachedSurfaces);
    vtable[10] = 0x400025;    // IDirectDrawSurface_EnumOverlayZOrders);
    vtable[11] = 0x400026;    // IDirectDrawSurface_Flip);
    vtable[12] = 0x400027;    // IDirectDrawSurface_GetAttachedSurface);
    vtable[13] = 0x400028;    // IDirectDrawSurface_GetBltStatus);
    vtable[14] = 0x400029;    // IDirectDrawSurface_GetCaps);
    vtable[15] = 0x40002a;    // IDirectDrawSurface_GetClipper);
    vtable[16] = 0x40002b;    // IDirectDrawSurface_GetColorKey);
    vtable[17] = 0x40002c;    // IDirectDrawSurface_GetDC);
    vtable[18] = 0x40002d;    // IDirectDrawSurface_GetFlipStatus);
    vtable[19] = 0x40002e;    // IDirectDrawSurface_GetOverlayPosition);
    vtable[20] = 0x40002f;    // IDirectDrawSurface_GetPalette);
    vtable[21] = 0x400030;    // IDirectDrawSurface_GetPixelFormat);
    vtable[22] = 0x400031;    // IDirectDrawSurface_GetSurfaceDesc);
    vtable[23] = 0x400032;    // IDirectDrawSurface_Initialize);
    vtable[24] = 0x400033;    // IDirectDrawSurface_IsLost);
    vtable[25] = 0x400034;    // IDirectDrawSurface_Lock);
    vtable[26] = 0x400035;    // IDirectDrawSurface_ReleaseDC);
    vtable[27] = 0x400036;    // IDirectDrawSurface_Restore);
    vtable[28] = 0x400037;    // IDirectDrawSurface_SetClipper);
    vtable[29] = 0x400038;    // IDirectDrawSurface_SetColorKey);
    vtable[30] = 0x400039;    // IDirectDrawSurface_SetOverlayPosition);
    vtable[31] = 0x40003a;    // IDirectDrawSurface_SetPalette);
    vtable[32] = 0x40003b;    // IDirectDrawSurface_Unlock);
    vtable[33] = 0x40003c;    // IDirectDrawSurface_UpdateOverlay);
    vtable[34] = 0x40003d;    // IDirectDrawSurface_UpdateOverlayDisplay);
    vtable[35] = 0x40003e;    // IDirectDrawSurface_UpdateOverlayZOrder);
    vtable[36] = 0x40003f;    // IDirectDrawSurface_GetDDInterface);
    vtable[37] = 0x400040;    // IDirectDrawSurface_PageLock);
    vtable[38] = 0x400041;    // IDirectDrawSurface_PageUnlock);
    return 39 * sizeof(x86::reg32);
}

}}
