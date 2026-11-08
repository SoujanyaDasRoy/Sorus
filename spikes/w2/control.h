// Spike W2 (throwaway): settings block shared between a normal user app and the APO inside audiodg.exe.
// The APO creates it (services may create Global\ objects; standard users may not) with a DACL that lets
// interactive users read and write it. Writer: set gain_db and written_qpc, then InterlockedIncrement(seq).
// The APO applies a new seq on its next APOProcess call and stamps applied_seq / applied_qpc.
#pragma once
#include <windows.h>

#define SORUS_W2_SECTION L"Global\\SorusW2Control"
constexpr DWORD kSorusW2Magic = 0x32575353;  // "SSW2"

struct SorusW2Control {
  DWORD magic;
  volatile LONG seq;
  float gain_db;
  LONGLONG written_qpc;
  volatile LONG applied_seq;
  volatile LONGLONG applied_qpc;
  volatile LONG apo_instances;  // APOs that have mapped the block
};
