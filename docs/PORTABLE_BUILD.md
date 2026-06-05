# Portable Build Notes

이 프로젝트는 소스 폴더 이름이나 위치가 바뀌어도 다시 빌드할 수 있게
`CMakePresets.json` 기준으로 구성되어 있습니다.

중요한 점:

- `CMakeLists.txt`, `CMakePresets.json`, `apps`, `include`, `src`, `osal`,
  `oshw`, `cmake` 같은 소스 파일은 이동/복사해도 됩니다.
- `build` 폴더 안의 `.sln`, `.vcxproj`, `CMakeCache.txt`는 생성 파일입니다.
  CMake/Visual Studio가 절대 경로를 포함해서 만들기 때문에 다른 폴더로
  복사한 뒤 그대로 열면 원본 경로를 참조할 수 있습니다.
- 폴더명을 바꿨거나 다른 위치로 복사했다면 `build`를 재생성하십시오.

## 권장 사용법

복사한 새 폴더에서 PowerShell을 열고 실행합니다.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build_vs2022.ps1 -Clean
```

또는 더블 클릭/명령 프롬프트용:

```cmd
scripts\build_vs2022.bat -Clean
```

생성되는 파일:

```text
build\windows-msvc-vs2022\SOEM.sln
build\windows-msvc-vs2022\bin\ethercat_gui.exe
build\windows-msvc-vs2022\bin\lms_master.exe
build\windows-msvc-vs2022\ethercat_sdk\include\ethercat_master.h
build\windows-msvc-vs2022\ethercat_sdk\lib\ethercat_dll.lib
build\windows-msvc-vs2022\ethercat_sdk\bin\ethercat_dll.dll
```

VS2022에서 직접 열고 싶으면 아래 솔루션을 여십시오.

```text
build\windows-msvc-vs2022\SOEM.sln
```

솔루션까지 자동으로 열고 싶으면:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build_vs2022.ps1 -Clean -OpenSolution
```

## Test2 폴더 예시

예를 들어 아래처럼 복사했다면:

```text
E:\001_Project\081_EtherCAT_Master\2_Source\2_GUI\EtherCAT_master_Test2
```

다음 파일을 열지 마십시오.

```text
build\samples\ec_sample\ec_sample.vcxproj
```

이 파일은 복사 전 원본 경로를 포함할 수 있습니다. 대신 새 폴더에서:

```powershell
cd E:\001_Project\081_EtherCAT_Master\2_Source\2_GUI\EtherCAT_master_Test2
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build_vs2022.ps1 -Clean -OpenSolution
```

이렇게 하면 `EtherCAT_master_Test2` 위치 기준으로 `.sln/.vcxproj`가 새로
생성됩니다.
