/*
 * Copyright © 2009 Red Hat, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors:
 *    Dave Airlie <airlied@redhat.com>
 *
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <sys/ioctl.h>
/* Driver data structures */
#include "amdgpu_drv.h"
#include "amdgpu_probe.h"
#include "micmap.h"

#include "amdgpu_version.h"
#include "shadow.h"

#include "amdpciids.h"

/* DPMS */
#ifdef HAVE_XEXTPROTO_71
#include <X11/extensions/dpmsconst.h>
#else
#define DPMS_SERVER
#include <X11/extensions/dpms.h>
#endif

#include "amdgpu_chipinfo_gen.h"
#include "amdgpu_bo_helper.h"
#include "amdgpu_pixmap.h"

#include <gbm.h>

extern SymTabRec AMDGPUChipsets[];
static Bool amdgpu_setup_kernel_mem(ScreenPtr pScreen);

const OptionInfoRec AMDGPUOptions_KMS[] = {
	{OPTION_NOACCEL, "NoAccel", OPTV_BOOLEAN, {0}, FALSE},
	{OPTION_SW_CURSOR, "SWcursor", OPTV_BOOLEAN, {0}, FALSE},
	{OPTION_PAGE_FLIP, "EnablePageFlip", OPTV_BOOLEAN, {0}, FALSE},
	{OPTION_SUBPIXEL_ORDER, "SubPixelOrder", OPTV_ANYSTR, {0}, FALSE},
	{OPTION_ZAPHOD_HEADS, "ZaphodHeads", OPTV_STRING, {0}, FALSE},
	{OPTION_ACCEL_METHOD, "AccelMethod", OPTV_STRING, {0}, FALSE},
	{-1, NULL, OPTV_NONE, {0}, FALSE}
};

const OptionInfoRec *AMDGPUOptionsWeak(void)
{
	return AMDGPUOptions_KMS;
}

extern _X_EXPORT int gAMDGPUEntityIndex;

static int getAMDGPUEntityIndex(void)
{
	return gAMDGPUEntityIndex;
}

AMDGPUEntPtr AMDGPUEntPriv(ScrnInfoPtr pScrn)
{
	DevUnion *pPriv;
	AMDGPUInfoPtr info = AMDGPUPTR(pScrn);
	pPriv = xf86GetEntityPrivate(info->pEnt->index, getAMDGPUEntityIndex());
	return pPriv->ptr;
}

/* Allocate our private AMDGPUInfoRec */
static Bool AMDGPUGetRec(ScrnInfoPtr pScrn)
{
	if (pScrn->driverPrivate)
		return TRUE;

	pScrn->driverPrivate = xnfcalloc(sizeof(AMDGPUInfoRec), 1);
	return TRUE;
}

/* Free our private AMDGPUInfoRec */
static void AMDGPUFreeRec(ScrnInfoPtr pScrn)
{
	AMDGPUInfoPtr info;

	if (!pScrn || !pScrn->driverPrivate)
		return;

	info = AMDGPUPTR(pScrn);

	if (info->dri2.drm_fd > 0) {
		DevUnion *pPriv;
		AMDGPUEntPtr pAMDGPUEnt;
		pPriv = xf86GetEntityPrivate(pScrn->entityList[0],
					     getAMDGPUEntityIndex());

		pAMDGPUEnt = pPriv->ptr;
		pAMDGPUEnt->fd_ref--;
		if (!pAMDGPUEnt->fd_ref) {
			amdgpu_device_deinitialize(pAMDGPUEnt->pDev);
			drmClose(pAMDGPUEnt->fd);
			pAMDGPUEnt->fd = 0;
		}
	}

	free(pScrn->driverPrivate);
	pScrn->driverPrivate = NULL;
}

static void *amdgpuShadowWindow(ScreenPtr screen, CARD32 row, CARD32 offset,
				int mode, CARD32 * size, void *closure)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(screen);
	AMDGPUInfoPtr info = AMDGPUPTR(pScrn);
	int stride;

	stride = (pScrn->displayWidth * pScrn->bitsPerPixel) / 8;
	*size = stride;

	return ((uint8_t *) info->front_buffer->cpu_ptr + row * stride + offset);
}

static Bool AMDGPUCreateScreenResources_KMS(ScreenPtr pScreen)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	AMDGPUInfoPtr info = AMDGPUPTR(pScrn);
	PixmapPtr pixmap;

	pScreen->CreateScreenResources = info->CreateScreenResources;
	if (!(*pScreen->CreateScreenResources) (pScreen))
		return FALSE;
	pScreen->CreateScreenResources = AMDGPUCreateScreenResources_KMS;

	if (!drmmode_set_desired_modes(pScrn, &info->drmmode))
		return FALSE;

	drmmode_uevent_init(pScrn, &info->drmmode);

	if (info->shadow_fb) {
		pixmap = pScreen->GetScreenPixmap(pScreen);

		if (!shadowAdd(pScreen, pixmap, shadowUpdatePackedWeak(),
			       amdgpuShadowWindow, 0, NULL))
			return FALSE;
	}

	if (info->dri2.enabled || info->use_glamor) {
		if (info->front_buffer) {
			PixmapPtr pPix = pScreen->GetScreenPixmap(pScreen);
			amdgpu_set_pixmap_bo(pPix, info->front_buffer);
		}
	}

	if (info->use_glamor)
		amdgpu_glamor_create_screen_resources(pScreen);

	return TRUE;
}

