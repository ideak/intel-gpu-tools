#include "intel_chipset.h"
#include "i915_pciids.h"
#include "i915_pciids_local.h"

#include <strings.h> /* ffs() */

static const struct intel_device_info intel_generic_info = {
	.graphics_ver = 0,
	.display_ver = 0,
};

static const struct intel_device_info intel_i810_info = {
	.graphics_ver = 1,
	.display_ver = 1,
	.is_whitney = true,
	.codename = "solano" /* 815 == "whitney" ? or vice versa? */
};

static const struct intel_device_info intel_i815_info = {
	.graphics_ver = 1,
	.display_ver = 1,
	.is_whitney = true,
	.codename = "whitney"
};

static const struct intel_device_info intel_i830_info = {
	.graphics_ver = 2,
	.display_ver = 2,
	.is_almador = true,
	.cmds_info = &pre_gen6_cmds_info,
	.codename = "almador"
};
static const struct intel_device_info intel_i845_info = {
	.graphics_ver = 2,
	.display_ver = 2,
	.is_brookdale = true,
	.cmds_info = &pre_gen6_cmds_info,
	.codename = "brookdale"
};
static const struct intel_device_info intel_i855_info = {
	.graphics_ver = 2,
	.display_ver = 2,
	.is_mobile = true,
	.is_montara = true,
	.cmds_info = &pre_gen6_cmds_info,
	.codename = "montara"
};
static const struct intel_device_info intel_i865_info = {
	.graphics_ver = 2,
	.display_ver = 2,
	.is_springdale = true,
	.cmds_info = &pre_gen6_cmds_info,
	.codename = "spingdale"
};

static const struct intel_device_info intel_i915_info = {
	.graphics_ver = 3,
	.display_ver = 3,
	.is_grantsdale = true,
	.cmds_info = &pre_gen6_cmds_info,
	.codename = "grantsdale"
};
static const struct intel_device_info intel_i915m_info = {
	.graphics_ver = 3,
	.display_ver = 3,
	.is_mobile = true,
	.is_alviso = true,
	.cmds_info = &pre_gen6_cmds_info,
	.codename = "alviso"
};
static const struct intel_device_info intel_i945_info = {
	.graphics_ver = 3,
	.display_ver = 3,
	.is_lakeport = true,
	.cmds_info = &pre_gen6_cmds_info,
	.codename = "lakeport"
};
static const struct intel_device_info intel_i945m_info = {
	.graphics_ver = 3,
	.display_ver = 3,
	.is_mobile = true,
	.is_calistoga = true,
	.cmds_info = &pre_gen6_cmds_info,
	.codename = "calistoga"
};

static const struct intel_device_info intel_g33_info = {
	.graphics_ver = 3,
	.display_ver = 3,
	.is_bearlake = true,
	.cmds_info = &pre_gen6_cmds_info,
	.codename = "bearlake"
};

static const struct intel_device_info intel_pineview_g_info = {
	.graphics_ver = 3,
	.display_ver = 3,
	.is_pineview = true,
	.cmds_info = &pre_gen6_cmds_info,
	.codename = "pineview"
};

static const struct intel_device_info intel_pineview_m_info = {
	.graphics_ver = 3,
	.display_ver = 3,
	.is_mobile = true,
	.is_pineview = true,
	.cmds_info = &pre_gen6_cmds_info,
	.codename = "pineview"
};

static const struct intel_device_info intel_i965_info = {
	.graphics_ver = 4,
	.display_ver = 4,
	.is_broadwater = true,
	.cmds_info = &pre_gen6_cmds_info,
	.codename = "broadwater"
};

static const struct intel_device_info intel_i965m_info = {
	.graphics_ver = 4,
	.display_ver = 4,
	.is_mobile = true,
	.is_crestline = true,
	.cmds_info = &pre_gen6_cmds_info,
	.codename = "crestline"
};

static const struct intel_device_info intel_g45_info = {
	.graphics_ver = 4,
	.display_ver = 4,
	.is_eaglelake = true,
	.cmds_info = &pre_gen6_cmds_info,
	.codename = "eaglelake"
};
static const struct intel_device_info intel_gm45_info = {
	.graphics_ver = 4,
	.display_ver = 4,
	.is_mobile = true,
	.is_cantiga = true,
	.cmds_info = &pre_gen6_cmds_info,
	.codename = "cantiga"
};

