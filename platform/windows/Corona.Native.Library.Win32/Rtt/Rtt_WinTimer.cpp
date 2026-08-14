//////////////////////////////////////////////////////////////////////////////
//
// This file is part of the Corona game engine.
// For overview and more information on licensing please refer to README.md
// Home page: https://github.com/coronalabs/corona
// Contact: support@coronalabs.com
//
//////////////////////////////////////////////////////////////////////////////
//
// MODIFIED: High-resolution frame pacing experiment
//
// Summary of changes vs. stock Solar2D:
//
// 1. TIMING PRECISION: Replaced GetTickCount() (~15.6ms resolution) with
//    QueryPerformanceCounter() (~0.1us resolution) for frame deadline checks.
//
// 2. FRACTIONAL INTERVAL: The runtime passes interval = 1000/FPS as integer ms
//    (60 FPS -> 16ms, losing 0.667ms per frame). We recover the correct interval
//    by detecting known FPS values and computing the exact QPC tick count.
//
// 3. POLLING RATE: Reduced SetTimer() polling from 10ms to 1ms, combined with
//    timeBeginPeriod(1) so the OS timer interrupt fires at ~1ms intervals.
//    This lets us hit frame deadlines with <2ms latency instead of <10ms.
//
// 4. ABSOLUTE DEADLINES: Uses nextDeadline += interval (accumulated from start)
//    rather than nextDeadline = now + interval, so scheduling jitter doesn't
//    cause long-term clock drift.
//
// 5. DIAGNOSTICS: Optional frame-pacing diagnostics (enabled via SOLAR2D_FRAME_DIAG=1
//    environment variable) that record QPC timestamps and output statistics + CSV
//    on shutdown for analysis.
//
// The stock GetTickCount()/SetTimer(10ms) path is preserved and can be activated
// by setting environment variable SOLAR2D_STOCK_TIMER=1.
//
// Threading: No changes to the threading model. All callbacks still execute on
// the main/window message pump thread via WM_TIMER messages.
//
//////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "Rtt_WinTimer.h"
#include <windows.h>
#include <timeapi.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <algorithm>
#include <string>

// Auto-link winmm.lib for timeBeginPeriod/timeEndPeriod
#pragma comment(lib, "winmm.lib")


namespace Rtt
{

// ============================================================================
// Frame Diagnostics Implementation
// ============================================================================

struct FrameDiagnostics
{
	static const size_t kMaxSamples = 10000;

	int64_t timestamps[kMaxSamples];
	size_t count;
	LARGE_INTEGER qpcFrequency;
	bool enabled;

	FrameDiagnostics()
		: count(0)
		, enabled(false)
	{
		::QueryPerformanceFrequency(&qpcFrequency);
		memset(timestamps, 0, sizeof(timestamps));

		// Enable diagnostics if SOLAR2D_FRAME_DIAG=1
		char buf[16] = {};
		DWORD len = ::GetEnvironmentVariableA("SOLAR2D_FRAME_DIAG", buf, sizeof(buf));
		if (len > 0)
		{
			enabled = (buf[0] == '1' || buf[0] == 'y' || buf[0] == 'Y');
		}
	}

	void RecordFrame()
	{
		if (!enabled || count >= kMaxSamples) return;

		LARGE_INTEGER now;
		::QueryPerformanceCounter(&now);
		timestamps[count++] = now.QuadPart;
	}