#ifdef AMDGPU_PIXMAP_SHARING
static void redisplay_dirty(ScreenPtr screen, PixmapDirtyUpdatePtr dirty)
{
	RegionRec pixregion;

	PixmapRegionInit(&pixregion, dirty->slave_dst);
	DamageRegionAppend(&dirty->slave_dst->drawable, &pixregion);
	PixmapSyncDirtyHelper(dirty, &pixregion);

	DamageRegionProcessPending(&dirty->slave_dst->drawable);
	RegionUninit(&pixregion);
}

static void amdgpu_dirty_update(ScreenPtr screen)
{
	RegionPtr region;
	PixmapDirtyUpdatePtr ent;

	if (xorg_list_is_empty(&screen->pixmap_dirty_list))
		return;

	xorg_list_for_each_entry(ent, &screen->pixmap_dirty_list, ent) {
		region = DamageRegion(ent->damage);
		if (RegionNotEmpty(region)) {
			redisplay_dirty(screen, ent);
			DamageEmpty(ent->damage);
		}
	}
}
#endif

static void AMDGPUBlockHandler_KMS(BLOCKHANDLER_ARGS_DECL)
{
	SCREEN_PTR(arg);
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	AMDGPUInfoPtr info = AMDGPUPTR(pScrn);

	pScreen->BlockHandler = info->BlockHandler;
	(*pScreen->BlockHandler) (BLOCKHANDLER_ARGS);
	pScreen->BlockHandler = AMDGPUBlockHandler_KMS;

	if (info->use_glamor)
		amdgpu_glamor_flush(pScrn);

#ifdef AMDGPU_PIXMAP_SHARING
	amdgpu_dirty_update(pScreen);
#endif
}

static void
amdgpu_flush_callback(CallbackListPtr * list,
		      pointer user_data, pointer call_data)
{
	ScrnInfoPtr pScrn = user_data;

	if (pScrn->vtSema) {
		amdgpu_glamor_flush(pScrn);
	}
}

static Bool AMDGPUIsAccelWorking(ScrnInfoPtr pScrn)
{
	AMDGPUEntPtr pAMDGPUEnt = AMDGPUEntPriv(pScrn);
	uint32_t accel_working;

	if (amdgpu_query_info(pAMDGPUEnt->pDev, AMDGPU_INFO_ACCEL_WORKING,
			      sizeof(accel_working), &accel_working) != 0)
		return FALSE;

	return accel_working;
}

/* This is called by AMDGPUPreInit to set up the default visual */
static Bool AMDGPUPreInitVisual(ScrnInfoPtr pScrn)
{
	AMDGPUInfoPtr info = AMDGPUPTR(pScrn);

	if (!xf86SetDepthBpp(pScrn, 0, 0, 0, Support32bppFb))
		return FALSE;

	switch (pScrn->depth) {
	case 8:
	case 15:
	case 16:
	case 24:
		break;

	default:
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "Given depth (%d) is not supported by %s driver\n",
			   pScrn->depth, AMDGPU_DRIVER_NAME);
		return FALSE;
	}

	xf86PrintDepthBpp(pScrn);

	info->pix24bpp = xf86GetBppFromDepth(pScrn, pScrn->depth);
	info->pixel_bytes = pScrn->bitsPerPixel / 8;

	if (info->pix24bpp == 24) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "Amdgpu does NOT support 24bpp\n");
		return FALSE;
	}

	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		   "Pixel depth = %d bits stored in %d byte%s (%d bpp pixmaps)\n",
		   pScrn->depth,
		   info->pixel_bytes,
		   info->pixel_bytes > 1 ? "s" : "", info->pix24bpp);

	if (!xf86SetDefaultVisual(pScrn, -1))
		return FALSE;

	if (pScrn->depth > 8 && pScrn->defaultVisual != TrueColor) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "Default visual (%s) is not supported at depth %d\n",
			   xf86GetVisualName(pScrn->defaultVisual),
			   pScrn->depth);
		return FALSE;
	}
	return TRUE;
}

