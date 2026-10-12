// Tube3DS - an unofficial YouTube client for the Nintendo 3DS.

#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>
#include <malloc.h>

#include "library.hpp"
#include "media/audio_out.hpp"
#include "net/http.hpp"
#include "tasks.hpp"
#include "thumbnails.hpp"
#include "ui/app.hpp"
#include "ui/gfx.hpp"
#include "util.hpp"
#include "yt/innertube.hpp"

static constexpr u32 kSocBufferSize = 0x100000;

static std::string systemLanguage() {
	u8 lang = CFG_LANGUAGE_EN;
	if (R_FAILED(CFGU_GetSystemLanguage(&lang))) return "en";
	switch (lang) {
	case CFG_LANGUAGE_JP: return "ja";
	case CFG_LANGUAGE_FR: return "fr";
	case CFG_LANGUAGE_DE: return "de";
	case CFG_LANGUAGE_IT: return "it";
	case CFG_LANGUAGE_ES: return "es";
	case CFG_LANGUAGE_ZH: return "zh-CN";
	case CFG_LANGUAGE_KO: return "ko";
	case CFG_LANGUAGE_NL: return "nl";
	case CFG_LANGUAGE_PT: return "pt";
	case CFG_LANGUAGE_RU: return "ru";
	case CFG_LANGUAGE_TW: return "zh-TW";
	default: return "en";
	}
}

int main(int argc, char** argv) {
	romfsInit();
	osSetSpeedupEnable(true);  // 804 MHz + L2 cache on New 3DS
	gfxInitDefault();
	gfxSet3D(false);
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	C2D_Init(C2D_DEFAULT_MAX_OBJECTS * 2);
	C2D_Prepare();
	cfguInit();
	ptmuInit();
	sslcInit(0);  // mbedtls draws entropy from the SSL service

	u32* socBuf = (u32*)memalign(0x1000, kSocBufferSize);
	bool netOk = socBuf && R_SUCCEEDED(socInit(socBuf, kSocBufferSize));

	g_audioAvailable = R_SUCCEEDED(ndspInit());
	if (g_audioAvailable) ndspSetOutputMode(NDSP_OUTPUT_STEREO);

	// Let threads use the system core (core 1); the audio decoder lives there.
	APT_SetAppCpuTimeLimit(30);

	util::logf("Tube3DS starting (%s 3DS, audio %s, net %s)", util::isNew3DS() ? "New" : "Old",
	           g_audioAvailable ? "ok" : "unavailable", netOk ? "ok" : "failed");

	g_lib.load();
	net::globalInit();
	net::setVerifyTls(g_lib.settings.verifyTls);
	yt::configure(g_lib.config, systemLanguage(), g_lib.settings.region);
	tasks::start(2);
	g_thumbs.start();
	ui::init();

	{
		ui::App app;
		std::string date;
		if (!netOk) ui::toast("Network unavailable - is Wi-Fi on?");
		else if (net::clockLooksWrong(date) && g_lib.settings.verifyTls)
			ui::toast("3DS clock is wrong (" + date + ") - fix it in System Settings", 10000);
		else if (!g_audioAvailable) ui::toast("No audio: dump your DSP firmware (DSP1)");
		app.run();
	}

	g_lib.saveIfDirty(true);
	g_thumbs.stop();
	tasks::stop();
	ui::exit();
	net::globalExit();
	if (g_audioAvailable) ndspExit();
	if (netOk) socExit();
	sslcExit();
	ptmuExit();
	cfguExit();
	C2D_Fini();
	C3D_Fini();
	gfxExit();
	romfsExit();
	return 0;
}