	void DumpResults()
	{
		if (!enabled || count < 2) return;

		double freq = (double)qpcFrequency.QuadPart;

		// Calculate intervals in milliseconds
		size_t numIntervals = count - 1;
		std::vector<double> intervals;
		intervals.reserve(numIntervals);
		for (size_t i = 1; i < count; i++)
		{
			double delta = (double)(timestamps[i] - timestamps[i - 1]) / freq * 1000.0;
			intervals.push_back(delta);
		}

		// Compute statistics
		std::vector<double> sorted = intervals;
		std::sort(sorted.begin(), sorted.end());

		double sum = 0.0, sumSq = 0.0;
		for (double d : intervals)
		{
			sum += d;
			sumSq += d * d;
		}
		double mean = sum / numIntervals;
		double variance = (sumSq / numIntervals) - (mean * mean);
		double stddev = std::sqrt(variance > 0.0 ? variance : 0.0);
		double median = sorted[numIntervals / 2];
		double p95 = sorted[(size_t)(numIntervals * 0.95)];
		double p99 = sorted[(size_t)(numIntervals * 0.99)];
		double minVal = sorted.front();
		double maxVal = sorted.back();

		// Count outliers: intervals that deviate >20% from expected 16.6667ms
		int outlierCount = 0;
		const double expectedInterval = 1000.0 / 60.0;  // 16.6667ms
		const double outlierThreshold = expectedInterval * 0.20;  // 3.333ms
		for (double d : intervals)
		{
			if (std::fabs(d - expectedInterval) > outlierThreshold)
			{
				outlierCount++;
			}
		}

		// Determine output directory
		char pathBuf[MAX_PATH] = {};
		::GetEnvironmentVariableA("SOLAR2D_DIAG_PATH", pathBuf, sizeof(pathBuf));
		std::string basePath;
		if (pathBuf[0] != '\0')
		{
			basePath = pathBuf;
		}
		else
		{
			// Default: current working directory
			char cwd[MAX_PATH] = {};
			::GetCurrentDirectoryA(MAX_PATH, cwd);
			basePath = cwd;
		}

		// Write raw intervals to CSV
		{
			std::string csvPath = basePath + "\\frame_intervals.csv";
			FILE* f = nullptr;
			fopen_s(&f, csvPath.c_str(), "w");
			if (f)
			{
				fprintf(f, "frame,interval_ms\n");
				for (size_t i = 0; i < numIntervals; i++)
				{
					fprintf(f, "%zu,%.6f\n", i + 1, intervals[i]);
				}
				fclose(f);
				::OutputDebugStringA(("Solar2D Diag: Wrote frame intervals to " + csvPath + "\n").c_str());
			}
		}

		// Write human-readable summary
		{
			std::string summaryPath = basePath + "\\frame_pacing_summary.txt";
			FILE* f = nullptr;
			fopen_s(&f, summaryPath.c_str(), "w");
			if (f)
			{
				fprintf(f, "Solar2D Frame Pacing Diagnostics\n");
				fprintf(f, "================================\n\n");
				fprintf(f, "Frames recorded:  %zu\n", count);
				fprintf(f, "Intervals:        %zu\n\n", numIntervals);
				fprintf(f, "Expected interval (60 FPS): %.4f ms\n\n", expectedInterval);
				fprintf(f, "Mean:    %.4f ms  (%.1f FPS)\n", mean, 1000.0 / mean);
				fprintf(f, "Median:  %.4f ms\n", median);
				fprintf(f, "Stddev:  %.4f ms\n", stddev);
				fprintf(f, "Min:     %.4f ms\n", minVal);
				fprintf(f, "Max:     %.4f ms\n", maxVal);
				fprintf(f, "P95:     %.4f ms\n", p95);
				fprintf(f, "P99:     %.4f ms\n\n", p99);
				fprintf(f, "Outliers (>20%% off %.4f ms): %d / %zu (%.1f%%)\n",
						expectedInterval, outlierCount, numIntervals,
						100.0 * outlierCount / numIntervals);
				fprintf(f, "\nQPC frequency: %lld Hz (%.1f MHz)\n",
						(long long)qpcFrequency.QuadPart,
						(double)qpcFrequency.QuadPart / 1e6);
				fclose(f);
				::OutputDebugStringA(("Solar2D Diag: Wrote summary to " + summaryPath + "\n").c_str());
			}
		}

		// Also output key stats to debug output for convenience
		{
			char msg[512];
			sprintf_s(msg, sizeof(msg),
				"Solar2D Frame Pacing: %zu frames, mean=%.3fms, stddev=%.3fms, "
				"min=%.3fms, max=%.3fms, p99=%.3fms, outliers=%d (%.1f%%)\n",
				count, mean, stddev, minVal, maxVal, p99,
				outlierCount, 100.0 * outlierCount / numIntervals);
			::OutputDebugStringA(msg);
		}
	}
};


// ============================================================================
// WinTimer Implementation
// ============================================================================

std::unordered_map<UINT_PTR, Rtt::WinTimer *> WinTimer::sTimerMap;
UINT_PTR WinTimer::sMostRecentTimerID;

#pragma region Constructors/Destructors
WinTimer::WinTimer(MCallback& callback, HWND windowHandle)
:	PlatformTimer(callback)
{
	fWindowHandle = windowHandle;
	fTimerPointer = NULL;
	fIntervalInMilliseconds = 10;
	fNextIntervalTimeInTicks = 0;

	// Initialize QPC
	::QueryPerformanceFrequency(&fQpcFrequency);
	fIntervalQpc = 0.0;
	fNextDeadlineQpc = 0.0;

	// Default to high-res timer unless SOLAR2D_STOCK_TIMER=1
	fUseHiResTimer = true;
	{
		char buf[16] = {};
		DWORD len = ::GetEnvironmentVariableA("SOLAR2D_STOCK_TIMER", buf, sizeof(buf));
		if (len > 0 && (buf[0] == '1' || buf[0] == 'y' || buf[0] == 'Y'))
		{
			fUseHiResTimer = false;
			::OutputDebugStringA("Solar2D: Using STOCK timer (GetTickCount + SetTimer 10ms)\n");
		}
		else
		{
			::OutputDebugStringA("Solar2D: Using HIGH-RES timer (QPC + SetTimer 1ms + timeBeginPeriod)\n");
		}
	}

	// Create diagnostics (records frame timestamps if SOLAR2D_FRAME_DIAG=1)
	fDiagnostics = new FrameDiagnostics();
}

WinTimer::~WinTimer()
{
	Stop();

	delete fDiagnostics;
	fDiagnostics = nullptr;
}

#pragma endregion


#pragma region Public Methods
void WinTimer::Start()
{
	// Do not continue if the timer is already running.
	if (IsRunning())
	{
		return;
	}

	fTimerID = ++sMostRecentTimerID;  // ID should be non-0, so pre-increment for first time

	if (fUseHiResTimer)
	{
		// Request 1ms system timer resolution.
		// This affects the granularity of SetTimer(), Sleep(), and other Windows timing functions.
		// Standard practice for games/media applications.
		::timeBeginPeriod(1);

		// Set the initial absolute deadline using QPC.
		LARGE_INTEGER now;
		::QueryPerformanceCounter(&now);
		fNextDeadlineQpc = (double)now.QuadPart + fIntervalQpc;

		// Schedule a 1ms Windows timer for polling.
		// With timeBeginPeriod(1), this fires every ~1-2ms, giving us <2ms latency
		// to hit our QPC-computed deadline (vs. ~10-15ms with the stock 10ms timer).
		fTimerPointer = ::SetTimer(fWindowHandle, fTimerID, 1, WinTimer::OnTimerElapsed);
	}
	else
	{
		// Stock Solar2D behavior: 10ms SetTimer polling + GetTickCount comparison
		fNextIntervalTimeInTicks = (S32)::GetTickCount() + (S32)fIntervalInMilliseconds;
		fTimerPointer = ::SetTimer(fWindowHandle, fTimerID, 10, WinTimer::OnTimerElapsed);
	}

	if (IsRunning())
	{
		sTimerMap[fTimerID] = this;
	}
}

void WinTimer::Stop()
{
	// Do not continue if the timer has already been stopped.
	if (IsRunning() == false)
	{
		return;
	}

	// Stop the timer.
	::KillTimer(fWindowHandle, fTimerID);

	sTimerMap.erase(fTimerID);

	fTimerPointer = NULL;
	fTimerID = 0;

	if (fUseHiResTimer)
	{
		// Restore default system timer resolution.
		::timeEndPeriod(1);
	}

	// Dump frame diagnostics on shutdown (writes CSV + summary if enabled)
	if (fDiagnostics)
	{
		fDiagnostics->DumpResults();
	}
}

void WinTimer::SetInterval(U32 milliseconds)
{
	fIntervalInMilliseconds = milliseconds;

	// Recover fractional precision lost by integer division in the caller.
	//
	// The Solar2D runtime computes:  interval = 1000 / FPS  (integer division)
	//   60 FPS  ->  1000 / 60  =  16  (actual: 16.6667ms, 0.667ms lost per frame)
	//   30 FPS  ->  1000 / 30  =  33  (actual: 33.3333ms, 0.333ms lost per frame)
	//
	// The 60 FPS truncation means the stock timer targets 62.5 FPS instead of 60.
	// Over one second, frames arrive 40ms early (60 * 0.667ms).
	//
	// We detect the intended FPS from the truncated integer and compute the
	// exact interval in QPC ticks using floating-point arithmetic.

	double actualMs;
	if (milliseconds == 16)
	{
		actualMs = 1000.0 / 60.0;   // 16.66666... ms
	}
	else if (milliseconds == 33)
	{
		actualMs = 1000.0 / 30.0;   // 33.33333... ms
	}
	else
	{
		actualMs = (double)milliseconds;
	}

	fIntervalQpc = actualMs * (double)fQpcFrequency.QuadPart / 1000.0;
}

bool WinTimer::IsRunning() const
{
	return (fTimerPointer != NULL);
}

void WinTimer::Evaluate()
{
	// Do not continue if the timer is not running.
	if (IsRunning() == false)
	{
		return;
	}

	if (fUseHiResTimer)
	{
		// --- High-resolution path ---
		LARGE_INTEGER now;
		::QueryPerformanceCounter(&now);
		int64_t nowQpc = now.QuadPart;

		// Not yet time for the next frame
		if ((double)nowQpc < fNextDeadlineQpc)
		{
			return;
		}

		// Advance deadline past the current time.
		// If we're late, this skips to the next valid deadline rather than
		// trying to "catch up" with rapid successive frames.
		// Uses absolute accumulation: deadline += interval (not deadline = now + interval)
		// so scheduling jitter doesn't cause long-term drift.
		while (fNextDeadlineQpc <= (double)nowQpc)
		{
			fNextDeadlineQpc += fIntervalQpc;
		}

		// Record frame timestamp for diagnostics (before callback, to measure scheduling)
		if (fDiagnostics)
		{
			fDiagnostics->RecordFrame();
		}

		// Invoke the runtime's frame callback (same thread as stock path)
		this->operator()();
	}
	else
	{
		// --- Stock Solar2D path (unchanged) ---
		// Do not continue if we haven't reached the scheduled time yet.
		if (CompareTicks((S32)::GetTickCount(), fNextIntervalTimeInTicks) < 0)
		{
			return;
		}

		// Schedule the next interval time.
		for (; CompareTicks((S32)::GetTickCount(), fNextIntervalTimeInTicks) > 0; fNextIntervalTimeInTicks += fIntervalInMilliseconds);

		// Invoke this timer's callback.
		this->operator()();
	}
}

#pragma endregion


#pragma region Private Methods/Functions
VOID CALLBACK WinTimer::OnTimerElapsed(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime)
{
	auto timer = sTimerMap.find(idEvent);
	if (sTimerMap.end() != timer)
	{
		timer->second->Evaluate();
	}
}

S32 WinTimer::CompareTicks(S32 x, S32 y)
{
	// Overflow will occur when flipping sign bit of largest negative number.
	// Give it a one millisecond boost before flipping the sign.
	if (0x80000000 == y)
	{
		y++;
	}

	// Compare the given tick values via subtraction. Overlow for this subtraction operation is okay.
	S32 deltaTime = x - y;
	if (deltaTime < 0)
	{
		return -1;
	}
	else if (0 == deltaTime)
	{
		return 0;
	}
	return 1;
}

#pragma endregion

}	// namespace Rtt
