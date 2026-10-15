#pragma once

namespace stratakv::crash {

// Deterministic crash injection for testing recovery.
//
// Random SIGKILLs find bugs probabilistically; named crash points hit the
// dangerous windows on purpose. Each one sits between two durability steps, so
// a test can arm exactly the moment it wants to interrupt. Using both is the
// strongest answer to "how do you know recovery works?"
//
// Armed by the environment variable STRATAKV_CRASH_AT=<name>, read once on
// first use.

// True when `name` is the armed crash point.
bool IsArmed(const char* name);

// Ends the process immediately if `name` is armed: no destructors, no buffer
// flushing, like a SIGKILL.
void CrashPoint(const char* name);

// Test-only. Overrides the environment variable within this process so the
// matching logic can be tested without killing the test runner. The string must
// outlive the override; pass nullptr to clear it.
void SetArmedPointForTesting(const char* name);

}  // namespace stratakv::crash
