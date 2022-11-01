/* SPDX-License-Identifier: GPL-2.0 */
/*
 * List of real DP EDIDs from popular monitors.
 * The current list (at the time of writing this comment) is based on the top
 * monitors used on ChromeOS.
 *
 * Copyright 2022 Google LLC.
 *
 * Authors: Mark Yacoub <markyacoub@chromium.org>
 */

#ifndef TESTS_CHAMELIUM_MONITOR_EDIDS_DP_EDIDS_H_
#define TESTS_CHAMELIUM_MONITOR_EDIDS_DP_EDIDS_H_

#include "monitor_edids_helper.h"

// TODO: Add more EDIDs.
monitor_edid DP_EDIDS[] = { { .name = "4K_DELL_UP3216Q_DP",
			      .edid = "00ffffffffffff0010acf84050383230"
				      "051a0104a5431c783aca95a6554ea126"
				      "0f5054a54b808100b300714f8180d1c0"
				      "0101010101017e4800e0a0381f404040"
				      "3a00a11c2100001a000000ff00393143"
				      "5937333538303238500a000000fc0044"
				      "454c4c205532393137570a20000000fd"
				      "00314c1d5e13010a2020202020200117"
				      "02031df1501005040302071601061112"
				      "1513141f2023091f0783010000023a80"
				      "1871382d40582c2500a11c2100001e01"
				      "1d8018711c1620582c2500a11c210000"
				      "9e011d007251d01e206e285500a11c21"
				      "00001e8c0ad08a20e02d10103e9600a1"
				      "1c210000180000000000000000000000"
				      "000000000000000000000000000000dd" },

			    { .name = "DEL_16543_DELL_P2314T_DP",
			      .edid = "00ffffffffffff0010ac9f404c4c3645"
				      "10180104a5331d783ae595a656529d27"
				      "105054a54b00714f8180a9c0d1c00101"
				      "010101010101023a801871382d40582c"
				      "4500fd1e1100001e000000ff00445746"
				      "325834344645364c4c0a000000fc0044"
				      "454c4c205032333134540a20000000fd"
				      "00384c1e5311010a20202020202001bb"
				      "02031cf14f9005040302071601061112"
				      "1513141f2309070783010000023a8018"
				      "71382d40582c4500fd1e1100001e011d"
				      "8018711c1620582c2500fd1e1100009e"
				      "011d007251d01e206e285500fd1e1100"
				      "001e8c0ad08a20e02d10103e9600fd1e"
				      "11000018000000000000000000000000"
				      "0000000000000000000000000000003f" } };

#endif /* TESTS_CHAMELIUM_MONITOR_EDIDS_DP_EDIDS_H_ */