static const struct intel_device_info intel_ironlake_info = {
	.graphics_ver = 5,
	.display_ver = 5,
	.is_ironlake = true,
	.cmds_info = &pre_gen6_cmds_info,
	.codename = "ironlake" /* clarkdale? */
};
static const struct intel_device_info intel_ironlake_m_info = {
	.graphics_ver = 5,
	.display_ver = 5,
	.is_mobile = true,
	.is_arrandale = true,
	.cmds_info = &pre_gen6_cmds_info,
	.codename = "arrandale"
};

static const struct intel_device_info intel_sandybridge_info = {
	.graphics_ver = 6,
	.display_ver = 6,
	.is_sandybridge = true,
	.cmds_info = &gen6_cmds_info,
	.codename = "sandybridge"
};
static const struct intel_device_info intel_sandybridge_m_info = {
	.graphics_ver = 6,
	.display_ver = 6,
	.is_mobile = true,
	.is_sandybridge = true,
	.cmds_info = &gen6_cmds_info,
	.codename = "sandybridge"
};

static const struct intel_device_info intel_ivybridge_info = {
	.graphics_ver = 7,
	.display_ver = 7,
	.is_ivybridge = true,
	.cmds_info = &gen6_cmds_info,
	.codename = "ivybridge"
};
static const struct intel_device_info intel_ivybridge_m_info = {
	.graphics_ver = 7,
	.display_ver = 7,
	.is_mobile = true,
	.is_ivybridge = true,
	.cmds_info = &gen6_cmds_info,
	.codename = "ivybridge"
};

static const struct intel_device_info intel_valleyview_info = {
	.graphics_ver = 7,
	.display_ver = 7,
	.is_valleyview = true,
	.cmds_info = &gen6_cmds_info,
	.codename = "valleyview"
};

#define HASWELL_FIELDS \
	.graphics_ver = 7, \
	.display_ver = 7, \
	.is_haswell = true, \
	.cmds_info = &gen6_cmds_info, \
	.codename = "haswell"

static const struct intel_device_info intel_haswell_gt1_info = {
	HASWELL_FIELDS,
	.gt = 1,
};

static const struct intel_device_info intel_haswell_gt2_info = {
	HASWELL_FIELDS,
	.gt = 2,
};

static const struct intel_device_info intel_haswell_gt3_info = {
	HASWELL_FIELDS,
	.gt = 3,
};

#define BROADWELL_FIELDS \
	.graphics_ver = 8, \
	.display_ver = 8, \
	.is_broadwell = true, \
	.cmds_info = &gen8_cmds_info, \
	.codename = "broadwell"

static const struct intel_device_info intel_broadwell_gt1_info = {
	BROADWELL_FIELDS,
	.gt = 1,
};

static const struct intel_device_info intel_broadwell_gt2_info = {
	BROADWELL_FIELDS,
	.gt = 2,
};

static const struct intel_device_info intel_broadwell_gt3_info = {
	BROADWELL_FIELDS,
	.gt = 3,
};

static const struct intel_device_info intel_broadwell_unknown_info = {
	BROADWELL_FIELDS,
};

static const struct intel_device_info intel_cherryview_info = {
	.graphics_ver = 8,
	.display_ver = 8,
	.is_cherryview = true,
	.cmds_info = &gen8_cmds_info,
	.codename = "cherryview"
};

#define SKYLAKE_FIELDS \
	.graphics_ver = 9, \
	.display_ver = 9, \
	.cmds_info = &gen11_cmds_info, \
	.codename = "skylake", \
	.is_skylake = true

static const struct intel_device_info intel_skylake_gt1_info = {
	SKYLAKE_FIELDS,
	.gt = 1,
};

static const struct intel_device_info intel_skylake_gt2_info = {
	SKYLAKE_FIELDS,
	.gt = 2,
};

static const struct intel_device_info intel_skylake_gt3_info = {
	SKYLAKE_FIELDS,
	.gt = 3,
};

static const struct intel_device_info intel_skylake_gt4_info = {
	SKYLAKE_FIELDS,
	.gt = 4,
};

static const struct intel_device_info intel_broxton_info = {
	.graphics_ver = 9,
	.display_ver = 9,
	.is_broxton = true,
	.cmds_info = &gen11_cmds_info,
	.codename = "broxton"
};

#define KABYLAKE_FIELDS \
	.graphics_ver = 9, \
	.display_ver = 9, \
	.is_kabylake = true, \
	.cmds_info = &gen11_cmds_info, \
	.codename = "kabylake"

static const struct intel_device_info intel_kabylake_gt1_info = {
	KABYLAKE_FIELDS,
	.gt = 1,
};