/* This is called by AMDGPUPreInit to handle all color weight issues */
static Bool AMDGPUPreInitWeight(ScrnInfoPtr pScrn)
{
	AMDGPUInfoPtr info = AMDGPUPTR(pScrn);

	/* Save flag for 6 bit DAC to use for
	   setting CRTC registers.  Otherwise use
	   an 8 bit DAC, even if xf86SetWeight sets
	   pScrn->rgbBits to some value other than
	   8. */
	info->dac6bits = FALSE;

	if (pScrn->depth > 8) {
		rgb defaultWeight = { 0, 0, 0 };

		if (!xf86SetWeight(pScrn, defaultWeight, defaultWeight))
			return FALSE;
	} else {
		pScrn->rgbBits = 8;
	}

	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		   "Using %d bits per RGB (%d bit DAC)\n",
		   pScrn->rgbBits, info->dac6bits ? 6 : 8);

	return TRUE;
}

static Bool AMDGPUPreInitAccel_KMS(ScrnInfoPtr pScrn)
{
	AMDGPUInfoPtr info = AMDGPUPTR(pScrn);
	const char *accel_method;

	if (!xf86ReturnOptValBool(info->Options, OPTION_NOACCEL,
				 info->ChipFamily == CHIP_FAMILY_HAWAII) &&
		AMDGPUIsAccelWorking(pScrn)) {
		Bool use_glamor = TRUE;

		accel_method = xf86GetOptValString(info->Options, OPTION_ACCEL_METHOD);
		if ((accel_method && !strcmp(accel_method, "none")))
			use_glamor = FALSE;

#ifdef DRI2
		info->dri2.available = ! !xf86LoadSubModule(pScrn, "dri2");
#endif

		if (info->dri2.available)
			info->gbm = gbm_create_device(info->dri2.drm_fd);
		if (info->gbm == NULL)
			info->dri2.available = FALSE;

		if (use_glamor &&
			amdgpu_glamor_pre_init(pScrn))
			return TRUE;

		if (info->dri2.available)
			return TRUE;
	}

	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		   "GPU accel disabled or not working, using shadowfb for KMS\n");
	info->shadow_fb = TRUE;
	if (!xf86LoadSubModule(pScrn, "shadow"))
		info->shadow_fb = FALSE;

	return TRUE;
}

static Bool AMDGPUPreInitChipType_KMS(ScrnInfoPtr pScrn)
{
	AMDGPUInfoPtr info = AMDGPUPTR(pScrn);
	int i;

	info->Chipset = PCI_DEV_DEVICE_ID(info->PciInfo);
	pScrn->chipset =
	    (char *)xf86TokenToString(AMDGPUChipsets, info->Chipset);
	if (!pScrn->chipset) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "ChipID 0x%04x is not recognized\n", info->Chipset);
		return FALSE;
	}

	if (info->Chipset < 0) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "Chipset \"%s\" is not recognized\n",
			   pScrn->chipset);
		return FALSE;
	}
	xf86DrvMsg(pScrn->scrnIndex, X_PROBED,
		   "Chipset: \"%s\" (ChipID = 0x%04x)\n",
		   pScrn->chipset, info->Chipset);

	for (i = 0; i < sizeof(AMDGPUCards) / sizeof(AMDGPUCardInfo); i++) {
		if (info->Chipset == AMDGPUCards[i].pci_device_id) {
			AMDGPUCardInfo *card = &AMDGPUCards[i];
			info->ChipFamily = card->chip_family;
			break;
		}
	}

	return TRUE;
}

static void amdgpu_reference_drm_fd(ScrnInfoPtr pScrn)
{
	AMDGPUInfoPtr info = AMDGPUPTR(pScrn);
	AMDGPUEntPtr pAMDGPUEnt = AMDGPUEntPriv(pScrn);

	info->dri2.drm_fd = pAMDGPUEnt->fd;
	pAMDGPUEnt->fd_ref++;
	info->drmmode.fd = info->dri2.drm_fd;
}

static Bool amdgpu_get_tile_config(ScrnInfoPtr pScrn)
{
	AMDGPUInfoPtr info = AMDGPUPTR(pScrn);
	AMDGPUEntPtr pAMDGPUEnt = AMDGPUEntPriv(pScrn);
	struct amdgpu_gpu_info gpu_info;

	memset(&gpu_info, 0, sizeof(gpu_info));
	amdgpu_query_gpu_info(pAMDGPUEnt->pDev, &gpu_info);

	switch ((gpu_info.gb_addr_cfg & 0x70) >> 4) {
	case 0:
		info->group_bytes = 256;
		break;
	case 1:
		info->group_bytes = 512;
		break;
	default:
		return FALSE;
	}

	info->have_tiling_info = TRUE;
	return TRUE;
}

static void AMDGPUSetupCapabilities(ScrnInfoPtr pScrn)
{
#ifdef AMDGPU_PIXMAP_SHARING
	AMDGPUInfoPtr info = AMDGPUPTR(pScrn);
	uint64_t value;
	int ret;

	pScrn->capabilities = 0;
	ret = drmGetCap(info->dri2.drm_fd, DRM_CAP_PRIME, &value);
	if (ret == 0) {
		if (value & DRM_PRIME_CAP_EXPORT)
			pScrn->capabilities |=
			    RR_Capability_SourceOutput |
			    RR_Capability_SinkOffload;
		if (value & DRM_PRIME_CAP_IMPORT)
			pScrn->capabilities |=
			    RR_Capability_SourceOffload |
			    RR_Capability_SinkOutput;
	}
#endif
}

