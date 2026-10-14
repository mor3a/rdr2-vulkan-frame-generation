#pragma once
namespace fgvk { void LogInit(); void Log(const char* fmt, ...); void LogDebug(const char* fmt, ...); void LogModules(); void SetDebugLogging(bool); bool DebugLoggingEnabled(); void StartDiagnosticCollector(); void RequestDiagnosticStacks(); void ArmDiagnosticDebugger(); }
