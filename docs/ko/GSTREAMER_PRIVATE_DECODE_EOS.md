# GStreamer private H.264 decode EOS 연동

실제 `vaapih264dec` EOS 패치는 현재 **차단 상태이며 설치하면 안 됩니다**.
LinDeX 릴리스 ZIP은 이 upstream 패치를 포함하거나 설치하지 않으며, 현재
기기 검증은 모두 Debian의 수정되지 않은 GStreamer 패키지로 수행했습니다.
저장된 GStreamer 패치는 비활성 선행 패치일 뿐입니다. decoder의 VA handle
접근자와 surface가 pool에 반환되기 직전 실행되는 fail-closed callback만
추가하며, callback을 쓰지 않으면 stock 동작은 그대로입니다.

차단 원인은 retry 횟수가 아니라 구조적 계약 부족입니다. Private
decode-EOS ABI 1.0은 EOS를 signal하고 output 하나를 progress할 수 있지만,
준비된 `VASurfaceID`를 status로 돌려주지 않습니다. 또한 downstream이 마지막
buffer를 놓았을 때 direct broker output lease를 surface 단위로 해제할 API가
없습니다. Upstream gstreamer-vaapi 1.26.2는 decode surface를 pool에 보관하므로
buffer 해제가 VA surface 파괴로 이어지지 않습니다. EOS 뒤에는 lease를
해제할 다음 `vaBeginPicture()`도 없을 수 있습니다. software DPB 순서와
MediaCodec output 순서가 같다고 추측하거나 broker 8-slot 창을 한꺼번에
drain하는 방식은 안전하지 않습니다.

차단 해제에 필요한 최소 조건은 `progress(1)`이 정확한 ready surface ID를
반환하고, bounded·idempotent per-surface release와 release-fence 계약을 제공하는
새 private ABI입니다. 향후 downstream 경로는 H.264에만 적용하고,
`vaGetLibFunc()`로 versioned getter를 찾으며, discovery/gate 실패 시 기존
finish 경로를 그대로 사용해야 합니다. Private 경로에서는 software DPB tail을
push하기 전에 EOS를 signal한 뒤, progress 하나와 확인된 frame push 하나,
해당 frame의 pool 반환 전 release 하나를 교차 실행해야 합니다. last-frame
EOS와 별도 control EOS를 모두 허용하되, private signal 이후 mismatch, 오류,
해제 가능한 tracked frame이 없는 output-window 압력, timeout은 모두
fail-closed 처리해야 합니다. 이미 downstream으로 보낸 frame의 pool 반환 전
callback이 lease를 해제한 경우에만 bounded `NEED_OUTPUT_RELEASE` retry 한 번을
깨울 수 있으며, 이는 blocking drain 허용이 아닙니다.

기존의 별도 teardown 가설은 실측으로 기각되었습니다. 명시적 VASurface
진단에서 모든 sink가 EOS를 게시했지만 descriptor cleanup이 선언되지 않은
0 값 object slot까지 닫아 GStreamer가 bus/poll wakeup에 사용하던 FD 0을
닫았습니다. Cleanup을 `object_count`가 선언한 고유 FD로 제한하자 destroy
drain 비활성, codec output 폐기 0, codec stop status 0 상태에서 bus EOS와
프로세스 종료가 모두 통과했습니다. Bus EOS 뒤 process timeout은 이제
`post-bus-eos-process-timeout`으로 중립 분류하며 MediaCodec teardown의 증거로
보지 않습니다.

이 수정이 위 private ABI 한계를 없애지는 않습니다. 명시적 old-VAAPI H.264
VASurface/fakesink EOS lifecycle만 증명하며 playbin, decodebin, seek, 화면 표시,
자동 decoder 선택은 증명하지 않습니다.

전체 증명, source hash, 패치, 검증 절차는
[`patches/gstreamer/README.md`](../../patches/gstreamer/README.md)에 있습니다.

변경되지 않은 upstream 1.26.2 source tree로 검증합니다.

```sh
sh scripts/test-gstreamer-private-decode-eos-downstream.sh \
  /path/to/gstreamer-vaapi-1.26.2
```

필요한 개발 dependency가 있는 host에서만 `--full-build`를 추가하십시오.
검증은 임시 directory에서 build하며 system plugin을 교체하지 않습니다.