Bool AMDGPUPreInit_KMS(ScrnInfoPtr pScrn, int flags)
{
	AMDGPUInfoPtr info;
	AMDGPUEntPtr pAMDGPUEnt;
	DevUnion *pPriv;
	Gamma zeros = { 0.0, 0.0, 0.0 };
	int cpp;
	uint64_t heap_size = 0;
	uint64_t max_allocation = 0;

	if (flags & PROBE_DETECT)
		return TRUE;

	xf86DrvMsgVerb(pScrn->scrnIndex, X_INFO, AMDGPU_LOGLEVEL_DEBUG,
		       "AMDGPUPreInit_KMS\n");
	if (pScrn->numEntities != 1)
		return FALSE;
	if (!AMDGPUGetRec(pScrn))
		return FALSE;

	info = AMDGPUPTR(pScrn);
	info->IsSecondary = FALSE;
	info->IsPrimary = FALSE;
	info->pEnt =
	    xf86GetEntityInfo(pScrn->entityList[pScrn->numEntities - 1]);
	if (info->pEnt->location.type != BUS_PCI
#ifdef XSERVER_PLATFORM_BUS
	    && info->pEnt->location.type != BUS_PLATFORM
#endif
	    )
		goto fail;

	pPriv = xf86GetEntityPrivate(pScrn->entityList[0],
				     getAMDGPUEntityIndex());
	pAMDGPUEnt = pPriv->ptr;

	if (xf86IsEntityShared(pScrn->entityList[0])) {
		if (xf86IsPrimInitDone(pScrn->entityList[0])) {
			info->IsSecondary = TRUE;
			pAMDGPUEnt->pSecondaryScrn = pScrn;
		} else {
			info->IsPrimary = TRUE;
			xf86SetPrimInitDone(pScrn->entityList[0]);
			pAMDGPUEnt->pPrimaryScrn = pScrn;
			pAMDGPUEnt->HasSecondary = FALSE;
		}
	}

	info->PciInfo = xf86GetPciInfoForEntity(info->pEnt->index);
	pScrn->monitor = pScrn->confScreen->monitor;

	if (!AMDGPUPreInitVisual(pScrn))
		goto fail;

	xf86CollectOptions(pScrn, NULL);
	if (!(info->Options = malloc(sizeof(AMDGPUOptions_KMS))))
		goto fail;

	memcpy(info->Options, AMDGPUOptions_KMS, sizeof(AMDGPUOptions_KMS));
	xf86ProcessOptions(pScrn->scrnIndex, pScrn->options, info->Options);

	if (!AMDGPUPreInitWeight(pScrn))
		goto fail;

	if (!AMDGPUPreInitChipType_KMS(pScrn))
		goto fail;

	amdgpu_reference_drm_fd(pScrn);

	info->dri2.available = FALSE;
	info->dri2.enabled = FALSE;
	info->dri2.pKernelDRMVersion = drmGetVersion(info->dri2.drm_fd);
	if (info->dri2.pKernelDRMVersion == NULL) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "AMDGPUDRIGetVersion failed to get the DRM version\n");
		goto fail;
	}

	if (!AMDGPUPreInitAccel_KMS(pScrn))
		goto fail;

	AMDGPUSetupCapabilities(pScrn);

	/* don't enable tiling if accel is not enabled */
	if (info->use_glamor) {
		/* set default group bytes, overridden by kernel info below */
		info->group_bytes = 256;
		info->have_tiling_info = FALSE;
		amdgpu_get_tile_config(pScrn);
	}

	info->allowPageFlip = xf86ReturnOptValBool(info->Options,
						   OPTION_PAGE_FLIP,
						   TRUE);
	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		   "KMS Pageflipping: %sabled\n",
		   info->allowPageFlip ? "en" : "dis");

	if (drmmode_pre_init(pScrn, &info->drmmode, pScrn->bitsPerPixel / 8) ==
	    FALSE) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "Kernel modesetting setup failed\n");
		goto fail;
	}

	if (info->drmmode.mode_res->count_crtcs == 1)
		pAMDGPUEnt->HasCRTC2 = FALSE;
	else
		pAMDGPUEnt->HasCRTC2 = TRUE;

	info->cursor_w = CURSOR_WIDTH_CIK;
	info->cursor_h = CURSOR_HEIGHT_CIK;

	amdgpu_query_heap_size(pAMDGPUEnt->pDev, AMDGPU_GEM_DOMAIN_GTT,
				&heap_size, &max_allocation);
	info->gart_size = heap_size;
	amdgpu_query_heap_size(pAMDGPUEnt->pDev, AMDGPU_GEM_DOMAIN_VRAM,
				&heap_size, &max_allocation);
	info->vram_size = max_allocation;

	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		   "mem size init: gart size :%llx vram size: s:%llx visible:%llx\n",
		   (unsigned long long)info->gart_size,
		   (unsigned long long)heap_size,
		   (unsigned long long)max_allocation);

	cpp = pScrn->bitsPerPixel / 8;
	pScrn->displayWidth =
	    AMDGPU_ALIGN(pScrn->virtualX, drmmode_get_pitch_align(pScrn, cpp));

	/* Set display resolution */
	xf86SetDpi(pScrn, 0, 0);

	/* Get ScreenInit function */
	if (!xf86LoadSubModule(pScrn, "fb"))
		return FALSE;

	if (!xf86SetGamma(pScrn, zeros))
		return FALSE;

	if (!xf86ReturnOptValBool(info->Options, OPTION_SW_CURSOR, FALSE)) {
		if (!xf86LoadSubModule(pScrn, "ramdac"))
			return FALSE;
	}

	if (pScrn->modes == NULL
#ifdef XSERVER_PLATFORM_BUS
	    && !pScrn->is_gpu
#endif
	    ) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "No modes.\n");
		goto fail;
	}

	return TRUE;
