# Firefox RDD 디코드 게이트웨이

LinDeX는 Firefox RDD seccomp 샌드박스를 유지합니다. 배포 경로는 RDD에 KGSL
ioctl을 허용하지 않으며 `MOZ_DISABLE_RDD_SANDBOX`도 설정하지 않습니다.

## 런타임 경계

배포 경로는 다음과 같습니다.

```text
Firefox RDD(샌드박스 활성)
  -> 정확히 선연결된 SOCK_SEQPACKET 소켓
  -> LinDeX repack gateway(Turnip/KGSL 소유)
  -> Android ADVC broker
  -> Android c2.qti 디코더
  -> QCOM/UBWC NV12 AHB + source fence
  -> 게이트웨이에서 Vulkan GPU repack 1회
  -> 명시적 modifier-0 LINEAR NV12 dma-buf + destination fence
  -> Firefox RDD
```

RDD가 읽는 VA 드라이버는 Vulkan/Turnip 연결 없이 빌드됩니다. 따라서 게이트웨이가
보낸 명시적 LINEAR descriptor만 받고, 프로세스 내부 repack hook으로 KGSL을
초기화할 수 없습니다. 두 preload 어댑터는 정확한 프로세스에서만 켜집니다.

- 소켓 어댑터는 Firefox `rdd` content process, 정확한
  `/run/android-drm/` 게이트웨이 소켓, 버전 ACK가 모두 일치할 때만 켜집니다.
- EGL identity 어댑터는 Firefox `glxtest`, 정확한 KGSL/DRM 노드, 버전 ACK가
  모두 일치할 때만 켜집니다.

그 밖의 프로세스는 원래 동작을 그대로 사용합니다.

코덱 어댑터는 컴포지터 전체 `LD_PRELOAD`에 포함되지 않습니다. 세션은 정확한
불변 경로를 `LINDEX_FIREFOX_PRELOAD`로 내보내고,
`/usr/local/bin/firefox`와 `firefox-esr`은 패키지된 `lindex-firefox`
실행기를 가리킵니다. 실행기는 두 파일이 symlink가 아닌 일반 파일인지 확인한
뒤 브라우저 preload 집합을 교체합니다. 따라서 Firefox `dlsym` interposer는
Sway/labwc 및 그 Vulkan loader에 들어가지 않으면서 브라우저 트리 내부에서는
위의 프로세스 gate를 유지합니다.

## 게이트웨이 규칙

게이트웨이는 root peer만 받고 client/output 수를 제한하며 기존 ADVC protocol을
전달합니다. 디코드 AHardwareBuffer가 나오면 broker 소유 PRIME descriptor를
게이트웨이가 직접 얻습니다. RDD가 제공하는 QCOM layout은 신뢰하지 않습니다.

- QCOM 압축 NV12는 Turnip으로 한 번 repack한 뒤 명시적 modifier `0` LINEAR
  NV12로 내보냅니다.
- 이미 LINEAR인 NV12는 다시 복사하지 않습니다.
- 지원하지 않는 형식은 fail-closed 처리하며 QCOM을 LINEAR라고 속이지 않습니다.
- source release fence는 Android로, destination acquire fence는 Firefox로 갑니다.
- parent-death signal을 사용하므로 compositor/session 종료나 강제 DP 정리 뒤에
  게이트웨이가 고아 프로세스로 남지 않습니다.

인코드 byte/dma-buf 연산은 같은 protocol로 계속 전달됩니다. Firefox 전용
게이트웨이는 AHB encode queue/transfer 연산을 의도적으로 노출하지 않습니다.

## 릴리스와 개발 동작

릴리스는 gateway trace를 끄고 codec log를 누적하지 않습니다. 개발 모드는
정확한 `ANDROID_DRM_CODEC_TRACE=1` gate로 제한된 진단을 켤 수 있습니다. 불변
codec manifest, Android broker, gateway, socket 확인이 모두 끝난 뒤에만 VA-API와
GStreamer 환경을 앱에 광고합니다.

## 실기 합격 조건

다음 항목을 정확한 배포 ZIP으로 통과하기 전까지 이 경로는 fail-closed 후보입니다.

1. RDD의 seccomp mode가 2이고 `MOZ_DISABLE_RDD_SANDBOX` 환경이 없어야 합니다.
2. RDD가 `/dev/kgsl-3d0` FD를 가지지 않고 KGSL seccomp 위반도 없어야 합니다.
3. Android가 하드웨어 `c2.qti` 디코더를 선택해야 합니다.
4. QCOM 디코드 프레임마다 gateway repack 1회, source release fence 1개,
   LINEAR destination acquire fence 1개가 대응해야 합니다.
5. Firefox에는 modifier-0 LINEAR만 도달하고 `EGL_BAD_MATCH`와 software decode
   fallback이 없어야 합니다.
6. 재생, seek, drain, tab 종료, RDD 종료, gateway 종료, 강제 세션 정리 뒤에
   codec output, dma-buf, socket, helper 고아가 없어야 합니다.

호스트 artifact·manifest·패키지 검증은 완료했습니다. 최종 설치 ZIP의 실기 결과를
기록하기 전에는 브라우저 하드웨어 디코드가 완성됐다고 표현하지 않습니다.

2026-08-28 설치된 v3.0.3-dev 검증은 패키지의 일반 production socket, gateway,
broker 및 hardware decode 경로를 사용했으며 실험용 Android low-latency switch는
필요하지 않았습니다. Firefox는 seek 30회와 document reload 10회를 완료하고
decoded 766, dropped 5 frame을 기록했습니다. Driver trace는 hardware output
1,757회, LINEAR export 성공 1,758회, export timeout 0회, 비동기 실패 0회를
기록했습니다. Android media metrics는 `c2.qti.avc.decoder`와 low-latency mode
off를 확인했습니다. RDD는 seccomp mode 2를 유지하고 패키지 VA driver와 프로세스
한정 어댑터 2개를 mapping한 상태에서 KGSL FD를 하나도 소유하지 않았습니다.
활성 session 종료 뒤 Firefox, gateway, Sway, session-runner 고아는 없었고 다음
Sway session도 정상 시작했습니다. 따라서 설치 개발 ZIP의 첫 B-frame/reorder
정체와 lifecycle 검증은 해결됐습니다. 공개 release ZIP 반복과 gateway의 정확한
프레임별 fence 계수 감사는 아직 남은 합격 항목입니다.

2026-08-27 staged-source 통합 검사에서는 새 프로세스 경계를 확인했습니다.
비디오 가속을 `auto`로 두고 gateway가 실행 중인 상태에서도 수정하지 않은
Sway/wlroots가 제한된 세션 동안 `WLR_RENDERER=vulkan`을 유지했고 GLES2
재시도에 들어가지 않았습니다. 직접 실행한 `vainfo`는 VA-API 1.22,
`advc_drv_video.so`, `__vaDriverInit_1_0`까지 확인했지만 capability query를
완료하지 못했습니다. 해당 probe는 gateway 또는 세션 고아 없이 종료했습니다.
이는 컴포지터 격리 증거일 뿐이며 위 Firefox 화면 재생 합격 조건을 완료한
결과는 아닙니다.