static const struct intel_device_info intel_kabylake_gt2_info = {
	KABYLAKE_FIELDS,
	.gt = 2,
};

static const struct intel_device_info intel_kabylake_gt3_info = {
	KABYLAKE_FIELDS,
	.gt = 3,
};

static const struct intel_device_info intel_kabylake_gt4_info = {
	KABYLAKE_FIELDS,
	.gt = 4,
};

static const struct intel_device_info intel_geminilake_info = {
	.graphics_ver = 9,
	.display_ver = 9,
	.is_geminilake = true,
	.cmds_info = &gen11_cmds_info,
	.codename = "geminilake"
};

#define COFFEELAKE_FIELDS \
	.graphics_ver = 9, \
	.display_ver = 9, \
	.is_coffeelake = true, \
	.cmds_info = &gen11_cmds_info, \
	.codename = "coffeelake"

static const struct intel_device_info intel_coffeelake_gt1_info = {
	COFFEELAKE_FIELDS,
	.gt = 1,
};

static const struct intel_device_info intel_coffeelake_gt2_info = {
	COFFEELAKE_FIELDS,
	.gt = 2,
};

static const struct intel_device_info intel_coffeelake_gt3_info = {
	COFFEELAKE_FIELDS,
	.gt = 3,
};

#define COMETLAKE_FIELDS \
	.graphics_ver = 9, \
	.display_ver = 9, \
	.is_cometlake = true, \
	.cmds_info = &gen11_cmds_info, \
	.codename = "cometlake"

static const struct intel_device_info intel_cometlake_gt1_info = {
	COMETLAKE_FIELDS,
	.gt = 1,
};

static const struct intel_device_info intel_cometlake_gt2_info = {
	COMETLAKE_FIELDS,
	.gt = 2,
};

static const struct intel_device_info intel_cannonlake_info = {
	.graphics_ver = 10,
	.display_ver = 10,
	.is_cannonlake = true,
	.cmds_info = &gen11_cmds_info,
	.codename = "cannonlake"
};

static const struct intel_device_info intel_icelake_info = {
	.graphics_ver = 11,
	.display_ver = 11,
	.is_icelake = true,
	.cmds_info = &gen11_cmds_info,
	.codename = "icelake"
};

static const struct intel_device_info intel_elkhartlake_info = {
	.graphics_ver = 11,
	.display_ver = 11,
	.is_elkhartlake = true,
	.cmds_info = &gen11_cmds_info,
	.codename = "elkhartlake"
};

static const struct intel_device_info intel_jasperlake_info = {
	.graphics_ver = 11,
	.display_ver = 11,
	.is_jasperlake = true,
	.cmds_info = &gen11_cmds_info,
	.codename = "jasperlake"
};

static const struct intel_device_info intel_tigerlake_gt1_info = {
	.graphics_ver = 12,
	.display_ver = 12,
	.is_tigerlake = true,
	.cmds_info = &gen12_cmds_info,
	.codename = "tigerlake",
	.gt = 1,
};

static const struct intel_device_info intel_tigerlake_gt2_info = {
	.graphics_ver = 12,
	.display_ver = 12,
	.is_tigerlake = true,
	.cmds_info = &gen12_cmds_info,
	.codename = "tigerlake",
	.gt = 2,
};

static const struct intel_device_info intel_rocketlake_info = {
	.graphics_ver = 12,
	.display_ver = 12,
	.is_rocketlake = true,
	.cmds_info = &gen12_cmds_info,
	.codename = "rocketlake"
};

static const struct intel_device_info intel_dg1_info = {
	.graphics_ver = 12,
	.graphics_rel = 10,
	.display_ver = 12,
	.is_dg1 = true,
	.cmds_info = &gen12_cmds_info,
	.codename = "dg1"
};

static const struct intel_device_info intel_dg2_info = {
	.graphics_ver = 12,
	.graphics_rel = 55,
	.display_ver = 13,
	.has_4tile = true,
	.is_dg2 = true,
	.codename = "dg2",
	.cmds_info = &gen12_dg2_cmds_info,
	.has_flatccs = true,
};

static const struct intel_device_info intel_alderlake_s_info = {
	.graphics_ver = 12,
	.display_ver = 12,
	.is_alderlake_s = true,
	.cmds_info = &gen12_cmds_info,
	.codename = "alderlake_s"
};

static const struct intel_device_info intel_raptorlake_s_info = {
	.graphics_ver = 12,
	.display_ver = 12,
	.is_raptorlake_s = true,
	.cmds_info = &gen12_cmds_info,
	.codename = "raptorlake_s"
};