fail:
	AMDGPUFreeRec(pScrn);
	return FALSE;

}

static Bool AMDGPUCursorInit_KMS(ScreenPtr pScreen)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	AMDGPUInfoPtr info = AMDGPUPTR(pScrn);

	return xf86_cursors_init(pScreen, info->cursor_w, info->cursor_h,
				 (HARDWARE_CURSOR_TRUECOLOR_AT_8BPP |
				  HARDWARE_CURSOR_AND_SOURCE_WITH_MASK |
				  HARDWARE_CURSOR_SOURCE_MASK_INTERLEAVE_1 |
				  HARDWARE_CURSOR_UPDATE_UNHIDDEN |
				  HARDWARE_CURSOR_ARGB));
}

void AMDGPUBlank(ScrnInfoPtr pScrn)
{
	xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);
	xf86OutputPtr output;
	xf86CrtcPtr crtc;
	int o, c;

	for (c = 0; c < xf86_config->num_crtc; c++) {
		crtc = xf86_config->crtc[c];
		for (o = 0; o < xf86_config->num_output; o++) {
			output = xf86_config->output[o];
			if (output->crtc != crtc)
				continue;

			output->funcs->dpms(output, DPMSModeOff);
		}
		crtc->funcs->dpms(crtc, DPMSModeOff);
	}
}

void AMDGPUUnblank(ScrnInfoPtr pScrn)
{
	xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);
	xf86OutputPtr output;
	xf86CrtcPtr crtc;
	int o, c;
	for (c = 0; c < xf86_config->num_crtc; c++) {
		crtc = xf86_config->crtc[c];
		if (!crtc->enabled)
			continue;
		crtc->funcs->dpms(crtc, DPMSModeOn);
		for (o = 0; o < xf86_config->num_output; o++) {
			output = xf86_config->output[o];
			if (output->crtc != crtc)
				continue;
			output->funcs->dpms(output, DPMSModeOn);
		}
	}
}

static Bool AMDGPUSaveScreen_KMS(ScreenPtr pScreen, int mode)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	Bool unblank;

	xf86DrvMsgVerb(pScrn->scrnIndex, X_INFO, AMDGPU_LOGLEVEL_DEBUG,
		       "AMDGPUSaveScreen(%d)\n", mode);

	unblank = xf86IsUnblank(mode);
	if (unblank)
		SetTimeSinceLastInputEvent();

	if ((pScrn != NULL) && pScrn->vtSema) {
		if (unblank)
			AMDGPUUnblank(pScrn);
		else
			AMDGPUBlank(pScrn);
	}
	return TRUE;
}

/* Called at the end of each server generation.  Restore the original
 * text mode, unmap video memory, and unwrap and call the saved
 * CloseScreen function.
 */
static Bool AMDGPUCloseScreen_KMS(CLOSE_SCREEN_ARGS_DECL)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	AMDGPUInfoPtr info = AMDGPUPTR(pScrn);

	xf86DrvMsgVerb(pScrn->scrnIndex, X_INFO, AMDGPU_LOGLEVEL_DEBUG,
		       "AMDGPUCloseScreen\n");

	drmmode_uevent_fini(pScrn, &info->drmmode);

	DeleteCallback(&FlushCallback, amdgpu_flush_callback, pScrn);

	drmDropMaster(info->dri2.drm_fd);

	drmmode_fini(pScrn, &info->drmmode);
	if (info->dri2.enabled) {
		amdgpu_dri2_close_screen(pScreen);
	}
	pScrn->vtSema = FALSE;
	xf86ClearPrimInitDone(info->pEnt->index);
	pScreen->BlockHandler = info->BlockHandler;
	pScreen->CloseScreen = info->CloseScreen;
	return (*pScreen->CloseScreen) (CLOSE_SCREEN_ARGS);
}

