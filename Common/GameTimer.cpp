#include <Windows.h>
#include "GameTimer.h"

GameTimer::GameTimer()
	: mSecondsPerCount(0.0),
	  mDeltaTime(-1.0),
	  mBaseTime(0),
	  mPausedTime(0),
	  mPrevTime(0),
	  mCurrTime(0),
	  mStopped(false)
{
	__int64 countsPerSec;
	QueryPerformanceFrequency((LARGE_INTEGER*)&countsPerSec); // get the frequency (counts per second) of the performance timer, which is a fixed value at system boot
	mSecondsPerCount = 1.0 / (double)countsPerSec;            // seconds per count is used to compute the actual time in seconds given the counts
}

// Returns the total time elapsed since Reset() was called, NOT counting any
// time when the clock is stopped.
float GameTimer::TotalTime() const
{
	// if we previously already had a pause, the distance 
	// mStopTime - mBaseTime includes paused time, which we do not want to count.
	// To correct this, we can subtract the paused time from mStopTime:  

	if (mStopped)
	{
		//? Why we do need this? 
		return (float)(((mStopTime - mPausedTime) - mBaseTime) * mSecondsPerCount);
	}

	// The distance mCurrTime - mBaseTime includes paused time,
	// which we do not want to count.  To correct this, we can subtract 
	// the paused time from mCurrTime:  
	return (float)(((mCurrTime - mPausedTime) - mBaseTime) * mSecondsPerCount);
}

float GameTimer::DeltaTime() const
{
	return (float)mDeltaTime;
}

void GameTimer::Reset()
{
	__int64 currTime;
	QueryPerformanceCounter((LARGE_INTEGER*)&currTime); // retrieve the current time value in counts

	//! initialize mBaseTime to the current time, which is considered as the time when the application started
	mBaseTime = currTime;
	//! It is important to intialize mPrevTime to the current time before the message loops starts, because for the first frame of animation, there is no previous time stamp. 
	mPrevTime = currTime;
	mStopTime = 0;
	mStopped  = false;
}

void GameTimer::Start()
{
	__int64 startTime;
	QueryPerformanceCounter((LARGE_INTEGER*)&startTime); // retrieve the current time value in counts


	// Accumulate the time elapsed between stop and start pairs.
	//
	//                     |<-------d------->|
	// ----*---------------*-----------------*------------> time
	//  mBaseTime       mStopTime        startTime     

	// if we are resuming the timer from a stopped state
	if (mStopped)
	{
		// then accumulate the paused time
		mPausedTime += (startTime - mStopTime);

		// since we are starting the timer back up,
		// the current previous time is not valid, as it occurred while paused.
		// so reset it to the current time. 
		mPrevTime = startTime;

		// no longer stopped
		mStopTime = 0;
		mStopped  = false;
	}
}

void GameTimer::Stop()
{
	// if we are already stopped, then don't do anything
	if (!mStopped)
	{
		__int64 currTime;
		QueryPerformanceCounter((LARGE_INTEGER*)&currTime); // retrieve the current time value in counts

		// otherwise, save the time we stopped at, and
		// set the bool flag indicating the timer is stopped
		mStopTime = currTime;
		mStopped  = true;
	}
}

void GameTimer::Tick()
{
	if (mStopped)
	{
		// if the timer is stopped, there is no delta time
		mDeltaTime = 0.0;
		return;
	}

	// get the time this frame
	__int64 currTime;
	QueryPerformanceCounter((LARGE_INTEGER*)&currTime); // retrieve the current time value in counts
	mCurrTime = currTime;

	// Time difference between this frame and the previous.
	mDeltaTime = (mCurrTime - mPrevTime) * mSecondsPerCount;

	// Prepare for next frame.
	mPrevTime = mCurrTime;

	// Force nonnegative.  The DXSDK's CDXUTTimer mentions that if the 
	// processor goes into a power save mode or we get shuffled to another
	// processor, then mDeltaTime can be negative.
	if (mDeltaTime < 0.0)
	{
		mDeltaTime = 0.0;
	}
}
