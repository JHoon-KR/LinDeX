#include "advc/capabilities.h"

#include <assert.h>
#include <stdio.h>

int main(void) {
    assert(advc_classify_codec_component(NULL) ==
           ADVC_ACCELERATION_UNKNOWN);
    assert(advc_classify_codec_component("") ==
           ADVC_ACCELERATION_UNKNOWN);

    assert(advc_classify_codec_component("c2.qti.avc.decoder") ==
           ADVC_ACCELERATION_HARDWARE);
    assert(advc_classify_codec_component("c2.mtk.hevc.decoder") ==
           ADVC_ACCELERATION_HARDWARE);
    assert(advc_classify_codec_component("OMX.Exynos.VP9.Decoder") ==
           ADVC_ACCELERATION_HARDWARE);
    assert(advc_classify_codec_component("c2.rk.av1.decoder") ==
           ADVC_ACCELERATION_HARDWARE);

    assert(advc_classify_codec_component("c2.android.avc.decoder") ==
           ADVC_ACCELERATION_SOFTWARE);
    assert(advc_classify_codec_component("c2.google.hevc.decoder") ==
           ADVC_ACCELERATION_SOFTWARE);
    assert(advc_classify_codec_component("OMX.google.vp9.decoder") ==
           ADVC_ACCELERATION_SOFTWARE);
    assert(advc_classify_codec_component("c2.ffmpeg.av1.decoder") ==
           ADVC_ACCELERATION_SOFTWARE);

    assert(advc_classify_codec_component("c2.qti.avc.decoder.secure") ==
           ADVC_ACCELERATION_UNKNOWN);
    assert(advc_classify_codec_component("c2.qti.avc.decoder.sw") ==
           ADVC_ACCELERATION_UNKNOWN);
    assert(advc_classify_codec_component("wrapper.c2.qti.avc.decoder") ==
           ADVC_ACCELERATION_UNKNOWN);
    assert(advc_classify_codec_component("c2.future.av1.decoder") ==
           ADVC_ACCELERATION_UNKNOWN);

    puts("advc codec component discovery policy: PASS");
    return 0;
}