void AMDGPUFreeScreen_KMS(FREE_SCREEN_ARGS_DECL)
{
	SCRN_INFO_PTR(arg);
	AMDGPUInfoPtr info = AMDGPUPTR(pScrn);

	xf86DrvMsgVerb(pScrn->scrnIndex, X_INFO, AMDGPU_LOGLEVEL_DEBUG,
		       "AMDGPUFreeScreen\n");

	/* when server quits at PreInit, we don't need do this anymore */
	if (!info)
		return;

	AMDGPUFreeRec(pScrn);
}

Bool AMDGPUScreenInit_KMS(SCREEN_INIT_ARGS_DECL)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	AMDGPUInfoPtr info = AMDGPUPTR(pScrn);
	int subPixelOrder = SubPixelUnknown;
	char *s;
	void *front_ptr;
	int ret;

	pScrn->fbOffset = 0;

	miClearVisualTypes();
	if (!miSetVisualTypes(pScrn->depth,
			      miGetDefaultVisualMask(pScrn->depth),
			      pScrn->rgbBits, pScrn->defaultVisual))
		return FALSE;
	miSetPixmapDepths();

	ret = drmSetMaster(info->dri2.drm_fd);
	if (ret) {
		ErrorF("Unable to retrieve master\n");
		return FALSE;
	}
	info->directRenderingEnabled = FALSE;
	if (info->shadow_fb == FALSE)
		info->directRenderingEnabled = amdgpu_dri2_screen_init(pScreen);

	if (!amdgpu_setup_kernel_mem(pScreen)) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "amdgpu_setup_kernel_mem failed\n");
		return FALSE;
	}
	front_ptr = info->front_buffer->cpu_ptr;

	if (info->shadow_fb) {
		info->fb_shadow = calloc(1,
					 pScrn->displayWidth * pScrn->virtualY *
					 ((pScrn->bitsPerPixel + 7) >> 3));
		if (info->fb_shadow == NULL) {
			xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				   "Failed to allocate shadow framebuffer\n");
			info->shadow_fb = FALSE;
		} else {
			if (!fbScreenInit(pScreen, info->fb_shadow,
					  pScrn->virtualX, pScrn->virtualY,
					  pScrn->xDpi, pScrn->yDpi,
					  pScrn->displayWidth,
					  pScrn->bitsPerPixel))
				return FALSE;
		}
	}

	if (info->shadow_fb == FALSE) {
		/* Init fb layer */
		if (!fbScreenInit(pScreen, front_ptr,
				  pScrn->virtualX, pScrn->virtualY,
				  pScrn->xDpi, pScrn->yDpi, pScrn->displayWidth,
				  pScrn->bitsPerPixel))
			return FALSE;
	}

	xf86SetBlackWhitePixels(pScreen);

	if (pScrn->bitsPerPixel > 8) {
		VisualPtr visual;

		visual = pScreen->visuals + pScreen->numVisuals;
		while (--visual >= pScreen->visuals) {
			if ((visual->class | DynamicClass) == DirectColor) {
				visual->offsetRed = pScrn->offset.red;
				visual->offsetGreen = pScrn->offset.green;
				visual->offsetBlue = pScrn->offset.blue;
				visual->redMask = pScrn->mask.red;
				visual->greenMask = pScrn->mask.green;
				visual->blueMask = pScrn->mask.blue;
			}
		}
	}

	/* Must be after RGB order fixed */
	fbPictureInit(pScreen, 0, 0);

#ifdef RENDER
	if ((s = xf86GetOptValString(info->Options, OPTION_SUBPIXEL_ORDER))) {
		if (strcmp(s, "RGB") == 0)
			subPixelOrder = SubPixelHorizontalRGB;
		else if (strcmp(s, "BGR") == 0)
			subPixelOrder = SubPixelHorizontalBGR;
		else if (strcmp(s, "NONE") == 0)
			subPixelOrder = SubPixelNone;
		PictureSetSubpixelOrder(pScreen, subPixelOrder);
	}
