# 문제 해결

[English](../TROUBLESHOOTING.md) | [한국어](TROUBLESHOOTING.md)

모든 복구는 DP를 물리적으로 분리한 상태에서 시작합니다. 범위가 제한된 진단이
필요할 때만 dev 빌드를 사용하세요. Release 모드는 의도적으로 영구 진단 로그를
보존하지 않습니다.

## 업데이트 후 WebUI가 열리지 않음

1. KernelSU 설치 관리자가 성공을 보고했는지 확인합니다.
2. 모듈 설치 후 Android를 일반 재부팅했는지 확인합니다.
3. DP를 분리한 상태에서 모듈 WebUI를 다시 엽니다.
4. 모듈이 비활성화되었거나 설치 검사가 실패했다면 터미널에서 바이너리를 강제
   시작하지 말고 정확한 검증 ZIP을 다시 설치합니다.

## KernelSU 설치기가 `illegal mode: 022` 또는 `\r`을 표시함

모듈 스크립트와 release ZIP은 LF 줄바꿈을 사용합니다. 이 오류는 Windows에서
빌드한 사용자 정의 `ksud`에 내장된 `installer.sh`가 CRLF 상태로 컴파일됐을 때도
발생할 수 있습니다. LF checkout에서 KernelSU userspace를 업데이트하거나 다시
빌드하세요. 우회 목적으로 Android 읽기 전용 파티션을 수정하거나 LinDeX에
`system/` overlay를 추가하지 마세요. 기준 기기 검수에서는 동일 `ksud`의 임시
복사본에서 내장 설치기 텍스트만 LF로 정규화했으며 설치된 KernelSU 바이너리는
수정하지 않았습니다.

## 프로필 설정 실패

먼저 짧은 상태를 읽습니다.

- `dependency-required`: 서명된 Debian 의존성을 사용할 수 없음.
- `profile-unavailable`: 필요한 bundled profile 자산이 없거나 checksum/source
  lock 검증 실패.
- `dependency-install-failed`: 패키지 또는 압축 파일 설치 실패.
- `missing-pywal-command`: Sway Pywal 선택에 필요한 고정 `wal` provider를
  사용할 수 없음. Dark/Light를 선택하거나 검증된 provider 입력을 복원.
- checksum 또는 브리지 오류: 우회하지 말고 릴리스 입력을 교체.

네트워크/패키지 가용성 또는 릴리스 산출물을 수정한 뒤에만 재시도합니다.
Archcraft Sway Free는 정확한 검증 LinDeX 릴리스를 재설치해 공식 공개 자산
bundle을 복원합니다. 런타임 오류를 해결하려고 비공식 바이너리나 비공개/유료
Ko-fi archive를 사용하지 않습니다. 안내된 Ko-fi 페이지는 Archcraft 제작자에게
선택적으로 후원하기 위한 링크이며 복구 또는 설치 소스가 아닙니다.

## DP가 연결되었지만 모니터가 준비되지 않음

- 휴대폰, 케이블, 어댑터, 모니터가 USB-C DP Alt Mode를 지원하는지 확인합니다.
- WebUI에서 물리 connector 연결과 유효한 EDID를 기다립니다.
- EDID가 광고한 모드로 돌아갑니다. 수동 고주사율 모드는 실험적입니다.
- 세션을 중지하고 DP를 분리한 뒤 다시 연결합니다. 새 세션과 lease가 만들어져야
  합니다.

## 시작 후 모니터가 검음

1. DP를 분리하고 WebUI에서 **중지**를 누릅니다.
2. 직접 스캔아웃을 호환용 `off`로 끄고 EDID 광고 모드에서 재시도합니다.
3. Dev 결과가 특정 backend 실패를 확인하지 않았다면 Vulkan/GLES 선택을 live
   probe 기본값으로 유지합니다.
4. 해결책으로 과거 컴포지터 패치를 설치하거나 Android 파티션을 다시 마운트하지
   않습니다.

Dev 로그에 `Could not match drm and vulkan device`가 있으면 immutable runtime에
`libandroid-vulkan-drm-identity-layer.so`와 explicit-layer JSON manifest가
있고 runtime checksum 검증이 통과했는지
확인합니다. ACK를 수동으로 설정하지 마세요. Stock 세션이 hardware Turnip
probe 뒤에만 소유하며 frontend는
[Vulkan DRM identity 브리지](VULKAN_DRM_IDENTITY.md)에 기록된 정확한 device
identity를 계속 요구합니다. 거부 결과는 DRM node를 추측해 우회하지 말고 기존
GLES2 폴백으로 이어져야 합니다.

## USB 입력이 Android로 돌아오지 않음

DP를 분리하고 정확한 LinDeX 세션을 중지합니다. 마지막 소유 디스크립터가 커널
grab을 해제해야 합니다. Global 입력 서비스를 종료하지 마세요. 반복되면 dev
빌드로 재현하고 WebUI가 `shared` 또는 `linux-exclusive` 중 어느 모드였는지와
영향받은 USB event 유형을 기록합니다.

## Sway 세션 후 Android에서 Wi-Fi를 다시 켤 수 없음

