# 릴리스 패키징 및 공개

[English](../RELEASING.md) | [한국어](RELEASING.md)

이 문서는 릴리스 담당자 체크리스트입니다. 로컬에서 ZIP을 만들었다고 바로
릴리스가 되는 것은 아닙니다. 정확한 소스 revision, 패키지 verifier, 기기
매트릭스, 문서, 공개 checksum이 모두 일치해야 합니다.

## 공개 소스 경계

공개 저장소에는 다음이 있어야 합니다.

- 루트 프로젝트, 출처, 보안, 기여 문서와 영문/한국어 사용자 문서
- `.github/workflows/verify.yml`, `.gitattributes`, `.gitignore`
- `src/` 아래 검토된 소스
- `module/` 아래 배포 모듈 트리
- `scripts/` 아래 릴리스, 검증, 테스트, 과거 provider builder
- checksum manifest가 허용하는 위치의 정확한 prebuilt 런타임 파일

Rootfs, 검토하지 않은 런타임/provider 압축 파일, build/dist/work 트리, 로컬
바이너리, 기기 또는 ADB 로그, 인증정보, cookie/token, 기기 serial, 비공개/유료
외형 자산, 과거 컴포지터 실험은 제외해야 합니다. Source lock에 명시된 checksum
고정 Archcraft Sway 공개 자산 압축 파일은 추적하지 않은 다운로드가 아니라 검토된
릴리스 입력입니다. [문서 인덱스](README.md)의 대응 페이지가 공개 v3 문서
범위이며, ignore된 과거 기록은 릴리스 일부가 아닙니다.

## 검토된 빌드 입력

- 검토된 LinDeX 소스 revision
- av2xn/Magisk-Debian-Chroot에서 파생된 canonical rootfs carrier
- 고정된 KGSL Mesa URL과 SHA-256 값
- `module/advc-artifacts.sha256`과 일치하는 Android 코덱 바이너리
- checksum manifest와 일치하는 브리지 payload
- `module/profile-assets/SOURCES.lock` 및
  `module/profile-assets/APPEARANCE_SOURCES.lock`과 일치하는 Archcraft 공식 공개
  Sway source archive와 LinDeX 제작 appearance aggregate

두 번째 외형 파일은 LinDeX가 만든 aggregate
`lindex-archcraft-sway-public-assets-v2.tar.gz`, SHA-256
`4b84564c692e270bb46bbc36c4e5f9b1684c5ed4f1d8bcbf053698780a0af08c`입니다.
Archcraft 업스트림 배포 archive가 아닙니다. 이를 다시 만들거나 교체할 때는
실제로 포함된 Qogirr 대응 SVG 소스와 `COPYING` 파일을 포함해 appearance
lock의 모든 공개 source path와 commit을 검토해야 합니다.

비공개/유료 Ko-fi 압축 파일, 인증정보, 전체 기기 로그, 기기 serial, 로컬 절대
경로를 소스나 릴리스 첨부 파일에 넣지 않습니다. Archcraft Ko-fi 페이지는 제작자
선택 후원용이며 설치 소스, 필수 결제, 라이선스 조건이 아닙니다.

## 패키징 및 검증

Release와 dev ZIP을 별도로 빌드합니다.

```powershell
./scripts/package-v3-module.ps1 -BaseModuleZip C:\path\to\base.zip -Flavor release
./scripts/package-v3-module.ps1 -BaseModuleZip C:\path\to\base.zip -Flavor dev

pwsh ./scripts/verify-v3-module.ps1 -ModuleZip ./dist/LinDeX-v3.0.4.zip -ExpectedFlavor release
pwsh ./scripts/verify-v3-module.ps1 -ModuleZip ./dist/LinDeX-v3.0.4-dev.zip -ExpectedFlavor dev
```

검증기는 PowerShell 7 이상이 필요합니다. Windows PowerShell 5.1은 릴리스
검증 환경으로 지원하지 않습니다.

Verifier는 모듈 identity, rootfs hash와 크기, 필수 bridge/codec/profile 파일,
3개 프로필 계약, 공식 Sway 자산 source lock과 checksum, 컴포지터 패치 payload
없음, `system/` 없음, 개발 전용 진단 실행 파일 제외, 선택한 logging flavor를
확인해야 합니다.

