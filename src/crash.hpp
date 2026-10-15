#pragma once
// Crash reporter: if the process dies from an unhandled exception (access violation, ...), write
// %APPDATA%\IslandCapsule\crash.log (code, address, module + RVA of every stack frame, version) and a small
// crash.dmp next to it, then let Windows terminate the process as usual.
namespace island {
void InstallCrashHandler();
}
