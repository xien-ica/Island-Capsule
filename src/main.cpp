// island-capsule DLL entry

#include <windows.h>

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
  switch (reason) {
    case DLL_PROCESS_ATTACH:
      DisableThreadLibraryCalls(hModule);
      break;
    case DLL_PROCESS_DETACH:
      break;
  }
  return TRUE;
}
