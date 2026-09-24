/* Compute LSC/MSC table offsets in the isp_param_config blob.
 * Cross-check against offs.txt ground truth (from aarch64):
 *   o_tune_bayer_gain 260, o_tune_lsc_trig 18714,
 *   o_tune_gamma_tbl 53632, o_tune_gamma_trig 84352, o_tune_ccm 84362,
 *   sz_all 119204
 * Build: cc -I <kernel>/bsp/drivers/vin/vin-isp lsc_offs.c -o lsc_offs
 */
#include <stdio.h>
#include <stddef.h>
#include "isp_tuning_priv.h"

int main(void)
{
	printf("sz_test      %zu (expect 124)\n", sizeof(struct isp_test_param));
	printf("sz_3a        %zu (expect 5760)\n", sizeof(struct isp_3a_param));
	printf("sz_iso       %zu (expect 13936)\n", sizeof(struct isp_dynamic_param));
	printf("sz_tune      %zu\n", sizeof(struct isp_tunning_param));
	printf("sz_all       %zu (expect 119204)\n", sizeof(struct isp_param_config));
	printf("bayer_gain   %zu (expect 260)\n", offsetof(struct isp_tunning_param, bayer_gain));
	printf("lsc_mode     %zu\n", offsetof(struct isp_tunning_param, lsc_mode));
	printf("lsc_center_x %zu\n", offsetof(struct isp_tunning_param, lsc_center_x));
	printf("lsc_tbl      %zu  size %zu = U16[%d][%d]  (282 + 18432 = 18714)\n",
		offsetof(struct isp_tunning_param, lsc_tbl),
		sizeof(((struct isp_tunning_param *)0)->lsc_tbl),
		ISP_LSC_TEMP_NUM + ISP_LSC_TEMP_NUM, ISP_LSC_TBL_LENGTH);
	printf("lsc_trig     %zu (expect 18714)\n", offsetof(struct isp_tunning_param, lsc_trig_cfg));
	printf("msc_mode     %zu\n", offsetof(struct isp_tunning_param, msc_mode));
	printf("msc_blw_lut  %zu\n", offsetof(struct isp_tunning_param, msc_blw_lut));
	printf("msc_tbl      %zu  size %zu = U16[%d][%d]  484 = 22x22 mesh x3ch\n",
		offsetof(struct isp_tunning_param, msc_tbl),
		sizeof(((struct isp_tunning_param *)0)->msc_tbl),
		ISP_MSC_TEMP_NUM + ISP_MSC_TEMP_NUM, ISP_MSC_TBL_LENGTH);
	printf("gamma_tbl    %zu (expect 53632)\n", offsetof(struct isp_tunning_param, gamma_tbl_ini));
	printf("gamma_trig   %zu (expect 84352)\n", offsetof(struct isp_tunning_param, gamma_trig_cfg));
	printf("ccm          %zu (expect 84362)\n", offsetof(struct isp_tunning_param, color_matrix_ini));
	printf("ccm_trig     %zu (expect 84434)\n", offsetof(struct isp_tunning_param, ccm_trig_cfg));
	printf("LSC constants: TEMP_NUM %d TBL_LENGTH %d (3x256 radial x3ch)\n",
		ISP_LSC_TEMP_NUM, ISP_LSC_TBL_LENGTH);
	printf("MSC constants: TEMP_NUM %d TBL_LENGTH %d (3x484, 484=22x22) LUT %d\n",
		ISP_MSC_TEMP_NUM, ISP_MSC_TBL_LENGTH, ISP_MSC_TBL_LUT_SIZE);
	return 0;
}