LinDeX는 Sway Waybar 네트워크 모듈과 `rofi_network` 도우미를 제거합니다.
chroot에서 `wlan*`이나 라우팅 상태를 읽지 않으며 NetworkManager,
`nm-applet`, supplicant 또는 Wi-Fi/rfkill 메뉴를 시작하지 않습니다.
먼저 제어권 충돌과 vendor driver 실패를 구분합니다.

- Android 설정의 `wifi_on=1`과 `cmd wifi status`의 disabled가 동시에 나타나면
  Android가 요청은 접수했지만 Wi-Fi 시작을 완료하지 못한 것입니다.
- `numSetupClientInterfaceFailureDueToHal`만 증가하고 wificond와 supplicant 실패
  횟수가 0이면 HAL 경계의 실패입니다.
- 커널 로그의 `CNSS` idle-restart timeout, `is_driver_recovering 1`, 또는
  `Failed to start WLAN modules`는 chroot의 제어권 점유가 아니라 Qualcomm
  firmware/driver 고착을 뜻합니다.

`/sys/kernel/cnss`에 추측한 값을 쓰거나 vendor module을 내리거나 LinDeX에서
Wi-Fi 서비스를 자동 재시작하지 않습니다. 제한된 dev 보고서를 수집하고 세션을
종료한 뒤 명시적으로 허용한 Android Wi-Fi stack 복구 또는 재부팅을 사용합니다.
Release 프로필에는 활성 네트워크 위젯, `nm-applet` 시작 또는
`rofi_network` 도우미가 없어야 합니다.

## 비디오 가속을 사용할 수 없음

이는 올바른 fail-closed 결과일 수 있습니다. 엄격한 zero-copy 선택은 정확한 live
backend probe가 통과한 뒤에만 나타납니다. 디코드 PRIME은 공개 검증 매트릭스를
완료할 때까지 의도적으로 비활성화됩니다. Capability bit를 강제하거나 byte/AHB
증거를 PRIME 검증으로 취급하지 않습니다.

FFmpeg와 `vainfo`에는 ADVC가 보이지만 GStreamer에 `vaapih264enc` 또는
`vaapih265enc`가 없다면 plugin-scanner process를 종료하지 말고 새 LinDeX
세션을 시작합니다. 검증된 세션은 transient task-owned registry를 사용하며 이
Android/chroot 조합에서 forked plugin
scanner가 VA-API feature reply를 잃는 문제를 피하려고 성공한 ADVC gate 내부에서
`GST_VAAPI_ALL_DRIVERS=1`과 `GST_REGISTRY_FORK=no`를 설정합니다. 실패한 ADVC
preflight를 성공처럼 보이게 만들 목적으로 이 변수를 전역 export하지 않습니다.

Dev checkout에서 opt-in 방식으로 H.264 디코드 경계를 제한된 시간 동안 비교하려면
이미 검증된 LinDeX 세션 안에서 다음을 실행합니다.

```sh
./scripts/diagnose-gstreamer-h264-decode.sh --timeout 30 --output-dir /tmp/lindex-gst-h264-report /path/to/sample.h264
```

도구는 동일 입력을 `avdec_h264`와 `vaapih264dec`로 각각 실행하고 scanner fork를
비활성화한 자체 transient registry를 사용한 뒤 종료할 때 registry를 삭제합니다.
Raw 로그와 `summary.txt`에는 parser EOS, decoder finish-frame, downstream buffer,
downstream EOS, bus EOS의 마지막 관찰 경계가 보수적으로 분류됩니다. 이 분류는
마지막으로 보인 증거이며 특정 함수에서 정지했다는 증명은 아니므로 raw 로그도
보존하세요. 이 명령은 plugin을 설치하거나 실패한 ADVC gate를 강제로 켜거나 ADB로
Android에 접속하거나 plugin scanner를 종료하지 않습니다. Dev 세션에
`h264parse`, `avdec_h264`, `vaapih264dec`가 미리 있어야 합니다.

Bus EOS message가 있는데도 제한 시간 안에 process가 끝나지 않으면 summary는
`post-bus-eos-process-timeout`으로 분류합니다. 이는 증상 경계일 뿐 MediaCodec
teardown의 증거가 아닙니다. Codec stop trace뿐 아니라 process poll FD가 잘못
닫혔는지도 확인하십시오. LinDeX에서는 dma-buf cleanup이 미선언 FD 0을 닫던
사례를 수정했습니다. 통과하려면 제한 시간 내 process 종료, destroy drain
비활성, 폐기 output 0, codec stop status 0을 모두 만족해야 합니다.

## 보고서에 포함할 내용

- LinDeX 버전, 알면 Git commit, ZIP SHA-256, 빌드 flavor
- Serial을 제외한 기기 모델, Android 버전, GPU 계열, 독/어댑터 모델
- 선택한 프로필과 WebUI 설정
- 정확한 시작/중지/연결 순서
- 가장 작은 정리된 dev 로그 일부 또는 WebUI 오류

기기 serial, 인증 token, cookie, 전체 Android 로그, 유료 제3자 파일을 포함하지
않습니다. 보안 관련 문제는 [SECURITY.ko.md](../../SECURITY.ko.md)를 따릅니다.
