// kernel_ids.c: prints what the running kernel reports about itself, the way a JVM reads it:
// KUSER_SHARED_DATA.NtBuildNumber, SystemBuildVersionInformation (class 222) layer 0, and the file
// version of ntoskrnl.exe (and ntkrla57.exe), plus whether 5-level paging is on (CR4.LA57 is not
// readable from user mode: the highest user address tells), and on x86 whether the system enables AVX.
#include <windows.h>
#include <stdio.h>
#include <string.h>

typedef LONG (NTAPI* NtQSIEx)(ULONG, PVOID, ULONG, PVOID, ULONG, PULONG);

static void filever(const char* path) {
  DWORD h = 0, n = GetFileVersionInfoSizeA(path, &h);
  char buf[8192];
  char root[2] = {92, 0};
  VS_FIXEDFILEINFO* fi;
  UINT len;
  if (n == 0 || n > sizeof(buf) || !GetFileVersionInfoA(path, 0, n, buf) || !VerQueryValueA(buf, root, (void**)&fi, &len)) {
    printf("KVER file %s: no version (error %lu)\n", path, GetLastError());
    return;
  }
  printf("KVER file %s: %u.%u.%u.%u\n", path, HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS),
         HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
}

int main(void) {
  struct { USHORT LayerNumber, LayerCount; ULONG Major, Minor, Build, Qfe; UCHAR rest[560]; } info;
  ULONG layer = 0, ret = 0;
  SYSTEM_INFO si;
  char sys[MAX_PATH], path[MAX_PATH];
  NtQSIEx q = (NtQSIEx)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQuerySystemInformationEx");
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  printf("KVER kuser NtBuildNumber %lu\n", *(volatile ULONG*)(ULONG_PTR)(0x7FFE0000 + 0x260) & 0xFFFF);
  if (q != NULL) {
    LONG st;
    memset(&info, 0, sizeof(info));
    st = q(222, &layer, sizeof(layer), &info, sizeof(info), &ret);
    printf("KVER class222 status 0x%08lx returned %lu layer %u of %u version %lu.%lu.%lu.%lu\n", (ULONG)st, ret,
           info.LayerNumber, info.LayerCount, info.Major, info.Minor, info.Build, info.Qfe);
  } else {
    printf("KVER class222 no NtQuerySystemInformationEx\n");
  }
  GetSystemDirectoryA(sys, sizeof(sys));
  sprintf(path, "%s%cntoskrnl.exe", sys, 92);
  filever(path);
  sprintf(path, "%s%cntkrla57.exe", sys, 92);
  if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) filever(path);
  GetNativeSystemInfo(&si);
  printf("KVER arch %u processors %lu max user address %p\n", si.wProcessorArchitecture, si.dwNumberOfProcessors,
         si.lpMaximumApplicationAddress);
  return 0;
}