#endif

	pScrn->vtSema = TRUE;
	xf86SetBackingStore(pScreen);

	if (info->directRenderingEnabled) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "Direct rendering enabled\n");
	} else {
		xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
			   "Direct rendering disabled\n");
	}

	if (info->use_glamor && info->directRenderingEnabled) {
		xf86DrvMsgVerb(pScrn->scrnIndex, X_INFO, AMDGPU_LOGLEVEL_DEBUG,
			       "Initializing Acceleration\n");
		if (amdgpu_glamor_init(pScreen)) {
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				   "Acceleration enabled\n");
		} else {
			xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				   "Acceleration initialization failed\n");
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				   "2D and 3D acceleration disabled\n");
			info->use_glamor = FALSE;
		}
	} else if (info->directRenderingEnabled) {
		if (!amdgpu_pixmap_init(pScreen))
			xf86DrvMsg(pScrn->scrnIndex, X_INFO, "3D acceleration disabled\n");
		xf86DrvMsg(pScrn->scrnIndex, X_INFO, "2D acceleration disabled\n");
	} else {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO, "2D and 3D cceleration disabled\n");
	}

	/* Init DPMS */
	xf86DrvMsgVerb(pScrn->scrnIndex, X_INFO, AMDGPU_LOGLEVEL_DEBUG,
		       "Initializing DPMS\n");
	xf86DPMSInit(pScreen, xf86DPMSSet, 0);

	xf86DrvMsgVerb(pScrn->scrnIndex, X_INFO, AMDGPU_LOGLEVEL_DEBUG,
		       "Initializing Cursor\n");

	/* Set Silken Mouse */
	xf86SetSilkenMouse(pScreen);

	/* Cursor setup */
	miDCInitialize(pScreen, xf86GetPointerScreenFuncs());

	if (!xf86ReturnOptValBool(info->Options, OPTION_SW_CURSOR, FALSE)) {
		if (AMDGPUCursorInit_KMS(pScreen)) {
		}
	}

	/* DGA setup */
#ifdef XFreeXDGA
	/* DGA is dangerous on kms as the base and framebuffer location may change:
	 * http://lists.freedesktop.org/archives/xorg-devel/2009-September/002113.html
	 */
	/* xf86DiDGAInit(pScreen, info->LinearAddr + pScrn->fbOffset); */
#endif
	if (info->shadow_fb == FALSE) {
		/* Init Xv */
		xf86DrvMsgVerb(pScrn->scrnIndex, X_INFO, AMDGPU_LOGLEVEL_DEBUG,
			       "Initializing Xv\n");
		AMDGPUInitVideo(pScreen);
	}

	if (info->shadow_fb == TRUE) {
		if (!shadowSetup(pScreen)) {
			xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				   "Shadowfb initialization failed\n");
			return FALSE;
		}
	}
	pScrn->pScreen = pScreen;

	/* Provide SaveScreen & wrap BlockHandler and CloseScreen */
	/* Wrap CloseScreen */
	info->CloseScreen = pScreen->CloseScreen;
	pScreen->CloseScreen = AMDGPUCloseScreen_KMS;
	pScreen->SaveScreen = AMDGPUSaveScreen_KMS;
	info->BlockHandler = pScreen->BlockHandler;
	pScreen->BlockHandler = AMDGPUBlockHandler_KMS;

	if (!AddCallback(&FlushCallback, amdgpu_flush_callback, pScrn))
		return FALSE;

	info->CreateScreenResources = pScreen->CreateScreenResources;
	pScreen->CreateScreenResources = AMDGPUCreateScreenResources_KMS;

#ifdef AMDGPU_PIXMAP_SHARING
	pScreen->StartPixmapTracking = PixmapStartDirtyTracking;
	pScreen->StopPixmapTracking = PixmapStopDirtyTracking;
#endif

	if (!xf86CrtcScreenInit(pScreen))
		return FALSE;

	/* Wrap pointer motion to flip touch screen around */
//    info->PointerMoved = pScrn->PointerMoved;
//    pScrn->PointerMoved = AMDGPUPointerMoved;

	if (!drmmode_setup_colormap(pScreen, pScrn))
		return FALSE;

	/* Note unused options */
	if (serverGeneration == 1)
		xf86ShowUnusedOptions(pScrn->scrnIndex, pScrn->options);

	drmmode_init(pScrn, &info->drmmode);

	xf86DrvMsgVerb(pScrn->scrnIndex, X_INFO, AMDGPU_LOGLEVEL_DEBUG,
		       "AMDGPUScreenInit finished\n");

	return TRUE;
}

Bool AMDGPUEnterVT_KMS(VT_FUNC_ARGS_DECL)
{
	SCRN_INFO_PTR(arg);
	AMDGPUInfoPtr info = AMDGPUPTR(pScrn);
	int ret;

	xf86DrvMsgVerb(pScrn->scrnIndex, X_INFO, AMDGPU_LOGLEVEL_DEBUG,
		       "AMDGPUEnterVT_KMS\n");

	ret = drmSetMaster(info->dri2.drm_fd);
	if (ret)
		ErrorF("Unable to retrieve master\n");

	pScrn->vtSema = TRUE;

	if (!drmmode_set_desired_modes(pScrn, &info->drmmode))
		return FALSE;

	return TRUE;
}

