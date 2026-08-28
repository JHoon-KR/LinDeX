# USB 입력 라우팅

[English](../USB_INPUT_ROUTING.md) | [한국어](USB_INPUT_ROUTING.md)

## 목표

외부 모니터 Debian 세션이 활성화된 동안 LinDeX는 USB 독의 키보드, 마우스,
간접 터치패드를 해당 세션 전용으로 만들 수 있습니다. 저장장치, 오디오, 네트워크,
시리얼, 캡처 기기 등 관련 없는 USB 기능은 Android와 Debian이 계속 공유합니다.
휴대폰 내장 터치스크린은 이 USB 모드로 grab하지 않습니다.

## WebUI 모드

- `linux-exclusive`(독 데스크톱에 권장): 대상 USB 입력 event node를 활성
  모니터 세션 동안에만 grab합니다.
- `shared`: Android와 Debian이 모두 이벤트를 받을 수 있습니다.
- **휴대폰 터치 공유**는 별도 선택이며 기본값은 꺼짐입니다.

## 소유권 모델

입력 helper는 소유한 컴포지터/seat 프로세스에만 로드됩니다. 경로명을 믿는 대신
열린 파일 디스크립터를 다음과 같이 분류합니다.

1. Linux input character device여야 합니다.
2. udev 또는 sysfs 상위 경로가 USB bus 기기임을 보여야 합니다.
3. event capability가 키보드, relative mouse, 간접 touchpad를 나타내야 합니다.
4. direct touchscreen과 관련 없는 event node는 제외합니다.

Helper는 그 정확한 디스크립터에 `EVIOCGRAB`을 적용합니다. 컴포지터가 끝나고
마지막 복제 디스크립터가 닫히면 커널이 grab을 해제합니다. LinDeX는 이 수명주기를
위해 global 입력 daemon, 광범위한 PID 검색, Android 재부팅, 영구 소유권 기록을
사용하지 않습니다.

Grab이 거부되면 데스크톱이 복구 가능하도록 디스크립터를 열어 두고 입력을 공유
상태로 유지합니다. Dev 진단은 크기가 제한된 세션 스트림으로 보내며 release
모드는 입력 로그 파일을 만들지 않습니다.

## 안전 경계

- Helper를 Debian 환경 전체에 전역 preload하지 않습니다.
- 공유 Android 또는 global seat daemon에 붙이지 않습니다.
- Bluetooth 입력은 USB 전용 분류 범위 밖입니다.
- USB 터치스크린을 간접 터치패드로 취급하지 않습니다.
- 기기 열거와 USB 전원은 계속 공유되며, 이 기능은 event 전달만 제어합니다.

## 최종 독 매트릭스

실제 독에서 패키지 모듈이 다음 검사를 모두 통과하기 전까지 이 기능은 최종
기기 매트릭스 pending 상태입니다.

1. shared 모드가 두 환경에 예상 이벤트를 전달한다.
2. exclusive 모드가 키보드, 마우스, 간접 터치패드를 Debian에만 유지한다.
3. 저장장치, 오디오, 네트워크 및 다른 USB 기능이 계속 동작한다.
4. 휴대폰 터치스크린이 계속 Android를 제어한다.
5. 정상 세션 중지가 즉시 USB 입력을 Android에 돌려준다.
6. 강제 DP 분리도 소유한 디스크립터를 해제한다.
7. 재연결이 stale grab 없이 새 세션을 만든다.

결과는 [검증 상태](VALIDATION_STATUS.md)에 기록합니다. 호스트 테스트만으로
일반적인 독 검증을 주장하지 않습니다.
