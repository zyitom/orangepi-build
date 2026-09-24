#include <stdio.h>
#include <stddef.h>
#include "isp_tuning_priv.h"
#define T(f) (offsetof(struct isp_param_config, isp_tunning_settings) + offsetof(struct isp_tunning_param, f))
#define A(f) (offsetof(struct isp_param_config, isp_3a_settings) + offsetof(struct isp_3a_param, f))
#define E(f) (offsetof(struct isp_param_config, isp_test_settings) + offsetof(struct isp_test_param, f))
int main(void){
 printf("sizeof(isp_param_config) %zu\n", sizeof(struct isp_param_config));
 printf("tune base %zu  3a base %zu  iso base %zu\n", offsetof(struct isp_param_config,isp_tunning_settings), offsetof(struct isp_param_config,isp_3a_settings), offsetof(struct isp_param_config,isp_iso_settings));
 printf("LSC_MODE %zu (script 3124)\nLSC_TBL %zu (script 3130)\nLSC_TRIG %zu (script 21562)\n", T(lsc_mode), T(lsc_tbl), T(lsc_trig_cfg));
 printf("MSC_MODE %zu (21574)\nMSC_BLW %zu (21576)\nMSC_BLH %zu (21598)\nMSC_TRIG %zu (21620)\nMSC_TBL %zu (21632)\n", T(msc_mode), T(msc_blw_lut), T(msc_blh_lut), T(msc_trig_cfg), T(msc_tbl));
 printf("bayer_gain %zu  ccm %zu  gamma_tbl %zu\n", T(bayer_gain), T(color_matrix_ini), T(gamma_tbl_ini));
 printf("test.lsc_en %zu msc_en %zu ae_en %zu awb_en %zu\n", E(lsc_en), E(msc_en), E(ae_en), E(awb_en));
 return 0; }
