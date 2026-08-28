# LinDeX 기여 안내

[English](CONTRIBUTING.md) | [한국어](CONTRIBUTING.ko.md)

LinDeX는 수정하지 않은 컴포지터용 브리지, 모듈, WebUI, 입력 라우팅, 비디오
브리지, 테스트, 공개 문서에 집중된 변경을 받습니다.

## 규칙

- av2xn의 MIT 고지와 모든 제3자 고지를 보존합니다.
- 유료 프로필 자산, 인증 cookie, 기기 serial, ADB 로그, rootfs 압축 파일,
  다운로드한 Mesa 압축 파일을 Git에 추가하지 않습니다.
- Android `/system`, `/vendor`, `/product`에 쓰지 않습니다.
- v3 릴리스 경로에 컴포지터 버전별 패치를 추가하지 않습니다.
- 지원되지 않는 capability는 fail-closed로 유지합니다. Feature bit에는 해당
  backend의 정확한 live probe가 필요합니다.
- Modifier 값 `0`은 descriptor가 명시적으로 선언할 때만 DRM LINEAR로
  취급하고, 누락된 검증을 대신하는 지름길로 사용하지 않습니다.
- 엄격한 zero-copy는 CPU raw-pixel 복사를 조용히 수행하면 안 됩니다.
- 정리는 기록된 세션 PID/프로세스 그룹과 파일 디스크립터로 제한합니다.

## 검사

변경을 열기 전에 다음을 실행합니다.

```sh
node --check module/webroot/app.js
node --check module/webroot/locales.js
cmake -S src/video_bridge -B build/video-host
cmake --build build/video-host
ctest --test-dir build/video-host --output-on-failure
```

변경한 구성요소와 관련된 현재 shell fixture를 실행합니다. Release ZIP은
`scripts/verify-v3-module.ps1`도 통과해야 합니다.

하드웨어 검증 보고서는 고유 기기 serial을 공개하지 않고 기기 계열과 Android
버전을 식별해야 합니다. 오류 재현에 필요한 가장 작은 정리된 로그만 첨부합니다.

공개 사용자 또는 릴리스 문서 변경은 같은 변경에서 영문과 한국어 대응 페이지를
함께 갱신해야 합니다. 정확한 식별자와 명령 구문은 영문을 기준으로 합니다.