Release 모드는 영구 설정/세션/코덱 로그를 만들지 않고 오래된 동일 로그도
제거해야 합니다. Dev 모드는 크기가 제한된 순환 로그만 보존할 수 있습니다.
Artifact checksum은 검증 후 생성합니다. 과거 또는 저장소 단위 checksum 파일을
새 ZIP 설명처럼 공개하지 않습니다.

## 기기 검수

1. DP가 물리적으로 분리되었는지 확인합니다.
2. 정확한 release ZIP을 KernelSU로 설치합니다.
3. Android를 일반 재부팅해 root 모듈을 활성화합니다.
4. Git commit과 release ZIP SHA-256을 기록합니다.
5. Rootfs 생성/업데이트, WebUI 상태, Mesa, chroot PolicyKit 에이전트 제외 계약,
   3개 프로필과 Sway Dark/Light/Pywal 선택을 검증합니다.
6. 각 프로필에서 시작, 중지, 강제 분리, 재연결을 실행합니다.
7. 물리 connector/EDID gating과 세션/프로세스 그룹 정리를 검증합니다.
8. Release no-log 정책을 검증하고 dev ZIP에서 제한된 로그를 반복 검증합니다.
9. 명시적 `modifier = 0` LINEAR 검증과 fail-closed 디코드 PRIME을 포함한 코덱
   행을 완료합니다.
10. USB 독 매트릭스와 직접 스캔아웃 증거 행을 완료합니다.

결과를 [검증 상태](VALIDATION_STATUS.md)에 기록합니다. 격리 프로브는 그 사실을
표시하며 패키지 모듈 검수를 대체하지 않습니다.

## 공개 전 체크리스트

### 저장소 및 문서

- [x] 영문 `README.md`와 한국어 `README.ko.md`를 분리하고 상호 연결함.
- [x] 공개 사용자/릴리스 문서에 EN/KO 대응 페이지와 인덱스가 있음.
- [x] 프로젝트 계보, MIT 고지, 제3자 소유권, Archcraft/Ko-fi 경계가 명확함.
- [x] 공개 문서가 수정하지 않은 컴포지터 브리지를 설명하고 과거 컴포지터
  실험을 릴리스 경로로 제시하지 않음.
- [x] 공개 문서가 개발 전용 진단 실행 파일 배포를 제외함.
- [x] `.gitignore`가 로그, secret, 빌드 트리, 다운로드 압축 파일, 과거 실험,
  비공개 작업 기록을 제외함.
- [ ] 정확한 태그에서 로컬 링크, ignored file, secret scan 재실행.
- [ ] 저장소가 생성되면 GitHub 저장소 URL, issue/security 연락 경로,
  release URL 확인.

### 빌드 및 하드웨어

- [ ] 정확한 tag commit에서 CI와 모든 패키지 verifier 실행.
- [x] 두 flavor를 재현 가능하게 빌드하고 SHA-256 기록.
- [ ] DP 분리 상태에서 정확한 release ZIP을 설치하고 Android 일반 재부팅.
- [ ] 3개 프로필 최종 기기 매트릭스 완료.
- [x] 기준 기기에서 제한된 패키지 모듈 앱 인코드 측정 완료.
- [ ] Modifier `0` LINEAR 디코드/import 검증 완료.
- [ ] 모든 개별 게이트를 통과하지 않으면 디코드 PRIME이 미광고인지 확인.
- [ ] Release/dev 로그, USB 독, hotplug, 직접 스캔아웃 게이트 완료.

### 공개

- [x] `VERSION`, changelog 날짜, 검증 날짜, 산출물 이름, hash를 함께 갱신.
- [ ] Rootfs 포함 release ZIP, 의도적으로 공개할 때만 dev ZIP, checksum, 소스
  압축 파일, 필요한 제3자 소스 제공물을 첨부.
- [ ] 첨부 파일을 다시 다운로드해 hash 검증 후 발표.
- [ ] 남은 pending 행을 그대로 표시하고 증거를 포괄적인 “production ready”
  문구로 대체하지 않음.
