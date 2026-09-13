// SPDX-License-Identifier: LGPL-2.1-or-later
#include <cstdio>
#include <cstdlib>
#include <cstdint>

#include "bc_dts_types.h"
#include "libcrystalhd_if.h"

static bool success(const char *name, BC_STATUS status)
{
	if (status == BC_STS_SUCCESS)
		return true;
	std::fprintf(stderr, "%zu-bit: %s failed with status %d\n",
		     sizeof(void *) * 8, name, status);
	return false;
}

int main()
{
	BC_HW_CAPS caps = {};
	HANDLE device = nullptr;
	uint32_t driver = 0, library = 0;
	const uint32_t mode = DTS_PLAYBACK_MODE | DTS_LOAD_FILE_PLAY_FW |
		DTS_SKIP_TX_CHK_CPB | DTS_DFLT_RESOLUTION(vdecRESOLUTION_720p29_97);

	if (!success("capabilities before open", DtsGetCapabilities(nullptr, &caps)) ||
	    !(caps.DecCaps & BC_DEC_FLAGS_H264))
		return EXIT_FAILURE;
	if (!success("firmware open", DtsDeviceOpen(&device, mode)))
		return EXIT_FAILURE;
	bool ok = success("version", DtsGetVersion(device, &driver, &library));
	ok = success("capabilities after open", DtsGetCapabilities(device, &caps)) && ok;
	ok = success("UYVY color mode", DtsSetColorSpace(device, OUTPUT_MODE422_UYVY)) && ok;
	ok = success("YUY2 color mode", DtsSetColorSpace(device, OUTPUT_MODE422_YUY2)) && ok;
	ok = success("close", DtsDeviceClose(device)) && ok;
	if (!ok)
		return EXIT_FAILURE;
	std::printf("%zu-bit: library firmware open/close PASS (driver %#x, library %#x)\n",
		    sizeof(void *) * 8, driver, library);
	return EXIT_SUCCESS;
}