void AMDGPULeaveVT_KMS(VT_FUNC_ARGS_DECL)
{
	SCRN_INFO_PTR(arg);
	AMDGPUInfoPtr info = AMDGPUPTR(pScrn);

	xf86DrvMsgVerb(pScrn->scrnIndex, X_INFO, AMDGPU_LOGLEVEL_DEBUG,
		       "AMDGPULeaveVT_KMS\n");

	drmDropMaster(info->dri2.drm_fd);

	xf86RotateFreeShadow(pScrn);

	xf86_hide_cursors(pScrn);

	xf86DrvMsgVerb(pScrn->scrnIndex, X_INFO, AMDGPU_LOGLEVEL_DEBUG,
		       "Ok, leaving now...\n");
}

Bool AMDGPUSwitchMode_KMS(SWITCH_MODE_ARGS_DECL)
{
	SCRN_INFO_PTR(arg);
	Bool ret;
	ret = xf86SetSingleMode(pScrn, mode, RR_Rotate_0);
	return ret;

}

void AMDGPUAdjustFrame_KMS(ADJUST_FRAME_ARGS_DECL)
{
	SCRN_INFO_PTR(arg);
	AMDGPUInfoPtr info = AMDGPUPTR(pScrn);
	drmmode_adjust_frame(pScrn, &info->drmmode, x, y);
	return;
}

static Bool amdgpu_setup_kernel_mem(ScreenPtr pScreen)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	AMDGPUInfoPtr info = AMDGPUPTR(pScrn);
	xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);
	int cpp = info->pixel_bytes;
	int cursor_size;
	int c;

	cursor_size = info->cursor_w * info->cursor_h * 4;
	cursor_size = AMDGPU_ALIGN(cursor_size, AMDGPU_GPU_PAGE_SIZE);
	for (c = 0; c < xf86_config->num_crtc; c++) {
		/* cursor objects */
		if (info->cursor_buffer[c] == NULL) {
			if (info->gbm) {
				info->cursor_buffer[c] = (struct amdgpu_buffer *)calloc(1, sizeof(struct amdgpu_buffer));
				if (!info->cursor_buffer[c]) {
					return FALSE;
				}
				info->cursor_buffer[c]->ref_count = 1;

				info->cursor_buffer[c]->bo.gbm = gbm_bo_create(info->gbm,
									       info->cursor_w,
									       info->cursor_h,
									       GBM_FORMAT_ARGB8888,
									       GBM_BO_USE_CURSOR | GBM_BO_USE_WRITE);
				if (!info->cursor_buffer[c]->bo.gbm) {
					xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
						   "Failed to allocate cursor buffer memory\n");
					free(info->cursor_buffer[c]);
					return FALSE;
				}
			} else {
				AMDGPUEntPtr pAMDGPUEnt = AMDGPUEntPriv(pScrn);
				info->cursor_buffer[c] = amdgpu_bo_open(pAMDGPUEnt->pDev,
									cursor_size,
									0,
									AMDGPU_GEM_DOMAIN_VRAM);
				if (!(info->cursor_buffer[c])) {
					ErrorF("Failed to allocate cursor buffer memory\n");
					return FALSE;
				}

				if (amdgpu_bo_cpu_map(info->cursor_buffer[c]->bo.amdgpu,
							&info->cursor_buffer[c]->cpu_ptr)) {
					ErrorF("Failed to map cursor buffer memory\n");
				}
			}

			drmmode_set_cursor(pScrn, &info->drmmode, c,
					   info->cursor_buffer[c]);
		}
	}

	if (info->front_buffer == NULL) {
		int pitch;
		int hint = info->use_glamor ? 0 : AMDGPU_CREATE_PIXMAP_LINEAR;

		info->front_buffer =
			amdgpu_alloc_pixmap_bo(pScrn, pScrn->virtualX,
					       pScrn->virtualY, pScrn->depth,
					       hint, pScrn->bitsPerPixel,
					       &pitch);
		if (!(info->front_buffer)) {
			ErrorF("Failed to allocate front buffer memory\n");
			return FALSE;
		}

		if (amdgpu_bo_map(pScrn, info->front_buffer)) {
			ErrorF("Failed to map front buffer memory\n");
			return FALSE;
		}

		pScrn->displayWidth = pitch / cpp;
	}

	xf86DrvMsg(pScrn->scrnIndex, X_INFO, "Front buffer pitch: %d bytes\n",
		   pScrn->displayWidth * cpp);
	return TRUE;
}

/* Used to disallow modes that are not supported by the hardware */
ModeStatus AMDGPUValidMode(SCRN_ARG_TYPE arg, DisplayModePtr mode,
			   Bool verbose, int flag)
{
	/* There are problems with double scan mode at high clocks
	 * They're likely related PLL and display buffer settings.
	 * Disable these modes for now.
	 */
	if (mode->Flags & V_DBLSCAN) {
		if ((mode->CrtcHDisplay >= 1024) || (mode->CrtcVDisplay >= 768))
			return MODE_CLOCK_RANGE;
	}
	return MODE_OK;
}