static const struct intel_device_info intel_alderlake_p_info = {
	.graphics_ver = 12,
	.display_ver = 13,
	.is_alderlake_p = true,
	.cmds_info = &gen12_cmds_info,
	.codename = "alderlake_p"
};

static const struct intel_device_info intel_alderlake_n_info = {
	.graphics_ver = 12,
	.display_ver = 13,
	.is_alderlake_n = true,
	.cmds_info = &gen12_cmds_info,
	.codename = "alderlake_n"
};

static const struct intel_device_info intel_ats_m_info = {
	.graphics_ver = 12,
	.graphics_rel = 55,
	.display_ver = 0, /* no display support */
	.is_dg2 = true,
	.has_4tile = true,
	.codename = "ats_m",
	.cmds_info = &gen12_dg2_cmds_info,
	.has_flatccs = true,
};

static const struct intel_device_info intel_meteorlake_info = {
	.graphics_ver = 12,
	.graphics_rel = 70,
	.display_ver = 14,
	.has_4tile = true,
	.has_oam = true,
	.is_meteorlake = true,
	.codename = "meteorlake",
	.cmds_info = &gen12_mtl_cmds_info,
};

static const struct pci_id_match intel_device_match[] = {
	INTEL_I810_IDS(&intel_i810_info),
	INTEL_I815_IDS(&intel_i815_info),

	INTEL_I830_IDS(&intel_i830_info),
	INTEL_I845G_IDS(&intel_i845_info),
	INTEL_I85X_IDS(&intel_i855_info),
	INTEL_I865G_IDS(&intel_i865_info),

	INTEL_I915G_IDS(&intel_i915_info),
	INTEL_I915GM_IDS(&intel_i915m_info),
	INTEL_I945G_IDS(&intel_i945_info),
	INTEL_I945GM_IDS(&intel_i945m_info),

	INTEL_G33_IDS(&intel_g33_info),
	INTEL_PINEVIEW_G_IDS(&intel_pineview_g_info),
	INTEL_PINEVIEW_M_IDS(&intel_pineview_m_info),

	INTEL_I965G_IDS(&intel_i965_info),
	INTEL_I965GM_IDS(&intel_i965m_info),

	INTEL_G45_IDS(&intel_g45_info),
	INTEL_GM45_IDS(&intel_gm45_info),

	INTEL_IRONLAKE_D_IDS(&intel_ironlake_info),
	INTEL_IRONLAKE_M_IDS(&intel_ironlake_m_info),

	INTEL_SNB_D_IDS(&intel_sandybridge_info),
	INTEL_SNB_M_IDS(&intel_sandybridge_m_info),

	INTEL_IVB_D_IDS(&intel_ivybridge_info),
	INTEL_IVB_M_IDS(&intel_ivybridge_m_info),

	INTEL_HSW_GT1_IDS(&intel_haswell_gt1_info),
	INTEL_HSW_GT2_IDS(&intel_haswell_gt2_info),
	INTEL_HSW_GT3_IDS(&intel_haswell_gt3_info),

	INTEL_VLV_IDS(&intel_valleyview_info),

	INTEL_BDW_GT1_IDS(&intel_broadwell_gt1_info),
	INTEL_BDW_GT2_IDS(&intel_broadwell_gt2_info),
	INTEL_BDW_GT3_IDS(&intel_broadwell_gt3_info),
	INTEL_BDW_RSVD_IDS(&intel_broadwell_unknown_info),

	INTEL_CHV_IDS(&intel_cherryview_info),

	INTEL_SKL_GT1_IDS(&intel_skylake_gt1_info),
	INTEL_SKL_GT2_IDS(&intel_skylake_gt2_info),
	INTEL_SKL_GT3_IDS(&intel_skylake_gt3_info),
	INTEL_SKL_GT4_IDS(&intel_skylake_gt4_info),

	INTEL_BXT_IDS(&intel_broxton_info),

	INTEL_KBL_GT1_IDS(&intel_kabylake_gt1_info),
	INTEL_KBL_GT2_IDS(&intel_kabylake_gt2_info),
	INTEL_KBL_GT3_IDS(&intel_kabylake_gt3_info),
	INTEL_KBL_GT4_IDS(&intel_kabylake_gt4_info),
	INTEL_AML_KBL_GT2_IDS(&intel_kabylake_gt2_info),

	INTEL_GLK_IDS(&intel_geminilake_info),

	INTEL_CFL_S_GT1_IDS(&intel_coffeelake_gt1_info),
	INTEL_CFL_S_GT2_IDS(&intel_coffeelake_gt2_info),
	INTEL_CFL_H_GT1_IDS(&intel_coffeelake_gt1_info),
	INTEL_CFL_H_GT2_IDS(&intel_coffeelake_gt2_info),
	INTEL_CFL_U_GT2_IDS(&intel_coffeelake_gt2_info),
	INTEL_CFL_U_GT3_IDS(&intel_coffeelake_gt3_info),
	INTEL_WHL_U_GT1_IDS(&intel_coffeelake_gt1_info),
	INTEL_WHL_U_GT2_IDS(&intel_coffeelake_gt2_info),
	INTEL_WHL_U_GT3_IDS(&intel_coffeelake_gt3_info),
	INTEL_AML_CFL_GT2_IDS(&intel_coffeelake_gt2_info),

	INTEL_CML_GT1_IDS(&intel_cometlake_gt1_info),
	INTEL_CML_GT2_IDS(&intel_cometlake_gt2_info),
	INTEL_CML_U_GT1_IDS(&intel_cometlake_gt1_info),
	INTEL_CML_U_GT2_IDS(&intel_cometlake_gt2_info),

	INTEL_CNL_IDS(&intel_cannonlake_info),

	INTEL_ICL_11_IDS(&intel_icelake_info),

	INTEL_EHL_IDS(&intel_elkhartlake_info),
	INTEL_JSL_IDS(&intel_jasperlake_info),

	INTEL_TGL_12_GT1_IDS(&intel_tigerlake_gt1_info),
	INTEL_TGL_12_GT2_IDS(&intel_tigerlake_gt2_info),
	INTEL_RKL_IDS(&intel_rocketlake_info),

	INTEL_DG1_IDS(&intel_dg1_info),
	INTEL_DG2_IDS(&intel_dg2_info),

	INTEL_ADLS_IDS(&intel_alderlake_s_info),
	INTEL_RPLS_IDS(&intel_raptorlake_s_info),
	INTEL_ADLP_IDS(&intel_alderlake_p_info),
	INTEL_RPLP_IDS(&intel_alderlake_p_info),
	INTEL_ADLN_IDS(&intel_alderlake_n_info),

	INTEL_ATS_M_IDS(&intel_ats_m_info),

	INTEL_MTL_IDS(&intel_meteorlake_info),

	INTEL_VGA_DEVICE(PCI_MATCH_ANY, &intel_generic_info),
};

/**
 * intel_get_device_info:
 * @devid: pci device id
 *
 * Looks up the Intel GFX device info for the given device id.
 *
 * Returns:
 * The associated intel_get_device_info
 */
const struct intel_device_info *intel_get_device_info(uint16_t devid)
{
	static const struct intel_device_info *cache = &intel_generic_info;
	static uint16_t cached_devid;
	int i;

	if (cached_devid == devid)
		goto out;

	/* XXX Presort table and bsearch! */
	for (i = 0; intel_device_match[i].device_id != PCI_MATCH_ANY; i++) {
		if (devid == intel_device_match[i].device_id)
			break;
	}

	cached_devid = devid;
	cache = (void *)intel_device_match[i].match_data;

out:
	return cache;
}

/**
 * intel_get_cmds_info:
 * @devid: pci device id
 *
 * Looks up information on copy commands and tiling formats supported
 * by the device.
 *
 * Returns:
 * The associated intel_cmds_info, NULL if no such information is found
 */
const struct intel_cmds_info *intel_get_cmds_info(uint16_t devid)
{
	const struct intel_device_info *dev_info;

	dev_info = intel_get_device_info(devid);

	return dev_info->cmds_info;
}

/**
 * intel_gen:
 * @devid: pci device id
 *
 * Computes the Intel GFX generation for the given device id.
 *
 * Returns:
 * The GFX generation on successful lookup, -1u on failure.
 */
unsigned intel_gen(uint16_t devid)
{
	return intel_get_device_info(devid)->graphics_ver ?: -1u;
}

unsigned intel_graphics_ver(uint16_t devid)
{
	const struct intel_device_info *info = intel_get_device_info(devid);

	return IP_VER(info->graphics_ver, info->graphics_rel);
}

/**
 * intel_display_ver:
 * @devid: pci device id
 *
 * Computes the Intel GFX display version for the given device id.
 *
 * Returns:
 * The display version on successful lookup, -1u on failure.
 */
unsigned intel_display_ver(uint16_t devid)
{
	return intel_get_device_info(devid)->display_ver ?: -1u;
}
